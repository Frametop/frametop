# just — the runner for the actions that happen inside the image.
# The CI smoke job runs these with: docker run ... IMAGE just test
# Host-side image management (build, shell, update) lives in ./ft; these
# recipes only ever run where the toolchain and the venv are: in the image.

# default: list the recipes
default:
    @just --list

# everything, strict (the recipes are the source of truth)
test: lint test-python test-c test-keyboard test-bash test-testbench test-fast-install

# the Python suites as a strict gate: every failure fails the run. The image's
# python3 has to import the dnf Qt stack and the locked packages together;
# without that check the Qt tests would only skip. scripts/testgate/testgate.py
# runs each file and fails a pattern that matches nothing; a skipped file or
# test is reported as skipped, with its reason, never as ok.
test-python:
    #!/usr/bin/env bash
    set -uo pipefail
    python3 -c 'import PySide6, numpy, cv2, sherpa_onnx' || { echo "python3 can't import PySide6, numpy, cv2, and sherpa_onnx"; exit 1; }
    status=0
    layout/countdown/deps.sh || status=1
    python3 scripts/testgate/testgate.py 'scripts/testgate/tests/*.py' 'conf/test/*.py' 'layout/tests/*.py' \
        'layout/countdown/tests/test_*.py' 'models/test/*.py' 'commands/test/*.py' 'input-settings/test/*.py' \
        'input/test/*.py' 'hands/tests/test_*.py' 'hands/rec/tests/*.py' 'recorder/tests/*.py' 'gaze/test/*.py' \
        'gaze/rec/tests/*.py' 'gaze/bench/tests/test_*.py' 'pack/test/*.py' 'voice/test/*.py' 'frunner/test/*.py' \
        'tour/test/*.py' 'avatar/tests/test_*.py' 'together/tests/test_*.py' 'menu/test/*.py' 'settings/test/*.py' 'remote/test/*.py' 'steam/test/*.py' 'float/test/*.py' 'display-settings/tests/*.py' 'pointer/tests/*.py' 'remote-displays/test/*.py' \
        'scripts/tests/test_app_imports.py' \
        'scripts/tests/test_uninstall.py' 'scripts/tests/test_get.py' 'scripts/tests/test_install_lib.py' \
        'scripts/tests/test_conf_readers.py' 'scripts/tests/test_install_interrupt.py' 'scripts/tests/test_install_keyboard.py' \
        'scripts/tests/test_install_once.py' 'scripts/tests/test_update_check.py' 'docs/tests/*.py' || status=1
    pytest -q session/tests || status=1
    # test_accessibility.py needs the host's AT-SPI and PyGObject, test_notifyfwd.py and
    # test_gtksettings.py the host's dbus-daemon and PyGObject
    echo "session/test/*.py: SKIPPED: they need the host's AT-SPI, dbus-daemon and PyGObject (run them on the Frame)"
    exit $status

# the C/C++ unit tests, built into build/tests: header-only logic with made-up
# events (pointer/tests: the helper's hand interaction logic), the hands'
# skeleton fit, and Full Keyboard's ctest. A test that fails fails the gate,
# with its output; one that can't build here (no ncnn, no OpenVR header) says
# SKIPPED and why.
test-c:
    #!/usr/bin/env bash
    set -eo pipefail
    mkdir -p build/tests
    status=0
    run() {  # run NAME CMD...: one test program; its output is shown only when it fails
        local name=$1 log=build/tests/last.log; shift
        if "$@" >"$log" 2>&1; then echo "$name: ok"; else echo "$name: FAILED"; cat "$log"; status=1; fi
    }
    run screens/tests/screen-scale-test.c scripts/test-screen-scale.sh
    g++ -std=c++17 -Wall -Wextra -Werror screens/tests/barhit-test.cpp -o build/tests/barhit-test
    run screens/tests/barhit-test.cpp build/tests/barhit-test
    g++ -std=c++17 -Wall -Wextra -Werror screens/tests/resizecorner-test.cpp -o build/tests/resizecorner-test
    run screens/tests/resizecorner-test.cpp build/tests/resizecorner-test
    g++ -std=c++17 -Wall -Wextra -Werror pointer/tests/overlayregion-test.cpp -o build/tests/overlayregion-test
    run pointer/tests/overlayregion-test.cpp build/tests/overlayregion-test
    gcc -std=c11 -Wall -Wextra -Werror screens/tests/pinlock-test.c -lm -o build/tests/pinlock-test
    run screens/tests/pinlock-test.c build/tests/pinlock-test
    gcc -std=c11 -Wall -Wextra -Werror -I. screens/tests/controller-click-test.c \
      -lm -o build/tests/controller-click-test
    run screens/tests/controller-click-test.c build/tests/controller-click-test
    gcc -std=c11 -Wall -Wextra -Werror screens/tests/relay-buttons-test.c \
      -o build/tests/relay-buttons-test
    run screens/tests/relay-buttons-test.c build/tests/relay-buttons-test
    gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g screens/tests/peerinput-test.c \
      -o build/tests/peerinput-test
    run screens/tests/peerinput-test.c build/tests/peerinput-test
    gcc -std=c11 -Wall -Wextra -Werror screens/tests/held-keys-test.c \
      -o build/tests/held-keys-test
    run screens/tests/held-keys-test.c build/tests/held-keys-test
    gcc -std=c11 -Wall -Wextra -Werror screens/tests/held-keys-wlr-test.c \
      $(pkg-config --cflags --libs wlroots-0.20 xkbcommon wayland-server) -o build/tests/held-keys-wlr-test
    run screens/tests/held-keys-wlr-test.c build/tests/held-keys-wlr-test
    gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Werror -DFT_REMOTE_QUIT_MS=600 -Iscreens \
      screens/tests/remote-test.c screens/remote.c screens/child.c $(pkg-config --cflags --libs wlroots-0.20 wayland-server) \
      -o build/tests/remote-test
    run screens/tests/remote-test.c build/tests/remote-test
    # the share tap (a screen's pictures to an ft-sharepanel, for a Together room), the test being that program too
    gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Werror -DFT_SHARE_STUCK_MS=500 -Iscreens \
      screens/tests/share-test.c screens/share.c screens/child.c $(pkg-config --cflags --libs wlroots-0.20 wayland-server) \
      -o build/tests/share-test
    run screens/tests/share-test.c build/tests/share-test
    g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror screens/tests/handinput_test.cpp \
      screens/handcut.cpp screens/handinput.cpp screens/palmmenu.cpp screens/grabthat.cpp screens/steampanels.cpp -o build/tests/handinput-test
    run screens/tests/handinput_test.cpp build/tests/handinput-test
    g++ -std=c++17 -O2 -Wall -Wextra -Werror screens/tests/handshape-test.cpp -o build/tests/handshape-test
    run screens/tests/handshape-test.cpp build/tests/handshape-test
    python3 -I screens/tools/hand-asset.py --cpp build/tests/handasset.cpp >/dev/null
    g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror -Iscreens screens/tests/handview_test.cpp \
      screens/handmesh.cpp screens/handskin.cpp screens/handmodels.cpp avatar/modelfile.cpp build/tests/handasset.cpp \
      screens/handrobot.cpp screens/handcut.cpp -o build/tests/handview-test
    run screens/tests/handview_test.cpp build/tests/handview-test
    run "screens/tools/hand-asset.py --check" python3 -I screens/tools/hand-asset.py --check
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined avatar/tests/modelfile-test.cpp avatar/modelfile.cpp \
      -o build/tests/modelfile-test
    run avatar/tests/modelfile-test.cpp build/tests/modelfile-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined avatar/tests/bodyfile-test.cpp avatar/bodyfile.cpp avatar/modelfile.cpp \
      -o build/tests/bodyfile-test
    run avatar/tests/bodyfile-test.cpp build/tests/bodyfile-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined avatar/tests/sharemark-test.cpp avatar/sharemark.cpp \
      avatar/abstract/abstract.cpp avatar/bodyfile.cpp avatar/modelfile.cpp -o build/tests/sharemark-test
    run avatar/tests/sharemark-test.cpp build/tests/sharemark-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined avatar/tests/bodypose-test.cpp avatar/pose/bodypose.cpp \
      avatar/bodyfile.cpp avatar/modelfile.cpp -o build/tests/bodypose-test
    run avatar/tests/bodypose-test.cpp build/tests/bodypose-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror -fsanitize=address,undefined -Iscreens avatar/tests/bodyview-test.cpp \
      avatar/view/bodymesh.cpp avatar/view/handfit.cpp avatar/view/demo.cpp avatar/pose/bodypose.cpp avatar/bodyfile.cpp avatar/modelfile.cpp \
      screens/handskin.cpp screens/handmodels.cpp screens/handmesh.cpp screens/handcut.cpp build/tests/handasset.cpp -o build/tests/bodyview-test
    run avatar/tests/bodyview-test.cpp build/tests/bodyview-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined avatar/tests/face-test.cpp -o build/tests/face-test
    run avatar/tests/face-test.cpp build/tests/face-test
    # the people of a shared room: the body someone has with no avatar, their pose over the wire onto it, and the client of ft-together
    roomhand="avatar/view/handfit.cpp avatar/view/demo.cpp avatar/pose/bodypose.cpp avatar/bodyfile.cpp avatar/modelfile.cpp screens/handskin.cpp screens/handmodels.cpp screens/handmesh.cpp screens/handcut.cpp build/tests/handasset.cpp"
    g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror -fsanitize=address,undefined -Iscreens avatar/tests/abstract-test.cpp \
      avatar/abstract/abstract.cpp $roomhand -o build/tests/abstract-test
    run avatar/tests/abstract-test.cpp build/tests/abstract-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror -fsanitize=address,undefined -Iscreens avatar/tests/room-test.cpp \
      avatar/room/people.cpp avatar/abstract/abstract.cpp $roomhand -o build/tests/avatar-room-test
    run avatar/tests/room-test.cpp build/tests/avatar-room-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror -fsanitize=address,undefined -Iscreens avatar/tests/sentbody-test.cpp \
      avatar/room/sentbody.cpp avatar/abstract/abstract.cpp $roomhand -o build/tests/sentbody-test
    run avatar/tests/sentbody-test.cpp build/tests/sentbody-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined avatar/tests/roomface-test.cpp -o build/tests/roomface-test
    run avatar/tests/roomface-test.cpp build/tests/roomface-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined avatar/tests/roomclient-test.cpp avatar/room/client.cpp \
      avatar/room/people.cpp -o build/tests/avatar-roomclient-test
    run avatar/tests/roomclient-test.cpp build/tests/avatar-roomclient-test
    # Frametop Together: what a peer sends is read here first, so its readers and rules run under the sanitizers
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/proto-test.cpp together/proto/messages.cpp \
      -o build/tests/together-proto-test
    run together/tests/proto-test.cpp build/tests/together-proto-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/room-test.cpp -o build/tests/together-room-test
    run together/tests/room-test.cpp build/tests/together-room-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/session-test.cpp together/session/session.cpp \
      together/session/wearer.cpp together/session/commands.cpp together/proto/messages.cpp -o build/tests/together-session-test
    run together/tests/session-test.cpp build/tests/together-session-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/rejoin-test.cpp together/session/session.cpp \
      together/session/wearer.cpp together/session/commands.cpp together/proto/messages.cpp -o build/tests/together-rejoin-test
    run together/tests/rejoin-test.cpp build/tests/together-rejoin-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/flow-test.cpp -o build/tests/together-flow-test
    run together/tests/flow-test.cpp build/tests/together-flow-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/budget-test.cpp -o build/tests/together-budget-test
    run together/tests/budget-test.cpp build/tests/together-budget-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/video-test.cpp together/flow/video.cpp \
      together/session/session.cpp together/session/wearer.cpp together/session/commands.cpp together/proto/messages.cpp \
      -o build/tests/together-video-test
    run together/tests/video-test.cpp build/tests/together-video-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/quality-test.cpp together/flow/video.cpp \
      together/session/session.cpp together/session/wearer.cpp together/session/commands.cpp together/proto/messages.cpp \
      -o build/tests/together-quality-test
    run together/tests/quality-test.cpp build/tests/together-quality-test
    # a split compute PC's panels: who may send them, and its pictures on their way to the room
    # (a panel's number indexes what is kept of it: what the sanitizer says of one out of range ends the test)
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined together/tests/pcpanels-test.cpp together/flow/pcpanels.cpp \
      together/flow/video.cpp together/session/session.cpp together/session/wearer.cpp together/session/commands.cpp together/proto/messages.cpp \
      -o build/tests/together-pcpanels-test
    run together/tests/pcpanels-test.cpp build/tests/together-pcpanels-test
    # the sizes the Frame's encoder is given (one it isn't resets the Frame): against /dev/null
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/encsize-test.cpp together/media/irisenc.cpp \
      -o build/tests/together-encsize-test
    run together/tests/encsize-test.cpp build/tests/together-encsize-test
    # control of shared panels: the rules by themselves, then several Frames' sessions against each other
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/controlrules-test.cpp -o build/tests/together-controlrules-test
    run together/tests/controlrules-test.cpp build/tests/together-controlrules-test
    # where the wearer looks on its way out: shared only when they say so, and the pointer that keeps quiet while their eyes move it
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/gaze-test.cpp together/local/gazeask.cpp -o build/tests/together-gaze-test
    run together/tests/gaze-test.cpp build/tests/together-gaze-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/glance-test.cpp -o build/tests/together-glance-test
    run together/tests/glance-test.cpp build/tests/together-glance-test
    # the wearer's hand on someone else's panel: ft-screens' words into what ft-together takes
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/panelhand-test.cpp -o build/tests/together-panelhand-test
    run together/tests/panelhand-test.cpp build/tests/together-panelhand-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/handsend-test.cpp -o build/tests/together-handsend-test
    run together/tests/handsend-test.cpp build/tests/together-handsend-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/control-test.cpp together/flow/control.cpp \
      together/local/controlask.cpp together/session/session.cpp together/session/wearer.cpp together/session/commands.cpp \
      together/proto/messages.cpp -o build/tests/together-control-test
    run together/tests/control-test.cpp build/tests/together-control-test
    # moving shared panels: who may, and where a move ends up
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/moves-test.cpp together/flow/moves.cpp \
      together/local/moveask.cpp together/flow/control.cpp together/local/controlask.cpp together/session/session.cpp together/session/wearer.cpp \
      together/session/commands.cpp together/proto/messages.cpp -o build/tests/together-moves-test
    run together/tests/moves-test.cpp build/tests/together-moves-test
    # the avatars people have on: who is asked for a file and whose is kept, and the files on this Frame
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/looks-test.cpp together/flow/looks.cpp \
      together/session/session.cpp together/session/wearer.cpp together/session/commands.cpp together/proto/messages.cpp \
      $(pkg-config --libs libcrypto) -o build/tests/together-looks-test
    run together/tests/looks-test.cpp build/tests/together-looks-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/wardrobe-test.cpp together/local/wardrobe.cpp \
      together/flow/looks.cpp together/session/session.cpp together/session/wearer.cpp together/proto/messages.cpp avatar/modelfile.cpp \
      $(pkg-config --libs libcrypto) -o build/tests/together-wardrobe-test
    run together/tests/wardrobe-test.cpp build/tests/together-wardrobe-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/tailnet-test.cpp together/net/tailnet.cpp \
      together/net/lookup.cpp -o build/tests/together-tailnet-test
    run together/tests/tailnet-test.cpp build/tests/together-tailnet-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/local-test.cpp together/local/watchers.cpp \
      together/local/inputs.cpp -o build/tests/together-local-test
    run together/tests/local-test.cpp build/tests/together-local-test
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/whereabouts-test.cpp -o build/tests/together-whereabouts-test
    run together/tests/whereabouts-test.cpp build/tests/together-whereabouts-test
    # where the others point: which word stands, which panel here shows the room's, where on it, and the mark's picture
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/points-test.cpp -o build/tests/together-points-test
    run together/tests/points-test.cpp build/tests/together-points-test
    # the room's voices: the jitter buffer, where a voice is heard from, the Opus codec and the mix
    g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/voice-test.cpp together/voice/codec.cpp \
      together/voice/voices.cpp -lopus -o build/tests/together-voice-test
    run together/tests/voice-test.cpp build/tests/together-voice-test
    # the links are msquic, which together/build.sh fetches and builds
    if [ -f together/build/libmsquic.so.2 ]; then
        quic="-Itogether/build/msquic/src/inc together/build/libmsquic.so.2 -Wl,-rpath,$PWD/together/build $(pkg-config --libs openssl) -lpthread"
        g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/links-test.cpp together/net/quiclinks.cpp \
          together/net/quicframes.cpp together/net/quicfiles.cpp together/net/lookup.cpp together/net/tailnet.cpp together/net/selfcert.cpp \
          -o build/tests/together-links-test $quic
        run together/tests/links-test.cpp build/tests/together-links-test
        g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/ft-together.cpp together/net/quiclinks.cpp \
          together/net/quicframes.cpp together/net/quicfiles.cpp together/net/lookup.cpp together/net/tailnet.cpp together/net/selfcert.cpp \
          together/session/session.cpp together/session/wearer.cpp together/session/commands.cpp together/proto/messages.cpp \
          together/flow/video.cpp together/flow/pcpanels.cpp together/flow/looks.cpp together/flow/control.cpp together/flow/moves.cpp together/local/watchers.cpp \
          together/local/media.cpp together/local/wardrobe.cpp together/local/inputs.cpp together/local/controlask.cpp together/local/moveask.cpp \
          together/local/gazeask.cpp avatar/modelfile.cpp -o build/tests/ft-together $quic
        run together/tests/loopback-test.py python3 together/tests/loopback-test.py build/tests/ft-together
        # a PC's sender (with canned pictures) at one of them, and its panels at the other
        g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/ft-pcsend.cpp together/net/quiclinks.cpp \
          together/net/quicframes.cpp together/net/quicfiles.cpp together/net/lookup.cpp together/net/tailnet.cpp together/net/selfcert.cpp \
          together/proto/messages.cpp -o build/tests/ft-pcsend $quic
        run together/tests/pcsend-test.py python3 together/tests/pcsend-test.py build/tests/ft-together build/tests/ft-pcsend
        # ...and with a streaming host's pictures, which the test hands it
        run together/tests/pcfeed-test.py python3 together/tests/pcfeed-test.py build/tests/ft-together build/tests/ft-pcsend
        # ft-roompanels between two of them, each with a stand-in ft-screens. (panels-test.py, the
        # video through the hardware encoder and decoder, is run by hand: together/README.md.)
        run together/tests/roompanels-test.py python3 together/tests/roompanels-test.py build/tests/ft-together
        # ...and panels moved by someone other than their owner
        run together/tests/roomcarry-test.py python3 together/tests/roomcarry-test.py build/tests/ft-together
        # ...and the windows of a PC shared as the room's panels, the PC a stand-in that runs ft-pcsend
        run together/tests/roompcs-test.py python3 together/tests/roompcs-test.py build/tests/ft-together build/tests/ft-pcsend
        g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/ft-roomvoice.cpp together/voice/codec.cpp \
          together/voice/voices.cpp together/voice/sender.cpp together/voice/player.cpp together/voice/headask.cpp together/voice/pulse.cpp \
          avatar/room/client.cpp avatar/room/people.cpp $(pkg-config --cflags --libs opus libpulse-simple) -lpthread -o build/tests/ft-roomvoice
        run together/tests/roomvoice-test.py python3 together/tests/roomvoice-test.py build/tests/ft-together build/tests/ft-roomvoice
        run together/tests/roomaway-test.py python3 together/tests/roomaway-test.py build/tests/ft-together
        run together/tests/roomcontrol-test.py python3 together/tests/roomcontrol-test.py build/tests/ft-together
        # ft-roomgaze next to one of them, with stand-ins for the pointer helper and the gaze service
        run together/tests/roomgaze-test.py python3 together/tests/roomgaze-test.py build/tests/ft-together
        # ft-roominput next to one of them, with stand-ins for ft-roompanels and ft-screens
        run together/tests/roominput-test.py python3 together/tests/roominput-test.py build/tests/ft-together
        # ft-roommove next to one of them and a real ft-roompanels, with a stand-in ft-screens
        run together/tests/roommove-test.py python3 together/tests/roommove-test.py build/tests/ft-together
        # ft-roompoints at one of three of them, with a stand-in for ft-roompanels. It draws through
        # SteamVR, so it wants OpenVR's header (together/build.sh fetches it) and library to build;
        # with --probe it draws nothing.
        openvr=${OPENVR_LIB:-/opt/steamvr/bin/linuxarm64}
        if [ -f together/build/include/openvr.h ] && [ -f "$openvr/libopenvr_api.so" ]; then
            g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -isystem together/build/include together/ft-roompoints.cpp \
              together/points/asker.cpp together/points/dots.cpp -L"$openvr" -lopenvr_api -Wl,-rpath,"/opt/steamvr/bin/linuxarm64:$openvr" \
              -o build/tests/ft-roompoints
            run together/tests/roompoints-test.py python3 together/tests/roompoints-test.py build/tests/ft-together build/tests/ft-roompoints
        else
            echo "together/tests/roompoints-test.py: SKIPPED: no together/build/include/openvr.h or no libopenvr_api.so in $openvr"
        fi
        # ft-peerpanel's hand without its decoder, from one of them to the other's desktop
        g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined together/tests/handwords.cpp -o build/tests/together-handwords
        run together/tests/panelhand-test.py python3 together/tests/panelhand-test.py build/tests/ft-together build/tests/together-handwords
    else
        echo "together/tests/{links-test.cpp,loopback-test.py,pcsend-test.py,pcfeed-test.py,roompanels-test.py,roomcarry-test.py,roompcs-test.py,roomvoice-test.py,roomaway-test.py,roomcontrol-test.py,roomgaze-test.py,roominput-test.py,roommove-test.py,roompoints-test.py,panelhand-test.py}: SKIPPED: no together/build/libmsquic.so.2 (together/build.sh builds it)"
    fi
    for t in pointer/tests/*-test.cpp; do
        n=$(basename "$t" .cpp)
        g++ -std=c++17 -Wall -Wextra -Werror -I. "$t" -o "build/tests/$n"
        run "$t" "build/tests/$n"
    done
    g++ -std=c++17 -Wall -Wextra -Werror -Istream stream/test/hostinput-test.cpp -o build/tests/hostinput-test
    run stream/test/hostinput-test.cpp build/tests/hostinput-test
    g++ -std=c++17 -Wall -Wextra -Werror -Istream stream/test/clipboard-test.cpp -o build/tests/clipboard-test
    run stream/test/clipboard-test.cpp build/tests/clipboard-test
    # ft-clip, the shared clipboard's desktop side, against a wlroots data-control seat of the test's own
    wayland-scanner client-header stream/protocols/wlr-data-control-unstable-v1.xml \
      build/tests/wlr-data-control-unstable-v1-client-protocol.h
    wayland-scanner private-code stream/protocols/wlr-data-control-unstable-v1.xml build/tests/wlr-data-control-protocol.c
    gcc -std=gnu11 -Wall -Wextra -Werror -DFT_CLIP_MAX=262144 -DFT_CLIP_MAX_IMAGE=262144 -Ibuild/tests stream/ft-clip.c \
      build/tests/wlr-data-control-protocol.c $(pkg-config --cflags --libs wayland-client) -o build/tests/ft-clip
    gcc -std=gnu11 -Wall -Wextra -Werror stream/test/ft-clip-test.c \
      $(pkg-config --cflags --libs wlroots-0.20 wayland-server) -o build/tests/ft-clip-test
    run stream/test/ft-clip-test.c build/tests/ft-clip-test build/tests/ft-clip
    gcc -std=gnu11 -Wall -Wextra -Wno-unused-function -Werror -pthread gaze/tracker/tests/eyegrab-files-test.c \
      -o build/tests/eyegrab-files-test
    run gaze/tracker/tests/eyegrab-files-test.c build/tests/eyegrab-files-test
    g++ -std=c++17 -O2 -Wall -Wextra -Werror screens/tests/trackwatch-test.cpp \
      -o build/tests/trackwatch-test
    run screens/tests/trackwatch-test.cpp build/tests/trackwatch-test
    g++ -std=c++17 -O2 -Wall -Wextra -Werror screens/tests/levelpin-test.cpp \
      -o build/tests/levelpin-test
    run screens/tests/levelpin-test.cpp build/tests/levelpin-test
    g++ -std=c++17 -O2 -Wall -Wextra -Werror screens/tests/universefix-test.cpp \
      -o build/tests/universefix-test
    run screens/tests/universefix-test.cpp build/tests/universefix-test
    g++ -std=c++17 -O2 -Wall -Wextra -Werror screens/tests/posemath-test.cpp \
      -o build/tests/posemath-test
    run screens/tests/posemath-test.cpp build/tests/posemath-test
    g++ -std=c++17 -O2 -Wall -Wextra -Werror screens/tests/roomshift-test.cpp \
      -o build/tests/roomshift-test
    run screens/tests/roomshift-test.cpp build/tests/roomshift-test
    g++ -std=c++17 -O2 -Wall -Wextra -Werror screens/tests/layoutgrab-test.cpp \
      -o build/tests/layoutgrab-test
    run screens/tests/layoutgrab-test.cpp build/tests/layoutgrab-test
    g++ -std=c++17 -O2 -Wall -Wextra -Werror screens/tests/vrcmd-test.cpp \
      -o build/tests/vrcmd-test
    run screens/tests/vrcmd-test.cpp build/tests/vrcmd-test
    g++ -std=c++17 -O2 -Wall -Wextra -Werror screens/tests/windowdrag-test.cpp \
      -o build/tests/windowdrag-test
    run screens/tests/windowdrag-test.cpp build/tests/windowdrag-test
    g++ -std=c++17 -O2 -Wall -Wextra -Werror screens/tests/wallimage-test.cpp \
      -o build/tests/wallimage-test
    run screens/tests/wallimage-test.cpp build/tests/wallimage-test
    # stb_truetype.h, pinned, fetched once; without it (no network) only this test fails
    if (. scripts/stb.sh && stb_fetch build/include stb_truetype.h); then
        g++ -std=c++17 -O2 -Wall -Wextra -Werror -I. -isystem build/include screens/tests/glyphs-test.cpp \
          -o build/tests/glyphs-test
        run screens/tests/glyphs-test.cpp build/tests/glyphs-test
    else
        echo "screens/tests/glyphs-test.cpp: FAILED (no stb_truetype.h: scripts/stb.sh couldn't fetch it)"; status=1
    fi
    g++ -std=c++17 -O2 -Wall -Wextra -Werror screens/tests/overlaypanel-test.cpp \
      -o build/tests/overlaypanel-test
    run screens/tests/overlaypanel-test.cpp build/tests/overlaypanel-test
    g++ -std=c++17 -O2 -Wall -Wextra -Werror screens/tests/gpuctx-test.cpp \
      -o build/tests/gpuctx-test
    run screens/tests/gpuctx-test.cpp build/tests/gpuctx-test
    gcc -std=c11 -Wall -Wextra -Werror screens/tests/logfile-test.c -o build/tests/logfile-test
    run screens/tests/logfile-test.c build/tests/logfile-test
    gcc -std=gnu11 -Wall -Wextra -Werror conf/test/ftsock-test.c -o build/tests/ftsock-test
    run conf/test/ftsock-test.c build/tests/ftsock-test
    g++ -std=c++17 -Wall -Wextra -Werror -x c++ conf/test/ftsock-test.c -o build/tests/ftsock-test-cxx
    run "conf/test/ftsock-test.c (as C++)" build/tests/ftsock-test-cxx
    # hands: the skeleton fit needs no model runtime; sides, pinch and flagbuf link ncnn,
    # which only hands/build.sh builds (minutes, and a download), so they run where it's built
    g++ -std=c++17 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-psabi hands/tests/skeleton_test.cpp \
      hands/track/skeleton.cpp -o build/tests/skeleton-test
    run hands/tests/skeleton_test.cpp build/tests/skeleton-test
    g++ -std=c++17 -O2 -Wall -Wextra -Werror hands/tests/shm_test.cpp -o build/tests/shm-test
    run hands/tests/shm_test.cpp build/tests/shm-test
    g++ -std=c++17 -O2 -Wall -Wextra -Werror -Wno-unused-parameter hands/tests/calibfile_test.cpp hands/track/calibfile.cpp \
      -o build/tests/calibfile-test
    run hands/tests/calibfile_test.cpp build/tests/calibfile-test
    gcc -std=gnu11 -Wall -Wextra -Werror hands/tests/xrcams_test.c -o build/tests/xrcams-test
    run hands/tests/xrcams_test.c build/tests/xrcams-test
    # ft-powerd has no test; it talks to SteamVR and the display, so it's only compiled
    run "power/ft-powerd.cpp (compiles)" g++ -std=c++17 -fsyntax-only -Wall -Werror -Wno-unused-parameter \
      -Ikeyboard/third_party/openvr power/ft-powerd.cpp
    if [ -f hands/build/ncnn/install/lib/libncnn.a ]; then
        make -s -C hands build/sides-test build/pinch-test build/flagbuf-test
        for n in sides pinch flagbuf; do run "hands/tests/${n}_test.cpp" "hands/build/$n-test"; done
    else
        echo "hands/tests/{sides,pinch,flagbuf}_test.cpp: SKIPPED: they link ncnn, which isn't built here (hands/build.sh)"
    fi
    # ft-gaze's eye-server.mmap layout detection: the pinned OpenVR header gaze/build.sh fetches
    # (no download in a test), and an OpenVR API library to link (OPENVR_LIB in the image)
    openvr=${OPENVR_LIB:-/opt/steamvr/bin/linuxarm64}
    if [ ! -f gaze/build/include/openvr.h ]; then
        echo "gaze/test/mmap-layout-test.cpp: SKIPPED: no gaze/build/include/openvr.h (gaze/build.sh fetches it)"
    elif [ ! -f "$openvr/libopenvr_api.so" ]; then
        echo "gaze/test/mmap-layout-test.cpp: SKIPPED: no libopenvr_api.so in $openvr"
    else
        g++ -std=c++17 -O2 -Wall -Wno-unused-parameter -Wno-missing-field-initializers -Igaze/build/include \
          -Ipointer/common -o build/tests/mmap-layout-test gaze/test/mmap-layout-test.cpp -L"$openvr" -lopenvr_api \
          -Wl,-rpath,"$openvr" -lpthread
        run gaze/test/mmap-layout-test.cpp build/tests/mmap-layout-test
    fi
    exit $status

# Full Keyboard's ctest (keyboard/tests), configured and built in build/tests/keyboard, with
# its own OpenVR header (keyboard/third_party/openvr) and the library scripts/openvr.sh names,
# without that script's header download. As root (the image, the dev container) the two
# tests that make a save fail with a read-only folder can't (root writes there anyway), so
# they run as nobody.
test-keyboard:
    #!/usr/bin/env bash
    set -eo pipefail
    openvr=${OPENVR_LIB:-/opt/steamvr/bin/linuxarm64}
    [ -f "$openvr/libopenvr_api.so" ] || { echo "keyboard ctest: SKIPPED: no libopenvr_api.so in $openvr"; exit 0; }
    out=build/tests/keyboard
    mkdir -p "$out"
    cmake -S keyboard -B "$out" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON \
      -DOPENVR_LIBRARY="$openvr/libopenvr_api.so" -DOPENVR_RUNTIME_PATH="/opt/steamvr/bin/linuxarm64:$openvr" >/dev/null
    cmake --build "$out" >"$out/build.log" 2>&1 || { tail -40 "$out/build.log"; exit 1; }
    cd "$out"
    # systemd-analyze --user (startup-unit) wants a runtime folder, with a path short enough
    # for the unix sockets in it
    export XDG_RUNTIME_DIR=/tmp/ft-keyboard-test-$(id -u)
    mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"
    status=0
    if [ "$(id -u)" = 0 ]; then
        ctest --output-on-failure -E '^(startup|settings)$' || status=1
        for t in startup settings; do
            if setpriv --reuid=65534 --regid=65534 --clear-groups env HOME=/tmp "./$t-tests" >"$t.log" 2>&1; then
                echo "keyboard ctest $t (as nobody): ok"
            else
                echo "keyboard ctest $t (as nobody): FAILED"; cat "$t.log"; status=1
            fi
        done
    else
        ctest --output-on-failure || status=1
    fi
    exit $status

# shell scripts: syntax-gate everything that ships, the *.sh files and the
# scripts named by their #! line alone (ft, ft-layout, in-box, ...); a #!/bin/sh
# script is checked as sh. No git: the image has none, and a mounted checkout
# can trip "dubious ownership"; find lists what the repo actually ships, without
# other worktrees (.worktrees/) or build output. The count guard makes a
# silently short list fail the gate instead of passing it.
test-bash:
    #!/usr/bin/env bash
    set -uo pipefail
    prune=(\( -path ./.git -o -path ./.worktrees -o -path '*/build' -o -path '*/.venv' -o -path '*/node_modules' \
        -o -path '*/__pycache__' \) -prune)
    mapfile -d '' named < <(find . "${prune[@]}" -o -type f -name '*.sh' -print0)
    mapfile -d '' shebang < <(find . "${prune[@]}" -o -type f -print0 | xargs -0 grep -lIZ -m1 '^#!' --)
    declare -A how=()
    for f in "${named[@]}"; do how[$f]=bash; done
    for f in "${shebang[@]}"; do
        case "$(head -n 1 -- "$f")" in
            '#!'*bash*) how[$f]=bash ;;
            '#!/bin/sh' | '#!/bin/sh '* | '#!/usr/bin/env sh' | '#!/usr/bin/env sh '*) how[$f]=sh ;;
        esac
    done
    [ "${#how[@]}" -ge 40 ] || { echo "only ${#how[@]} scripts found — the list is broken"; exit 1; }
    status=0
    while IFS= read -r -d '' f; do
        "${how[$f]}" -n "$f" && echo "${how[$f]} -n $f: ok" || { echo "${how[$f]} -n $f: FAILED"; status=1; }
    done < <(printf '%s\0' "${!how[@]}" | sort -z)
    echo "shell scripts: ${#how[@]} checked"
    # The installers' shared helper, the one script so far that shellcheck must pass (warnings
    # and errors): it's sourced by every installer. A check for the dev container: the release
    # image (CI's ./ft dev test) has no ShellCheck, so it skips there.
    if command -v shellcheck >/dev/null; then
        shellcheck -S warning scripts/install-lib.sh && echo "shellcheck scripts/install-lib.sh: ok" ||
            { echo "shellcheck scripts/install-lib.sh: FAILED"; status=1; }
    else
        echo "skip  shellcheck: not installed (setup/packages.txt's dev line; setup/dev-container.sh adds it)"
    fi
    exit $status

# the testbench, a dev-only tool: its SteamVR input proxy and observer against fake runtimes,
# pose math, the installer, the CLI. .dockerignore keeps it out of the image, so only a
# mounted checkout (`ft dev test`) has it. The MCP tests skip here, with no mcp package;
# testbench/install.sh runs them in its venv.
test-testbench:
    #!/usr/bin/env bash
    set -e
    [ -d testbench ] || { echo "testbench/: not in the image (dev-only), skipped"; exit 0; }
    cd testbench && python3 -m unittest discover -s tests

# scripts/fast-install, a dev-only tool (AGENTS.md): against a made-up repo and release tree.
# .dockerignore keeps it out of the image too.
test-fast-install:
    #!/usr/bin/env bash
    set -e
    [ -f scripts/fast-install ] || { echo "scripts/fast-install: not in the image (dev-only), skipped"; exit 0; }
    python3 scripts/tests/test_fast_install.py

# lint the Python side (ruff from the locked dev group, the rules in pyproject.toml's
# [tool.ruff]): a finding fails it, and `just test`, which runs it first (a second, not minutes).
# It lints the tree as it is here, untracked .py files too, so a scratch script can fail it.
lint:
    ruff check --no-cache .

# the whole gate (`just test` lints too)
check: test
