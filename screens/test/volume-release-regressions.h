// Review regressions through the actual poll loop. No live VR or helper process.
#include <iostream>
void ReleaseFixtureSetup() {
    g_vr = true;
    systemVR.poses[0].bPoseIsValid = true;
    systemVR.poses[0].mDeviceToAbsoluteTracking = Translation(0, 1.6, 0);
    Command("passthrough controls 1"); Tick();
    vr::VROverlay()->CreateOverlay("frametop.catcher", "catcher", &g_catcher);
}
void AimGrab(unsigned device) {
    const auto p = passthrough::GrabCenter(g_passthrough);
    systemVR.poses[device].bPoseIsValid = true;
    systemVR.poses[device].mDeviceToAbsoluteTracking = Translation(p[0], p[1], p[2] + .2);
}
void MovePortal(unsigned device = 3) {
    AimGrab(device);
    Send(g_volumeGrab.overlay, vr::VREvent_MouseButtonDown, device); Tick();
    assert(VolumeDragging());
    systemVR.poses[device].mDeviceToAbsoluteTracking.m[0][3] += .2; Tick();
}
int PersistenceFixture() {
    ReleaseFixtureSetup();
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit") return 0;
        if (line == "move") MovePortal();
        else if (line == "release") {
            Send(g_volumeGrab.overlay, vr::VREvent_MouseButtonUp, 3); Tick();
            assert(!VolumeDragging() && !launches.empty());
        } else if (line == "cancel") Command("passthrough controls 0");
        else if (line == "capture-command") {
            assert(!launches.empty() && launches.back().size() == 2);
            std::cout << launches.back()[1] << std::endl;
            continue;
        } else {
            std::cout << Reply(line.c_str()) << std::endl;
            continue;
        }
        std::cout << Reply("passthrough state3d") << std::endl;
    }
    return 0;
}
#ifdef FT_CPU_KEYBOARD
namespace keyboard { void TestOverlay(vr::VROverlayHandle_t); void TestHeldKey(); }
int KeyboardReleaseRegression() {
    ReleaseFixtureSetup();
    vr::VROverlayHandle_t keys;
    vr::VROverlay()->CreateOverlay("frametop.keyboard", "keyboard", &keys);
    keyboard::TestOverlay(keys);
    for (bool resize : {false, true}) {
        Command("passthrough controls 1");
        const auto control = resize ? g_volumeFaces[1].overlay : g_volumeGrab.overlay;
        if (resize) {
            const auto p = passthrough::RectangleControls(g_passthrough)[1];
            systemVR.poses[3].bPoseIsValid = true;
            systemVR.poses[3].mDeviceToAbsoluteTracking = Translation(p[0],p[1],p[2]+.4);
            Send(control, vr::VREvent_MouseButtonDown, 3); Tick();
            assert(VolumeDragging());
            systemVR.poses[3].mDeviceToAbsoluteTracking.m[0][3] += .2; Tick();
        } else MovePortal();
        Send(keys, vr::VREvent_MouseButtonUp, 4); Tick();
        Send(keys, vr::VREvent_MouseButtonUp, 3, vr::VRMouseButton_Right); Tick();
        assert(VolumeDragging() && launches.empty());
        Send(keys, vr::VREvent_MouseButtonUp, 3); Tick();
        assert(overlay.parts.at(keys).events.empty());
        assert(!VolumeDragging() && launches.size() == 1);
        const auto released = Reply("passthrough state3d");
        systemVR.poses[3].mDeviceToAbsoluteTracking.m[0][3] += .3; Tick();
        assert(Reply("passthrough state3d") == released);
        Send(keys, vr::VREvent_MouseButtonUp, 3); Tick();
        assert(launches.size() == 1); launches.clear();
    }
    // Raw keyboard releases also route desktop ownership with the exact button.
    ft_vr_screen_create(0,800,600);
    g_screens.at(0).shown = &g_screens.at(0); Tick();
    PressDown(3, BTN_RIGHT, 0, 100, 100);
    std::vector<ft_event> events;
    auto record = [](const ft_event *e, void *p) { static_cast<std::vector<ft_event> *>(p)->push_back(*e); };
    Send(keys, vr::VREvent_MouseButtonUp, 4, vr::VRMouseButton_Right);
    Send(keys, vr::VREvent_MouseButtonUp, 3);
    ft_vr_poll(record, &events);
    assert(g_press.buttons && events.empty());
    Send(keys, vr::VREvent_MouseButtonUp, 3, vr::VRMouseButton_Right);
    ft_vr_poll(record, &events);
    assert(!g_press.buttons && events.size() == 2 && events[0].button == BTN_RIGHT && !events[0].pressed);
    // Keep the keyboard's existing left-only key/modifier release semantics.
    events.clear(); keyboard::TestHeldKey();
    Send(keys, vr::VREvent_MouseButtonUp, 3, vr::VRMouseButton_Right);
    ft_vr_poll(record, &events);
    assert(events.empty());
    Send(keys, vr::VREvent_MouseButtonUp, 3);
    ft_vr_poll(record, &events);
    assert(events.size() == 2);
    assert(events[0].type == FT_KEY && events[0].key == 30 && !events[0].pressed);
    assert(events[1].type == FT_KEY && events[1].key == 1 && !events[1].pressed);
    std::puts("PASS keyboard owner/button release: move, resize, desktop, keys/modifiers");
    return 0;
}
int DesktopButtonOwnershipRegression() {
    ReleaseFixtureSetup();
    systemVR.controllerStateAvailable = false;
    ft_vr_screen_create(0,800,600);
    auto &screen = g_screens.at(0);
    screen.shown = &screen; Tick(); screen.visible = true;
    AimGrab(3); AimGrab(4);
    std::vector<ft_event> events;
    auto record = [](const ft_event *e, void *p) {
        if (e->type == FT_BUTTON) static_cast<std::vector<ft_event> *>(p)->push_back(*e);
    };
    auto button = [&](unsigned device, uint32_t type, uint32_t b) {
        Send(screen.overlay, type, device, b); ft_vr_poll(record, &events);
    };
    button(3, vr::VREvent_MouseButtonDown, vr::VRMouseButton_Left);
    button(4, vr::VREvent_MouseButtonDown, vr::VRMouseButton_Right);
    button(4, vr::VREvent_MouseButtonUp, vr::VRMouseButton_Right);
    button(3, vr::VREvent_MouseButtonUp, vr::VRMouseButton_Left);
    g_press.idleSince = NowMs() - kStuckMs - 1;
    ft_vr_poll(record, &events);
    assert(events.size() == 2 && !g_press.buttons);
    assert(events[0].button == BTN_LEFT && events[0].pressed);
    assert(events[1].button == BTN_LEFT && !events[1].pressed);
    // After the first owner releases, the other device can click normally.
    events.clear();
    button(4, vr::VREvent_MouseButtonDown, vr::VRMouseButton_Right);
    button(4, vr::VREvent_MouseButtonUp, vr::VRMouseButton_Right);
    assert(events.size() == 2 && !g_press.buttons);
    assert(events[0].button == BTN_RIGHT && events[0].pressed);
    assert(events[1].button == BTN_RIGHT && !events[1].pressed);
    std::puts("PASS desktop press/release ownership and subsequent device handoff");
    return 0;
}
int CatcherOwnershipRegression() {
    ReleaseFixtureSetup();
    systemVR.controllerStateAvailable = false; // outside-game state unknown
    ft_vr_screen_create(0,800,600);
    auto &screen = g_screens.at(0);
    screen.shown = &screen; Tick(); screen.visible = true;
    systemVR.poses[3].bPoseIsValid = true;
    systemVR.poses[3].mDeviceToAbsoluteTracking = Translation(-3,1,0);
    RefreshPoses();
    PressDown(3, BTN_LEFT, 0, 100, 100); UpdateCatcher();
    assert(g_catcherShown);
    const auto before = overlay.parts.at(g_catcher).pose;
    AimGrab(4);
    Send(g_volumeGrab.overlay, vr::VREvent_MouseButtonDown, 4); Tick();
    assert(!VolumeDragging()); // single catcher is reserved for the desktop owner
    assert(g_press.buttons && g_press.device == 3);
    assert(std::memcmp(before.m, overlay.parts.at(g_catcher).pose.m, sizeof before.m) == 0);
    g_press.idleSince = NowMs() - kStuckMs - 1; Tick();
    assert(g_press.buttons); // unknown controller state must not fake a release
    Send(g_catcher, vr::VREvent_MouseButtonUp, 4); Tick();
    assert(g_press.buttons);
    // A rejected portal press must not steal the desktop owner's release,
    // even if the second controller aims back onto the desktop panel.
    Send(screen.overlay, vr::VREvent_MouseButtonUp, 4); Tick();
    assert(g_press.buttons && g_press.device == 3);
    std::vector<ft_event> events;
    auto record = [](const ft_event *e, void *p) { static_cast<std::vector<ft_event> *>(p)->push_back(*e); };
    Send(g_catcher, vr::VREvent_MouseButtonUp, 3); ft_vr_poll(record, &events);
    assert(!g_press.buttons && events.size() == 2 && !events[0].pressed);
    // Reverse direction: another controller cannot send a desktop press while held.
    events.clear(); MovePortal(4);
    Send(screen.overlay, vr::VREvent_MouseButtonDown, 3);
    Send(screen.overlay, vr::VREvent_MouseButtonUp, 3);
    Send(screen.bar, vr::VREvent_MouseButtonDown, 3);
    ft_vr_poll(record, &events);
    assert(VolumeDragging() && VolumeDevice() == 4 && !g_press.buttons && events.empty());
    assert(screen.drag == Drag::None);
    Send(g_catcher, vr::VREvent_MouseButtonUp, 4); Tick();
    assert(!VolumeDragging());
    std::puts("PASS two-device catcher exclusion in both directions");
    return 0;
}
#endif
