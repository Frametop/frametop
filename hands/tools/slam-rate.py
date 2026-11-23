#!/usr/bin/env python3
"""slam-rate: set the rate XRService runs the tracking (SLAM) cameras at.

  python3 hands/tools/slam-rate.py 120       all four cameras at 114 Hz, no dark frames
  python3 hands/tools/slam-rate.py 60        60 Hz, no dark frames
  python3 hands/tools/slam-rate.py default   SteamVR's own: 60 Hz, every other frame dark (30 bright)
  python3 hands/tools/slam-rate.py           print the current setting

120 and 60 turn off the controller-LED frames (streamingMode 0): the controllers lose
optical tracking and SteamVR shows an overlay while any are on. It edits only
captureSessionSettings.trackingCameras in ~/.config/openvr/config/cv/xrservice/XRServiceSettings.json
and keeps the rest (hands/handsrate.py's writer: Frametop Settings' 60 fps switch is the 60 here,
with ft-hands' settings). XRService reads it when it starts: --kill stops it so SteamVR starts
a new one (after a few kills in a row it can stay down; then restart SteamVR).
slamFrameFPSMode, measured with hands/tools/camrate.py on SteamOS 0.5.9 (2026-11-21):
5 = 114.4 Hz, 3 = 60 Hz, 0 and 6-9 = 45 Hz, 1 and 4 stall the cameras.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.realpath(__file__))))
import handsrate  # noqa: E402  (XRService's settings file, and stopping XRService)

MODES = {'120': {'streamingMode': 0, 'slamFrameFPSMode': 5}, '60': handsrate.HIGH, 'default': {}}


def show():
    return json.dumps(handsrate.read_cameras() or 'default')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        print(show())
        return
    if args[0] not in MODES:
        sys.exit('usage: slam-rate.py [120|60|default] [--kill]')
    handsrate.write_cameras(MODES[args[0]])
    print('trackingCameras:', show())
    if '--kill' not in sys.argv:
        print('applies when XRService next starts (--kill, or restart SteamVR)')
        return
    host = handsrate.Host()
    running = bool(host.xrservice())
    restarted, said = handsrate.restart_tracking(host)
    if running and not restarted:
        sys.exit(said)
    print(said)


if __name__ == '__main__':
    try:
        main()
    except handsrate.RateError as e:
        sys.exit('slam-rate: %s' % e)
