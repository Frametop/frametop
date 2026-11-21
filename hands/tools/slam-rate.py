#!/usr/bin/env python3
"""slam-rate: set the rate XRService runs the tracking (SLAM) cameras at.

  python3 hands/tools/slam-rate.py 120       all four cameras at 114 Hz, no dark frames
  python3 hands/tools/slam-rate.py 60        60 Hz, no dark frames
  python3 hands/tools/slam-rate.py default   SteamVR's own: 60 Hz, every other frame dark (30 bright)
  python3 hands/tools/slam-rate.py           print the current setting

120 and 60 turn off the controller-LED frames (streamingMode 0): the controllers lose
optical tracking and SteamVR shows an overlay while any are on. It edits only
captureSessionSettings.trackingCameras in ~/.config/openvr/config/cv/xrservice/XRServiceSettings.json
and keeps the rest. XRService reads it when it starts: --kill stops it so SteamVR starts
a new one (after a few kills in a row it can stay down; then restart SteamVR).
slamFrameFPSMode, measured with hands/tools/camrate.py on SteamOS 0.5.9 (2026-11-21):
5 = 114.4 Hz, 3 = 60 Hz, 0 and 6-9 = 45 Hz, 1 and 4 stall the cameras.
"""
import json
import os
import subprocess
import sys
import time

PATH = os.path.expanduser('~/.config/openvr/config/cv/xrservice/XRServiceSettings.json')
MODES = {'120': (0, 5), '60': (0, 3)}


def xrservice():
    r = subprocess.run(['pgrep', '-f', '^XRService --documentsRoot'], capture_output=True, text=True)
    return r.stdout.split()[0] if r.stdout.split() else None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    try:
        with open(PATH) as f:
            s = json.load(f)
    except FileNotFoundError:
        s = {}
    cap = s.setdefault('captureSessionSettings', {})
    if not args:
        print(json.dumps(cap.get('trackingCameras', 'default')))
        return
    mode = args[0]
    if mode == 'default':
        cap.pop('trackingCameras', None)
        if not cap:
            s.pop('captureSessionSettings')
    elif mode in MODES:
        tc = cap.setdefault('trackingCameras', {})
        tc['streamingMode'], tc['slamFrameFPSMode'] = MODES[mode]
    else:
        sys.exit('usage: slam-rate.py [120|60|default] [--kill]')
    os.makedirs(os.path.dirname(PATH), exist_ok=True)
    with open(PATH, 'w') as f:
        f.write(json.dumps(s, indent=4) + '\n')
    print('trackingCameras:', json.dumps(cap.get('trackingCameras', 'default')))
    if '--kill' not in sys.argv:
        print('applies when XRService next starts (--kill, or restart SteamVR)')
        return
    old = xrservice()
    if old:
        subprocess.run(['kill', old])
    for _ in range(100):
        new = xrservice()
        if new and new != old:
            print('XRService restarted (%s -> %s)' % (old, new))
            return
        time.sleep(0.2)
    sys.exit('XRService did not come back: restart SteamVR')


if __name__ == '__main__':
    main()
