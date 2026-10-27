"""CPU-only tests: compile the real executable and a public-header OpenVR boundary.

No test loads SteamVR. Unused virtual methods abort, so unexpected side effects
fail the subprocess rather than touching hardware. Only Python stdlib and g++.
"""
import json
import os
import re
import struct
import subprocess
import tempfile
import time
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def input_components(side):
    buttons = ['system', 'bumper', 'trigger', 'grip', 'thumbstick']
    buttons += ['view', 'dpad_up', 'dpad_right', 'dpad_down', 'dpad_left'] if side == 'left' else ['menu', 'a', 'b', 'x', 'y']
    result = {f'/input/{button}/{kind}': 'boolean' for button in buttons for kind in ('click', 'touch')}
    result['/input/thumbrest/touch'] = 'boolean'
    result.update({f'/input/{part}/value': 'vector1' for part in ('trigger', 'grip')})
    result.update({f'/input/thumbstick/{axis}': 'vector2' for axis in ('x', 'y')})
    return result


def action_name(side, component):
    suffix = component.removeprefix('/input/').replace('/', '_')
    if suffix in ('thumbstick_x', 'thumbstick_y'):
        suffix = 'thumbstick_position'
    return f'/actions/observe/in/{side}_{suffix}'


def png():
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(b'\0\xff\0\0')) + chunk(b'IEND', b''))


def fake_source():
    header = (ROOT / 'vendor/openvr.h').read_text()
    # Derive actual vtable signatures, never guessed ABI layouts. Every method
    # outside the explicit read/capture/settings/debug contract aborts.
    overrides = {
        'SetActionManifestPath': '''if(!pchActionManifestPath || pchActionManifestPath[0]!='/') abort();
            log("manifest"); manifest_set=true;
            return mode("manifest_error")?VRInputError_InvalidParam:VRInputError_None;''',
        'GetActionSetHandle': '''if(!manifest_set || std::string(pchActionSetName)!="/actions/observe") abort();
            *pHandle=90; return mode("set_error")?VRInputError_NameNotFound:VRInputError_None;''',
        'GetActionHandle': '''if(!manifest_set) abort();
            std::string name(pchActionName); if(name.find("/actions/observe/in/")!=0) abort();
            if(mode("missing_action") && name=="/actions/observe/in/left_bumper_click") return VRInputError_NameNotFound;
            for(size_t i=0;i<action_names.size();++i) if(action_names[i]==name){*pHandle=i+1;return VRInputError_None;}
            action_names.push_back(name); *pHandle=action_names.size(); return VRInputError_None;''',
        'GetInputSourceHandle': '''std::string name(pchInputSourcePath);
            if(name!="/user/hand/left" && name!="/user/hand/right") abort();
            *pHandle=name=="/user/hand/left"?104:107;
            return mode("source_error") && *pHandle==107?VRInputError_NameNotFound:VRInputError_None;''',
        'UpdateActionState': '''if(!manifest_set || unSetCount!=1 || unSizeOfVRSelectedActionSet_t!=sizeof(*pSets) ||
                pSets[0].ulActionSet!=90 || pSets[0].ulRestrictedToDevice!=k_ulInvalidInputValueHandle ||
                pSets[0].ulSecondaryActionSet || pSets[0].nPriority) abort();
            ++updates; log("update_actions");
            if(mode("input_hang")) for(;;) std::this_thread::sleep_for(std::chrono::seconds(1));
            if(mode("update_error")) return VRInputError_InvalidParam;
            if(mode("no_data") || (mode("delayed_update") && updates<3)) return VRInputError_NoData;
            return VRInputError_None;''',
        'GetDigitalActionData': '''if(unActionDataSize!=sizeof(*pActionData)) abort();
            const auto &name=check_action(action,ulRestrictToDevice);
            if(name.find("_value")!=std::string::npos || name.find("_position")!=std::string::npos) abort();
            *pActionData={}; pActionData->bActive=input_active(name);
            pActionData->bState=ulRestrictToDevice==104; pActionData->bChanged=true;
            pActionData->activeOrigin=ulRestrictToDevice+1000;
            return mode("digital_error") && name=="/actions/observe/in/left_bumper_click"?VRInputError_WrongType:VRInputError_None;''',
        'GetAnalogActionData': '''if(unActionDataSize!=sizeof(*pActionData)) abort();
            const auto &name=check_action(action,ulRestrictToDevice);
            if(name.find("_value")==std::string::npos && name.find("_position")==std::string::npos) abort();
            *pActionData={}; pActionData->bActive=input_active(name);
            pActionData->activeOrigin=ulRestrictToDevice+1000;
            pActionData->x=ulRestrictToDevice==104?.25f:.75f; pActionData->y=ulRestrictToDevice==104?-.5f:.5f;
            if(mode("nonfinite")) pActionData->x=std::numeric_limits<float>::quiet_NaN();
            return mode("analog_error") && name=="/actions/observe/in/right_trigger_value"?VRInputError_WrongType:VRInputError_None;''',
        'GetOriginTrackedDeviceInfo': '''if(unOriginInfoSize!=sizeof(*pOriginInfo) || (origin!=1104 && origin!=1107)) abort();
            if(mode("origin_error")) return VRInputError_InvalidHandle;
            *pOriginInfo={}; pOriginInfo->devicePath=origin-1000; pOriginInfo->trackedDeviceIndex=origin==1104?4:7;
            strcpy(pOriginInfo->rchRenderModelComponentName,"input"); return VRInputError_None;''',
        'GetDeviceToAbsoluteTrackingPose': '''if(fPredictedSecondsToPhotonsFromNow != 0 ||
                (unTrackedDevicePoseArrayCount != 1 && unTrackedDevicePoseArrayCount != k_unMaxTrackedDeviceCount)) abort();
            log(("poses " + std::to_string(eOrigin) + " " + std::to_string(unTrackedDevicePoseArrayCount)).c_str());
            for(uint32_t i=0; i<unTrackedDevicePoseArrayCount; ++i){
                auto &p=pTrackedDevicePoseArray[i]; p={}; p.bDeviceIsConnected=i==0 || i==2 || i==4;
                p.bPoseIsValid=p.bDeviceIsConnected && !mode("invalid_pose");
                p.eTrackingResult=p.bPoseIsValid?TrackingResult_Running_OK:TrackingResult_Uninitialized;
                p.mDeviceToAbsoluteTracking=matrix(float(eOrigin) + 100*float(i));
                p.vVelocity.v[0]=0.25f+float(i); p.vAngularVelocity.v[2]=float(i);
            }''',
        'IsInputAvailable': 'return !mode("input_unavailable");',
        'GetTrackedDeviceClass': '''if(unDeviceIndex>=k_unMaxTrackedDeviceCount) abort();
            if(unDeviceIndex==0) return TrackedDeviceClass_HMD;
            if(mode("no_controllers")) return TrackedDeviceClass_Invalid;
            if(unDeviceIndex==2 || unDeviceIndex==4 || unDeviceIndex==7 || unDeviceIndex==63)
                return TrackedDeviceClass_Controller;
            return unDeviceIndex==8?TrackedDeviceClass_GenericTracker:TrackedDeviceClass_Invalid;''',
        'GetControllerRoleForTrackedDeviceIndex': '''switch(unDeviceIndex){
            case 2: case 4: return TrackedControllerRole_LeftHand;
            case 7: return TrackedControllerRole_RightHand;
            case 63: return TrackedControllerRole_OptOut;
            default: abort();}''',
        'GetSeatedZeroPoseToStandingAbsoluteTrackingPose': 'return matrix(10);',
        'GetRawZeroPoseToStandingAbsoluteTrackingPose': 'return matrix(20);',
        'GetTrackedDeviceActivityLevel': 'return k_EDeviceActivityLevel_UserInteraction;',
        'GetStringTrackedDeviceProperty': '''if(pError) *pError=TrackedProp_Success;
            const char *s=prop==Prop_ModelNumber_String?"Fake \\\"HMD\\\"":prop==Prop_SerialNumber_String?"TEST-001":"fake_driver";
            if(unDeviceIndex!=0){
                if(unDeviceIndex!=2 && unDeviceIndex!=4 && unDeviceIndex!=7 && unDeviceIndex!=63) abort();
                s=prop==Prop_ModelNumber_String?"Pose controller":prop==Prop_SerialNumber_String?
                    (unDeviceIndex==4?"frame_testbench_left":unDeviceIndex==7?"frame_testbench_right":
                     unDeviceIndex==2?"PHYSICAL-002":"frame_testbench_left_extra"):"frame_testbench";
            }
            if(mode("property_error") || (mode("controller_property_error") && unDeviceIndex==7)){
                if(pError) *pError=TrackedProp_UnknownProperty; return 0;
            }
            uint32_t n=uint32_t(strlen(s)+1); if(unBufferSize<n){if(pError) *pError=TrackedProp_BufferTooSmall; return n;}
            memcpy(pchValue,s,n); return n;''',
        'GetTimeSinceLastVsync': '*pfSecondsSinceLastVsync=0.01f; *pulFrameCounter=12345; return true;',
        'GetFrameTiming': '''if(pTiming->m_nSize!=sizeof(*pTiming)) abort();
            if(mode("no_timing")) return false;
            pTiming->m_nFrameIndex=100+unFramesAgo; pTiming->m_nNumFramePresents=2;
            pTiming->m_flSystemTimeInSeconds=42.5; pTiming->m_HmdPose.bPoseIsValid=true;
            pTiming->m_HmdPose.mDeviceToAbsoluteTracking=matrix(30); return true;''',
        'GetTrackingSpace': 'return TrackingUniverseStanding;',
        'GetBool': '''check_setting(pchSection,pchSettingsKey); log("get_bool");
            if(peError) *peError=mode("read_error")?VRSettingsError_ReadFailed:VRSettingsError_None;
            return mode("mismatch")?true:setting;''',
        'SetBool': '''check_setting(pchSection,pchSettingsKey); log(bValue?"set_true":"set_false");
            if(peError) *peError=mode("write_error")?VRSettingsError_WriteFailed:VRSettingsError_None;
            setting=bValue;''',
        'RequestScreenshot': '''if(type!=VRScreenshotType_Stereo) abort(); log("request_screenshot");
            if(mode("screenshot_error")) return VRScreenshotError_RequestFailed;
            *pOutScreenshotHandle=77;
            // Steam Frame's compositor appends .png to API filename prefixes.
            std::string a=std::string(pchPreviewFilename)+".png",b=std::string(pchVRFilename)+".png";
            worker=std::thread([a,b]{
                auto bytes=image;
                if(mode("corrupt")) bytes[45]^=1;
                std::ofstream f(a,std::ios::binary); f.write((char*)bytes.data(),33); f.flush();
                if(mode("incomplete")) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                f.write((char*)bytes.data()+33,bytes.size()-33); f.close();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                std::ofstream g(b,std::ios::binary); g.write((char*)bytes.data(),bytes.size()); g.close();
                log("pngs_complete");
            }); return VRScreenshotError_None;''',
        'DriverDebugRequest': '''log("debug");
            if(unDeviceIndex!=3 || std::string(pchRequest)!="test request") abort();
            const char *s="response\\n\\\"ok\\\""; auto n=uint32_t(strlen(s)+1);
            if(mode("debug_large")) return unResponseBufferSize+1;
            if(unResponseBufferSize>=n) memcpy(pchResponseBuffer,s,n); return n;''',
    }
    source = '''#include "openvr.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <cstdio>
#include <limits>
using namespace vr;
bool setting=true;
bool manifest_set=false;
unsigned updates=0;
std::vector<std::string> action_names;
std::thread worker;
bool mode(const char *m){const char *v=getenv("FAKE_MODE");return v && std::string(v)==m;}
void log(const char *s){const char *p=getenv("FAKE_LOG");if(p) std::ofstream(p,std::ios::app)<<s<<"\\n";}
const std::string &check_action(VRActionHandle_t action,VRInputValueHandle_t device){
 if(!updates || !action || action>action_names.size()) abort();
 const auto &name=action_names[action-1];
 if(device!=(name.find("/left_")!=std::string::npos?104:107)) abort();
 return name;
}
bool input_active(const std::string &name){
 if(mode("inactive") || name.find("thumbrest")!=std::string::npos || name.find("system")!=std::string::npos) return false;
 if(mode("delayed") && updates<4 && name.find("/right_")!=std::string::npos) return false;
 return true;
}
void check_setting(const char *s,const char *k){if(std::string(s)!="power" || std::string(k)!="pauseCompositorOnStandby") abort();}
HmdMatrix34_t matrix(float x){HmdMatrix34_t m={};for(int i=0;i<3;i++)m.m[i][i]=1;m.m[0][3]=x;return m;}
'''
    source += 'std::vector<unsigned char> image={' + ','.join(str(b) for b in png()) + '};\n'
    for interface in ('IVRSystem', 'IVRCompositor', 'IVRSettings', 'IVRScreenshots', 'IVRDebug', 'IVRInput'):
        match = re.search(r'class\s+' + interface + r'\s*\{(.*?)\n\s*\};', header, re.DOTALL)
        assert match is not None, interface
        body = match.group(1)
        body = re.sub(r'/\*.*?\*/|//[^\n]*', '', body, flags=re.DOTALL)
        methods = re.findall(r'virtual\s+(.*?)\s*=\s*0\s*;', body, re.DOTALL)
        source += f'class Fake{interface}: public {interface} {{ public:\n'
        for method in methods:
            name_match = re.search(r'(\w+)\s*\(', method)
            assert name_match is not None, method
            name = name_match.group(1)
            source += method + ' override {' + overrides.get(name, 'abort();') + '}\n'
        source += f'}} object{interface};\n'
    source += '''extern "C" {
uint32_t VR_InitInternal2(EVRInitError *e,EVRApplicationType t,const char*){
 if(t!=VRApplication_Background) abort(); log("init_background"); printf("sdk noise\\n"); fflush(stdout);
 if(mode("hang")) for(;;) std::this_thread::sleep_for(std::chrono::seconds(1));
 *e=mode("init_error")?VRInitError_Init_HmdNotFound:VRInitError_None;return 1;
}
void VR_ShutdownInternal(){log("shutdown_enter");if(worker.joinable()) worker.join();log("shutdown");printf("shutdown noise\\n");}
bool VR_IsInterfaceVersionValid(const char *v){log(v);return !mode("bad_version");}
void *VR_GetGenericInterface(const char *v,EVRInitError *e){*e=VRInitError_None;
 if(mode("missing_interface")){*e=VRInitError_Init_InterfaceNotFound;return nullptr;}
'''
    for interface in ('IVRSystem', 'IVRCompositor', 'IVRSettings', 'IVRScreenshots', 'IVRDebug', 'IVRInput'):
        source += f'if(std::string(v)=={interface}_Version) return &object{interface};\n'
    return source + '*e=VRInitError_Init_InterfaceNotFound;return nullptr;}\n}\n'


class ObserveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        script = ROOT / 'scripts/build-observe.sh'
        if not script.exists():
            raise AssertionError('missing native observation build script')
        subprocess.run(['sh', str(script)], cwd=ROOT, check=True)
        cls.tmp = tempfile.TemporaryDirectory(prefix='observe-', dir=ROOT / 'build')
        cls.work = Path(cls.tmp.name)
        source = cls.work / 'fake.cpp'
        source.write_text(fake_source())
        cls.library = cls.work / 'libfakeopenvr.so'
        subprocess.run([os.environ.get('CXX', 'g++'), '-std=c++17', '-shared', '-fPIC', '-pthread',
                        '-I', str(ROOT / 'vendor'), str(source), '-o', str(cls.library)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.case = tempfile.TemporaryDirectory(dir=self.work)
        self.path = Path(self.case.name)
        self.log = self.path / 'calls.log'
        self.env = dict(os.environ, OPENVR_API_LIBRARY=str(self.library), FAKE_LOG=str(self.log),
                        FAKE_MODE='', FRAME_OBSERVE_TIMEOUT_SECONDS='3', FRAME_OBSERVE_INPUT_WAIT_MS='200')

    def tearDown(self):
        self.case.cleanup()

    def run_observe(self, *args, ok=True, mode=''):
        self.env['FAKE_MODE'] = mode
        p = subprocess.run([str(ROOT / 'build/frame-observe'), *map(str, args)], env=self.env,
                           capture_output=True, text=True, timeout=6, check=False)
        self.assertEqual(p.returncode == 0, ok, (p.returncode, p.stdout, p.stderr))
        data = json.loads(p.stdout)
        self.assertIs(data['ok'], ok)
        if not ok:
            self.assertTrue(data['error'])
        return data

    def calls(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def inputs(self, **kwargs):
        return self.run_observe('inputs', ROOT / 'resources/input-actions.json', **kwargs)

    def test_inputs_reports_application_input_availability(self):
        self.assertIs(self.inputs()['input_available'], True)
        self.assertIs(self.inputs(mode='input_unavailable')['input_available'], False)

    def test_inputs_read_all_components_with_handed_values_and_origin_identity(self):
        data = self.inputs()
        self.assertEqual(data['command'], 'inputs')
        self.assertEqual(data['manifest'], str(ROOT / 'resources/input-actions.json'))
        self.assertEqual(data['action_set'], '/actions/observe')
        self.assertTrue(data['binding_ready'])
        self.assertFalse(data['wait_expired'])
        for side, scalar, y, digital, index in [('left', .25, -.5, True, 4), ('right', .75, .5, False, 7)]:
            entries = data['inputs'][side]
            self.assertEqual(set(entries), set(input_components(side)))
            for component, kind in input_components(side).items():
                entry = entries[component]
                self.assertEqual(entry['action'], action_name(side, component))
                self.assertEqual(entry['type'], kind)
                self.assertEqual(entry['error'], 0)
                if '/system/' in component or '/thumbrest/' in component:
                    self.assertFalse(entry['active'])
                    self.assertIsNone(entry['value'])
                    self.assertIsNone(entry['origin'])
                    continue
                self.assertTrue(entry['active'])
                self.assertEqual(entry['value'], digital if kind == 'boolean' else y if component.endswith('/y') else scalar)
                self.assertEqual(entry['origin']['device_index'], index)
                self.assertEqual(entry['origin']['serial'], 'frame_testbench_' + side)
                self.assertTrue(entry['origin']['synthetic'])
                self.assertEqual(entry['origin']['error'], 0)
                self.assertEqual(entry['origin']['serial_error'], 0)
        calls = self.calls()
        self.assertLess(calls.index('manifest'), calls.index('update_actions'))
        self.assertEqual(calls[-1], 'shutdown')

    def test_inputs_wait_for_both_sides_and_transient_update_no_data(self):
        for mode, minimum in [('delayed', 4), ('delayed_update', 3)]:
            self.log.unlink(missing_ok=True)
            data = self.inputs(mode=mode)
            self.assertTrue(data['binding_ready'])
            self.assertGreaterEqual(self.calls().count('update_actions'), minimum)
            self.assertTrue(data['inputs']['right']['/input/trigger/value']['active'])

    def test_inputs_inactive_is_not_zero_or_false_and_wait_is_bounded(self):
        start = time.monotonic()
        data = self.inputs(mode='inactive')
        self.assertGreaterEqual(time.monotonic() - start, .18)
        self.assertLess(time.monotonic() - start, 1.5)
        self.assertFalse(data['binding_ready'])
        self.assertTrue(data['wait_expired'])
        for entries in data['inputs'].values():
            for entry in entries.values():
                self.assertFalse(entry['active'])
                self.assertIsNone(entry['value'])
                self.assertIsNone(entry['origin'])
                self.assertEqual(entry['error'], 0)

    def test_inputs_component_errors_preserve_other_readback(self):
        for mode, side, component in [('missing_action', 'left', '/input/bumper/click'),
                                      ('digital_error', 'left', '/input/bumper/click'),
                                      ('analog_error', 'right', '/input/trigger/value'),
                                      ('source_error', 'right', '/input/bumper/click')]:
            with self.subTest(mode=mode):
                data = self.inputs(mode=mode)
                entry = data['inputs'][side][component]
                self.assertNotEqual(entry['error'], 0)
                self.assertIsNone(entry['active'])
                self.assertIsNone(entry['value'])
                self.assertIsNone(entry['origin'])
                self.assertTrue(data['inputs']['left']['/input/grip/value']['active'])
                self.assertFalse(data['binding_ready'])

    def test_inputs_origin_errors_do_not_invent_identity(self):
        data = self.inputs(mode='origin_error')
        origin = data['inputs']['left']['/input/trigger/value']['origin']
        self.assertNotEqual(origin['error'], 0)
        self.assertIsNone(origin['device_index'])
        self.assertIsNone(origin['serial'])
        self.assertFalse(origin['synthetic'])
        data = self.inputs(mode='controller_property_error')
        origin = data['inputs']['right']['/input/trigger/value']['origin']
        self.assertEqual(origin['device_index'], 7)
        self.assertIsNone(origin['serial'])
        self.assertNotEqual(origin['serial_error'], 0)
        self.assertFalse(origin['synthetic'])

    def test_inputs_setup_and_update_errors_fail_with_json(self):
        for mode, operation in [('manifest_error', 'SetActionManifestPath'), ('set_error', 'GetActionSetHandle'),
                                ('update_error', 'UpdateActionState'), ('no_data', 'UpdateActionState')]:
            data = self.inputs(ok=False, mode=mode)
            self.assertIn(operation, data['error'])

    def test_inputs_nonfinite_analog_is_json_null(self):
        data = self.inputs(mode='nonfinite')
        self.assertIsNone(data['inputs']['left']['/input/trigger/value']['value'])

    def test_inputs_hung_sdk_and_readiness_obey_process_alarm(self):
        self.env['FRAME_OBSERVE_TIMEOUT_SECONDS'] = '1'
        self.env['FRAME_OBSERVE_INPUT_WAIT_MS'] = '5000'
        for mode in ('input_hang', 'inactive'):
            self.assertIn('timed out', self.inputs(ok=False, mode=mode)['error'])

    def test_inputs_cli_validation_happens_before_sdk(self):
        for args in [('inputs',), ('inputs', 'relative.json'), ('inputs', self.path / 'missing.json'),
                     ('inputs', self.path), ('inputs', self.path, 'extra')]:
            self.run_observe(*args, ok=False)
        for value in ('-1', '10001', '2x'):
            self.env['FRAME_OBSERVE_INPUT_WAIT_MS'] = value
            self.inputs(ok=False)
        self.assertEqual(self.calls(), [])

    def test_original_input_resources_cover_each_frame_component(self):
        manifest_path = ROOT / 'resources/input-actions.json'
        self.assertTrue(manifest_path.is_file(), 'original action manifest missing')
        manifest = json.loads(manifest_path.read_text())
        self.assertEqual(manifest['action_sets'], [{'name': '/actions/observe', 'usage': 'leftright'}])
        self.assertEqual(manifest['default_bindings'], [{'controller_type': 'frame_controller', 'binding_url': 'frame-controller-bindings.json'}])
        expected = {action_name(side, path): kind for side in ('left', 'right') for path, kind in input_components(side).items()}
        actions = {a['name']: a['type'] for a in manifest['actions']}
        self.assertEqual(actions, expected)
        self.assertEqual(len(manifest['actions']), len(expected))
        self.assertTrue(all(a['requirement'] == 'optional' for a in manifest['actions']))
        binding = json.loads((manifest_path.parent / 'frame-controller-bindings.json').read_text())
        self.assertEqual(binding['controller_type'], 'frame_controller')
        actual = {}
        for source in binding['bindings']['/actions/observe']['sources']:
            side, part = source['path'].split('/')[3], source['path'].split('/')[-1]
            self.assertEqual(source['mode'], 'trigger' if part in ('trigger', 'grip') else 'joystick' if part == 'thumbstick' else 'button')
            for slot, target in source['inputs'].items():
                component = f'/input/{part}/' + ('value' if slot == 'pull' else 'x' if slot == 'position' else slot)
                self.assertEqual(target['output'], action_name(side, component))
                self.assertNotIn(target['output'], actual)
                actual[target['output']] = actions[target['output']]
        self.assertEqual(actual, expected)

    def test_bad_cli_is_rejected_before_sdk(self):
        for args in [(), ('wat',), ('status', 'extra'), ('capture',), ('capture', 'relative'),
                     ('setting', 'yes'), ('setting',), ('debug', '-1', 'x'), ('debug', '64', 'x'),
                     ('debug', '3x', 'x'), ('debug', '3', '')]:
            with self.subTest(args=args):
                self.run_observe(*args, ok=False)
                self.assertEqual(self.calls(), [])

    def test_missing_library(self):
        self.env['OPENVR_API_LIBRARY'] = str(self.path / 'missing.so')
        self.run_observe('status', ok=False)
        self.assertEqual(self.calls(), [])

    def test_bad_sdk(self):
        for mode in ('init_error', 'bad_version', 'missing_interface'):
            with self.subTest(mode=mode):
                self.run_observe('status', ok=False, mode=mode)

    def test_status_is_read_only_and_has_distinct_coordinate_spaces(self):
        d = self.run_observe('status')
        self.assertEqual(d['hmd']['model'], 'Fake "HMD"')
        self.assertEqual(d['hmd']['driver'], 'fake_driver')
        self.assertEqual(d['hmd']['serial'], 'TEST-001')
        self.assertEqual(d['hmd']['activity_level'], 1)
        for space, x in [('standing', 1), ('raw', 2), ('seated', 0)]:
            p = d['hmd']['poses'][space]
            self.assertTrue(p['valid'])
            self.assertTrue(p['connected'])
            self.assertEqual(p['tracking_result'], 200)
            self.assertEqual(p['matrix'], [[1, 0, 0, x], [0, 1, 0, 0], [0, 0, 1, 0]])
            self.assertEqual(p['velocity'], [0.25, 0, 0])
        self.assertEqual(d['transforms']['raw_to_standing'][0][3], 20)
        self.assertEqual(d['transforms']['seated_to_standing'][0][3], 10)
        self.assertEqual(d['compositor']['frame_index'], 100)
        self.assertEqual(d['compositor']['previous_frame_index'], 101)
        self.assertEqual(d['compositor']['vsync_frame_counter'], 12345)
        self.assertTrue(d['settings']['pauseCompositorOnStandby'])
        self.assertEqual(self.calls().count('init_background'), 1)
        self.assertEqual(self.calls()[-1], 'shutdown')
        self.assertFalse(any(x.startswith('set_') for x in self.calls()))

    def test_controllers_include_disconnected_devices_and_exact_index_poses(self):
        d = self.run_observe('status')
        self.assertIn('controllers', d)
        controllers = d['controllers']
        self.assertEqual([c['device_index'] for c in controllers], [2, 4, 7, 63])
        self.assertEqual([c['role'] for c in controllers], [1, 1, 2, 3])
        self.assertEqual([c['serial'] for c in controllers],
                         ['PHYSICAL-002', 'frame_testbench_left', 'frame_testbench_right',
                          'frame_testbench_left_extra'])
        self.assertEqual([c['synthetic'] for c in controllers], [False, True, True, False])
        for controller in controllers:
            index = controller['device_index']
            self.assertEqual(controller['driver'], 'frame_testbench')
            self.assertEqual(controller['model'], 'Pose controller')
            self.assertEqual(controller['property_errors'], {'driver': 0, 'model': 0, 'serial': 0})
            self.assertEqual(set(controller['poses']), {'standing', 'raw', 'seated'})
            for space, origin in [('standing', 1), ('raw', 2), ('seated', 0)]:
                p = controller['poses'][space]
                self.assertEqual(p['connected'], index in (2, 4))
                self.assertEqual(p['valid'], index in (2, 4))
                self.assertEqual(p['tracking_result'], 200 if index in (2, 4) else 1)
                self.assertEqual(p['matrix'], [[1, 0, 0, index * 100 + origin],
                                              [0, 1, 0, 0], [0, 0, 1, 0]])
                self.assertEqual(p['velocity'], [index + .25, 0, 0])
                self.assertEqual(p['angular_velocity'], [0, 0, index])
        # The array is indexed by device, not compacted by class or role. Read
        # each full origin array once and share it with the HMD snapshot.
        self.assertEqual([c for c in self.calls() if c.startswith('poses ')],
                         ['poses 1 64', 'poses 2 64', 'poses 0 64'])
        self.assertFalse(any(c.startswith('set_') for c in self.calls()))

    def test_controller_property_errors_do_not_hide_device_or_claim_synthetic(self):
        d = self.run_observe('status', mode='controller_property_error')
        self.assertIn('controllers', d)
        controllers = {c['device_index']: c for c in d['controllers']}
        self.assertEqual(set(controllers), {2, 4, 7, 63})
        for name in ('driver', 'model', 'serial'):
            self.assertIsNone(controllers[7][name])
            self.assertNotEqual(controllers[7]['property_errors'][name], 0)
            self.assertEqual(controllers[4]['property_errors'][name], 0)
        self.assertFalse(controllers[7]['synthetic'])
        self.assertTrue(controllers[4]['synthetic'])
        self.assertFalse(controllers[7]['poses']['standing']['connected'])

    def test_no_controllers_returns_empty_list(self):
        d = self.run_observe('status', mode='no_controllers')
        self.assertIn('controllers', d)
        self.assertEqual(d['controllers'], [])
        self.assertTrue(d['hmd']['poses']['standing']['valid'])

    def test_invalid_pose_is_not_claimed_valid(self):
        d = self.run_observe('status', mode='invalid_pose')
        self.assertFalse(d['hmd']['poses']['standing']['valid'])

    def test_absent_timing_is_explicit(self):
        d = self.run_observe('status', mode='no_timing')
        self.assertFalse(d['compositor']['timing_available'])
        self.assertIsNone(d['compositor']['frame_index'])

    def test_property_error_is_explicit(self):
        d = self.run_observe('status', mode='property_error')
        self.assertIsNone(d['hmd']['model'])
        self.assertNotEqual(d['hmd']['property_errors']['model'], 0)

    def test_setting_reads_back_and_is_not_restored(self):
        for arg, value in [('false', False), ('true', True), ('0', False), ('1', True)]:
            self.log.unlink(missing_ok=True)
            d = self.run_observe('setting', arg)
            self.assertIs(d['pauseCompositorOnStandby'], value)
            calls = self.calls()
            self.assertEqual(sum(x.startswith('set_') for x in calls), 1)
            self.assertLess(calls.index('set_true' if value else 'set_false'), calls.index('get_bool'))

    def test_settings_errors_and_mismatch_fail(self):
        for mode in ('write_error', 'read_error', 'mismatch'):
            self.run_observe('setting', 'false', ok=False, mode=mode)
        self.run_observe('status', ok=False, mode='read_error')

    def test_capture_refuses_existing_directory_file_and_symlink(self):
        for p in (self.path, self.path / 'file', self.path / 'link'):
            if p.name == 'file':
                p.write_text('keep')
            if p.name == 'link':
                p.symlink_to(self.path / 'missing')
            self.run_observe('capture', p, ok=False)
        self.assertEqual(self.calls(), [])
        self.assertEqual((self.path / 'file').read_text(), 'keep')

    def test_capture_waits_for_two_complete_pngs(self):
        start = time.monotonic()
        d = self.run_observe('capture', self.path / 'new')
        self.assertGreaterEqual(time.monotonic() - start, 0.45)
        self.assertEqual(d['screenshot_handle'], 77)
        for key in ('preview', 'stereo'):
            p = Path(d[key])
            self.assertEqual(p, self.path / 'new' / (key + '.png'))
            self.assertEqual(p.read_bytes(), png())
        self.assertEqual(self.calls().count('request_screenshot'), 1)
        self.assertLess(self.calls().index('pngs_complete'), self.calls().index('shutdown_enter'))
        self.assertFalse(any(x.startswith('set_') for x in self.calls()))

    def test_capture_rejects_incomplete_or_corrupt_png(self):
        self.env['FRAME_OBSERVE_TIMEOUT_SECONDS'] = '1'
        for mode in ('incomplete', 'corrupt'):
            self.run_observe('capture', self.path / mode, ok=False, mode=mode)

    def test_screenshot_api_error(self):
        self.run_observe('capture', self.path / 'new', ok=False, mode='screenshot_error')

    def test_hung_sdk_is_bounded_with_json_error(self):
        self.env['FRAME_OBSERVE_TIMEOUT_SECONDS'] = '1'
        self.run_observe('status', ok=False, mode='hang')

    def test_timeout_setting_is_validated_before_sdk(self):
        for val in ('0', '-1', '3x', '121'):
            self.env['FRAME_OBSERVE_TIMEOUT_SECONDS'] = val
            self.run_observe('status', ok=False)
        self.assertEqual(self.calls(), [])

    def test_debug_uses_public_interface(self):
        d = self.run_observe('debug', '3', 'test request')
        self.assertEqual(d['device_index'], 3)
        self.assertEqual(d['response'], 'response\n"ok"')
        self.assertEqual(self.calls().count('debug'), 1)
        self.run_observe('debug', '3', 'test request', ok=False, mode='debug_large')


if __name__ == '__main__':
    unittest.main()
