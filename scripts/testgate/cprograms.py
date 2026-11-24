"""The C/C++ unit tests `just test-c` builds into build/tests and runs, in the order it reports
them (scripts/testgate/cbuild.py says what each item is): header-only logic with made-up events
(pointer/tests: the helper's hand interaction logic), the screens' and avatars' parts, Frametop
Together's readers and rules under the sanitizers, the hands' skeleton fit, and the programs
the Together loopback tests run. A test that can't build here (no msquic, no ncnn, no OpenVR
header) is a Skip that says why. Add a new test here."""
import glob
import os
import shlex
import subprocess

from cbuild import Check, Fail, Generate, Program, Skip

W = "-Wall -Wextra -Werror"
CXX = f"-std=c++17 {W}"
CXX2 = f"-std=c++17 -O2 {W}"
CXX2_NUP = "-std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror"
C11 = f"-std=c11 {W}"
GNU11 = f"-std=gnu11 {W}"
GNU11_NUP = "-std=gnu11 -Wall -Wextra -Wno-unused-parameter -Werror"
# what a peer sends is read here first, so its readers and rules run under the sanitizers
ASAN = "-std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined"
ASAN_NUP = "-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -Werror -fsanitize=address,undefined"

HANDASSET = "build/tests/handasset.cpp"
# the hand an avatar has with no model: its fit, pose and skin
ROOMHAND = ("avatar/view/handfit.cpp", "avatar/view/demo.cpp", "avatar/pose/bodypose.cpp", "avatar/bodyfile.cpp",
            "avatar/modelfile.cpp", "screens/handskin.cpp", "screens/handmodels.cpp", "screens/handmesh.cpp",
            "screens/handcut.cpp", HANDASSET)
SESSION = ("together/session/session.cpp", "together/session/wearer.cpp", "together/session/commands.cpp",
           "together/proto/messages.cpp")
# Together's links (msquic)
NET = ("together/net/quiclinks.cpp", "together/net/quicframes.cpp", "together/net/quicfiles.cpp",
       "together/net/lookup.cpp", "together/net/tailnet.cpp", "together/net/selfcert.cpp")
XCB = "xcb xcb-xtest xcb-icccm"
WLR_PROTO = "stream/protocols/wlr-data-control-unstable-v1.xml"


def P(out, *sources, flags, test=True, **kw):
    """A Program whose test name is its first source (test=False: a helper)."""
    return Program(out, tuple(sources), flags, test=sources[0] if test is True else (test or None), **kw)


def single(path, flags, cflags="", out=None, **kw):
    """A one-file test named after its file, compiled by gcc (a .c) or g++."""
    return P(out or os.path.splitext(os.path.basename(path))[0], path, flags=flags, cflags=cflags,
             cc="gcc" if path.endswith(".c") else "g++", **kw)


def py(path, *programs):
    """A Together loopback test: a Python script run with the programs it drives, and the room's
    Python tools (together/ft-room*, ...), which read conf/'s modules."""
    return Check(path, ("python3", path, *(f"build/tests/{p}" for p in programs)), needs=programs,
                 watch=("together", "conf"))


def prepare(root):
    """Before items(): fetch stb_truetype.h (pinned, scripts/stb.sh) for glyphs-test, once."""
    subprocess.run(["bash", "-c", ". scripts/stb.sh && stb_fetch build/include stb_truetype.h"], cwd=root,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def items(root, env=os.environ):
    openvr = env.get("OPENVR_LIB") or "/opt/steamvr/bin/linuxarm64"
    has = lambda p: os.path.exists(os.path.join(root, p))   # noqa: E731
    out = [
        Check("screens/tests/screen-scale-test.c", ("scripts/test-screen-scale.sh",),   # it compiles the test
              watch=("screens/tests/screen-scale-test.c", "screens/screen-scale.h")),
        single("screens/tests/barhit-test.cpp", CXX),
        single("screens/tests/resizecorner-test.cpp", CXX),
        single("pointer/tests/overlayregion-test.cpp", CXX),
        P("pinlock-test", "screens/tests/pinlock-test.c", cc="gcc", flags=C11, libs="-lm"),
        P("controller-click-test", "screens/tests/controller-click-test.c", cc="gcc", flags=C11, cflags="-I.", libs="-lm"),
        single("screens/tests/relay-buttons-test.c", C11),
        P("peerinput-test", "screens/tests/peerinput-test.c", cc="gcc", flags=f"{C11} -fsanitize=address,undefined -g"),
        single("screens/tests/held-keys-test.c", C11),
        P("held-keys-wlr-test", "screens/tests/held-keys-wlr-test.c", cc="gcc", flags=C11,
          pkgs="wlroots-0.20 xkbcommon wayland-server"),
        P("remote-test", "screens/tests/remote-test.c", "screens/remote.c", "screens/child.c", cc="gcc", flags=GNU11_NUP,
          cflags="-DFT_REMOTE_QUIT_MS=600 -Iscreens", pkgs="wlroots-0.20 wayland-server"),
        # the share tap (a screen's pictures to an ft-sharepanel, for a Together room), the test being that program too
        P("share-test", "screens/tests/share-test.c", "screens/share.c", "screens/child.c", cc="gcc", flags=GNU11_NUP,
          cflags="-DFT_SHARE_STUCK_MS=500 -Iscreens", pkgs="wlroots-0.20 wayland-server"),
        P("handinput-test", "screens/tests/handinput_test.cpp", "screens/handcut.cpp", "screens/handinput.cpp",
          "screens/palmmenu.cpp", "screens/grabthat.cpp", "screens/steampanels.cpp", flags=CXX2_NUP),
        single("screens/tests/handshape-test.cpp", CXX2),
        Generate(HANDASSET, ("python3", "-I", "screens/tools/hand-asset.py", "--cpp", "{out}"),
                 watch=("screens/tools/hand-asset.py", "screens/assets/hands")),
        P("handview-test", "screens/tests/handview_test.cpp", "screens/handmesh.cpp", "screens/handskin.cpp",
          "screens/handmodels.cpp", "avatar/modelfile.cpp", HANDASSET, "screens/handrobot.cpp", "screens/handcut.cpp",
          flags=CXX2_NUP, cflags="-Iscreens"),
        Check("screens/tools/hand-asset.py --check", ("python3", "-I", "screens/tools/hand-asset.py", "--check"),
              watch=("screens/assets/hands", "screens/handasset.h", "screens/handasset.cpp")),
        P("modelfile-test", "avatar/tests/modelfile-test.cpp", "avatar/modelfile.cpp", flags=ASAN),
        P("bodyfile-test", "avatar/tests/bodyfile-test.cpp", "avatar/bodyfile.cpp", "avatar/modelfile.cpp", flags=ASAN),
        P("sharemark-test", "avatar/tests/sharemark-test.cpp", "avatar/sharemark.cpp", "avatar/abstract/abstract.cpp",
          "avatar/bodyfile.cpp", "avatar/modelfile.cpp", flags=ASAN),
        P("bodypose-test", "avatar/tests/bodypose-test.cpp", "avatar/pose/bodypose.cpp", "avatar/bodyfile.cpp",
          "avatar/modelfile.cpp", flags=ASAN),
        P("bodyview-test", "avatar/tests/bodyview-test.cpp", "avatar/view/bodymesh.cpp", "avatar/view/handfit.cpp",
          "avatar/view/demo.cpp", "avatar/pose/bodypose.cpp", "avatar/bodyfile.cpp", "avatar/modelfile.cpp",
          "screens/handskin.cpp", "screens/handmodels.cpp", "screens/handmesh.cpp", "screens/handcut.cpp", HANDASSET,
          flags=ASAN_NUP, cflags="-Iscreens"),
        single("avatar/tests/face-test.cpp", ASAN),
        # the people of a shared room: the body someone has with no avatar, their pose over the wire onto it,
        # and the client of ft-together
        P("abstract-test", "avatar/tests/abstract-test.cpp", "avatar/abstract/abstract.cpp", *ROOMHAND,
          flags=ASAN_NUP, cflags="-Iscreens"),
        P("avatar-room-test", "avatar/tests/room-test.cpp", "avatar/room/people.cpp", "avatar/abstract/abstract.cpp",
          *ROOMHAND, flags=ASAN_NUP, cflags="-Iscreens"),
        P("sentbody-test", "avatar/tests/sentbody-test.cpp", "avatar/room/sentbody.cpp", "avatar/abstract/abstract.cpp",
          *ROOMHAND, flags=ASAN_NUP, cflags="-Iscreens"),
        single("avatar/tests/roomface-test.cpp", ASAN),
        P("avatar-roomclient-test", "avatar/tests/roomclient-test.cpp", "avatar/room/client.cpp",
          "avatar/room/people.cpp", flags=ASAN),
        # Frametop Together: what a peer sends is read here first
        P("together-proto-test", "together/tests/proto-test.cpp", "together/proto/messages.cpp", flags=ASAN),
        P("together-room-test", "together/tests/room-test.cpp", flags=ASAN),
        P("together-session-test", "together/tests/session-test.cpp", *SESSION, flags=ASAN),
        P("together-rejoin-test", "together/tests/rejoin-test.cpp", *SESSION, flags=ASAN),
        P("together-flow-test", "together/tests/flow-test.cpp", flags=ASAN),
        P("together-budget-test", "together/tests/budget-test.cpp", flags=ASAN),
        P("together-video-test", "together/tests/video-test.cpp", "together/flow/video.cpp", *SESSION, flags=ASAN),
        P("together-quality-test", "together/tests/quality-test.cpp", "together/flow/video.cpp", *SESSION, flags=ASAN),
        # a split compute PC's panels: who may send them, and its pictures on their way to the room (a panel's
        # number indexes what is kept of it: what the sanitizer says of one out of range ends the test)
        P("together-pcpanels-test", "together/tests/pcpanels-test.cpp", "together/flow/pcpanels.cpp",
          "together/flow/video.cpp", *SESSION, flags=f"{ASAN} -fno-sanitize-recover=undefined"),
        # the sizes the Frame's encoder is given (one it isn't resets the Frame): against /dev/null
        P("together-encsize-test", "together/tests/encsize-test.cpp", "together/media/irisenc.cpp", flags=ASAN),
        # control of shared panels: the rules by themselves, then several Frames' sessions against each other
        P("together-controlrules-test", "together/tests/controlrules-test.cpp", flags=ASAN),
        # where the wearer looks on its way out: shared only when they say so, and the pointer that keeps quiet
        # while their eyes move it
        P("together-gaze-test", "together/tests/gaze-test.cpp", "together/local/gazeask.cpp", flags=ASAN),
        P("together-glance-test", "together/tests/glance-test.cpp", flags=ASAN),
        # the wearer's hand on someone else's panel: ft-screens' words into what ft-together takes
        P("together-panelhand-test", "together/tests/panelhand-test.cpp", flags=ASAN),
        P("together-handsend-test", "together/tests/handsend-test.cpp", flags=ASAN),
        P("together-control-test", "together/tests/control-test.cpp", "together/flow/control.cpp",
          "together/local/controlask.cpp", *SESSION, flags=ASAN),
        # moving shared panels: who may, and where a move ends up
        P("together-moves-test", "together/tests/moves-test.cpp", "together/flow/moves.cpp", "together/local/moveask.cpp",
          "together/flow/control.cpp", "together/local/controlask.cpp", *SESSION, flags=ASAN),
        # the avatars people have on: who is asked for a file and whose is kept, and the files on this Frame
        P("together-looks-test", "together/tests/looks-test.cpp", "together/flow/looks.cpp", *SESSION, flags=ASAN,
          pkglibs="libcrypto"),
        P("together-wardrobe-test", "together/tests/wardrobe-test.cpp", "together/local/wardrobe.cpp",
          "together/flow/looks.cpp", "together/session/session.cpp", "together/session/wearer.cpp",
          "together/proto/messages.cpp", "avatar/modelfile.cpp", flags=ASAN, pkglibs="libcrypto"),
        P("together-tailnet-test", "together/tests/tailnet-test.cpp", "together/net/tailnet.cpp",
          "together/net/lookup.cpp", flags=ASAN),
        P("together-local-test", "together/tests/local-test.cpp", "together/local/watchers.cpp",
          "together/local/inputs.cpp", flags=ASAN),
        P("together-whereabouts-test", "together/tests/whereabouts-test.cpp", flags=ASAN),
        # where the others point: which word stands, which panel here shows the room's, where on it, and the
        # mark's picture
        P("together-points-test", "together/tests/points-test.cpp", flags=ASAN),
        # the room's voices: the jitter buffer, where a voice is heard from, the Opus codec and the mix
        P("together-voice-test", "together/tests/voice-test.cpp", "together/voice/codec.cpp",
          "together/voice/voices.cpp", flags=ASAN, libs="-lopus"),
    ]
    out += _quic(root, openvr, has)
    pointer = {p.sources[0] for p in out if isinstance(p, Program)}
    for t in sorted(glob.glob(os.path.join(root, "pointer/tests/*-test.cpp"))):
        rel = os.path.relpath(t, root)
        if rel not in pointer:
            out.append(single(rel, CXX, cflags="-I."))
    out += [
        single("power/standby-test.cpp", CXX),   # ft-powerd's forced standby (power/standby.h)
        single("stream/test/hostinput-test.cpp", CXX, cflags="-Istream"),
        single("stream/test/clipboard-test.cpp", CXX, cflags="-Istream"),
        single("stream/test/audio-clock-test.cpp", CXX),
        single("stream/test/kwin-frame-test.cpp", CXX, cflags="-Istream", pkgs="libcrypto"),
        # ft-appless's macOS High Performance Screen Sharing client: SRP, its crypto and a loopback session
        single("stream/test/apple-hp-test.cpp", CXX2, cflags="-Istream", pkgs="openssl zlib", libs="-lpthread"),
        # ft-clip, the shared clipboard's desktop side, against a wlroots data-control seat of the test's own
        Generate("build/tests/wlr-data-control-unstable-v1-client-protocol.h",
                 ("wayland-scanner", "client-header", WLR_PROTO, "{out}"), watch=(WLR_PROTO,)),
        Generate("build/tests/wlr-data-control-protocol.c", ("wayland-scanner", "private-code", WLR_PROTO, "{out}"),
                 watch=(WLR_PROTO,)),
        P("ft-clip", "stream/ft-clip.c", "build/tests/wlr-data-control-protocol.c", cc="gcc", flags=GNU11, test=False,
          cflags="-DFT_CLIP_MAX=262144 -DFT_CLIP_MAX_IMAGE=262144 -Ibuild/tests", pkgs="wayland-client"),
        P("ft-clip-test", "stream/test/ft-clip-test.c", cc="gcc", flags=GNU11, pkgs="wlroots-0.20 wayland-server",
          args=("build/tests/ft-clip",), needs=("ft-clip",)),
        P("eyegrab-files-test", "gaze/tracker/tests/eyegrab-files-test.c", cc="gcc",
          flags="-std=gnu11 -Wall -Wextra -Wno-unused-function -Werror -pthread"),
        P("eyegrab-late-test", "gaze/tracker/tests/eyegrab-late-test.c", cc="gcc",
          flags="-std=gnu11 -Wall -Wextra -Wno-unused-function -Werror -pthread"),
    ]
    for name in ("trackwatch", "levelpin", "dashanchor", "tiling", "dashtiles", "tilewide", "tilecurve", "centreglow", "barside", "appscale", "panelmenu", "menulayout",
                 # palm Quick Access (quickaccess.h): its tiles, where a tip or laser lands, Reset's ask,
                 # its Quick Settings toggles (quicksettings.h); and its readouts (sysstats.h, battery.h)
                 "quickaccess", "sysstats", "battery"):
        out.append(single(f"screens/tests/{name}-test.cpp", CXX2))
    out += [
        # gamescope apps (gsforward.h): the policy, and the X side against an Xvfb when there is one
        P("gsforward-test", "screens/tests/gsforward-test.cpp", "screens/gsforward.cpp", flags=CXX2, cflags="-Iscreens",
          pkgs=XCB),
        single("screens/tests/gsapps-test.cpp", CXX2, cflags="-Iscreens"),
    ]
    for name in ("steamwin", "steamplace", "steamglue", "steamshrink", "cdpwire"):
        out.append(single(f"screens/tests/{name}-test.cpp", CXX2, cflags="-Iscreens"))
    out += [
        P("gscontrol-test", "screens/tests/gscontrol-test.cpp", "screens/gsforward.cpp", flags=CXX2, cflags="-Iscreens",
          pkgs=XCB),
        single("screens/tests/gstheater-test.cpp", CXX2, cflags="-Iscreens"),
        P("gslink-test", "screens/tests/gslink-test.cpp", "screens/gslink.cpp", "screens/gsforward.cpp",
          flags=f"{CXX2} -pthread", cflags="-Iscreens", pkgs=XCB),
    ]
    for name in ("universefix", "posemath", "roomshift", "layoutgrab", "tilekeep", "chromepool", "vrcmd", "windowdrag", "remoteaway"):
        out.append(single(f"screens/tests/{name}-test.cpp", CXX2))
    out.append(single("screens/tests/wallimage-test.cpp", CXX2, watch=("screens/handrender.cpp",)))   # reads it
    # stb_truetype.h, pinned, fetched once (prepare()); without it (no network) only this
    # test fails
    if has("build/include/stb_truetype.h"):
        out.append(single("screens/tests/glyphs-test.cpp", CXX2, cflags="-I. -isystem build/include"))
    else:
        out.append(Fail("screens/tests/glyphs-test.cpp: FAILED (no stb_truetype.h: scripts/stb.sh couldn't fetch it)"))
    out += [
        single("screens/tests/overlaypanel-test.cpp", CXX2),
        single("screens/tests/gpuctx-test.cpp", CXX2),
        single("screens/tests/logfile-test.c", C11),
        single("conf/test/ftsock-test.c", GNU11),
        P("ftsock-test-cxx", "conf/test/ftsock-test.c", flags=CXX, cflags="-x c++", test="conf/test/ftsock-test.c (as C++)"),
        # hands: the skeleton fit needs no model runtime; sides, pinch and flagbuf link ncnn, which only
        # hands/build.sh builds (minutes, and a download), so they run where it's built
        P("skeleton-test", "hands/tests/skeleton_test.cpp", "hands/track/skeleton.cpp",
          flags=f"{CXX2} -Wno-unused-parameter -Wno-psabi"),
        single("hands/tests/shm_test.cpp", CXX2, out="shm-test"),
        P("calibfile-test", "hands/tests/calibfile_test.cpp", "hands/track/calibfile.cpp",
          flags=f"{CXX2} -Wno-unused-parameter"),
        single("hands/tests/xrcams_test.c", GNU11, out="xrcams-test"),
        single("hands/tests/camstart_test.c", GNU11, out="camstart-test"),
    ]
    # ft-powerd itself has no test (its standby rules do: power/standby-test.cpp); it talks to
    # SteamVR and the display, so it's only compiled, against the pinned OpenVR header
    # power/build.sh fetches (no download in a test)
    if has("power/build/include/openvr.h"):
        out.append(P("ft-powerd", "power/ft-powerd.cpp", flags="-std=c++17 -Wall -Werror -Wno-unused-parameter",
                     cflags="-Ipower/build/include", test="power/ft-powerd.cpp (compiles)", syntax=True))
    else:
        out.append(Skip("power/ft-powerd.cpp (compiles): SKIPPED: no power/build/include/openvr.h "
                        "(power/build.sh fetches it)"))
    if has("hands/build/ncnn/install/lib/libncnn.a"):
        make = ("make", "-s", "-C", "hands", "build/sides-test", "build/pinch-test", "build/flagbuf-test")
        out += [Check(f"hands/tests/{n}_test.cpp", (f"hands/build/{n}-test",), prep=make, watch=("hands",))
                for n in ("sides", "pinch", "flagbuf")]
    else:
        out.append(Skip("hands/tests/{sides,pinch,flagbuf}_test.cpp: SKIPPED: they link ncnn, which isn't built here "
                        "(hands/build.sh)"))
    # ft-gaze's eye-server.mmap layout detection: the pinned OpenVR header gaze/build.sh fetches (no download
    # in a test), and an OpenVR API library to link (OPENVR_LIB in the image)
    if not has("gaze/build/include/openvr.h"):
        out.append(Skip("gaze/test/mmap-layout-test.cpp: SKIPPED: no gaze/build/include/openvr.h (gaze/build.sh fetches it)"))
    elif not os.path.exists(f"{openvr}/libopenvr_api.so"):
        out.append(Skip(f"gaze/test/mmap-layout-test.cpp: SKIPPED: no libopenvr_api.so in {openvr}"))
    else:
        out.append(P("mmap-layout-test", "gaze/test/mmap-layout-test.cpp",
                     flags="-std=c++17 -O2 -Wall -Wno-unused-parameter -Wno-missing-field-initializers",
                     cflags="-Igaze/build/include -Ipointer/common",
                     libs=f"-L{shlex.quote(openvr)} -lopenvr_api -Wl,-rpath,{shlex.quote(openvr)} -lpthread"))
    return out


def _quic(root, openvr, has):
    """Together's links and the loopback tests: msquic, which together/build.sh fetches and builds."""
    if not has("together/build/libmsquic.so.2"):
        return [Skip("together/tests/{links-test.cpp,loopback-test.py,pcsend-test.py,pcfeed-test.py,roompanels-test.py,"
                     "roomcarry-test.py,roompcs-test.py,roomvoice-test.py,roomaway-test.py,roomcontrol-test.py,"
                     "roomgaze-test.py,roominput-test.py,roommove-test.py,roompoints-test.py,panelhand-test.py}: "
                     "SKIPPED: no together/build/libmsquic.so.2 (together/build.sh builds it)")]
    quic = dict(cflags="-Itogether/build/msquic/src/inc",
                libs=f"together/build/libmsquic.so.2 -Wl,-rpath,{shlex.quote(root + '/together/build')} -lpthread", pkglibs="openssl")
    out = [
        P("together-links-test", "together/tests/links-test.cpp", *NET, flags=ASAN, **quic),
        P("ft-together", "together/ft-together.cpp", *NET, *SESSION, "together/flow/video.cpp",
          "together/flow/pcpanels.cpp", "together/flow/looks.cpp", "together/flow/control.cpp", "together/flow/moves.cpp",
          "together/local/watchers.cpp", "together/local/media.cpp", "together/local/wardrobe.cpp",
          "together/local/inputs.cpp", "together/local/controlask.cpp", "together/local/moveask.cpp",
          "together/local/gazeask.cpp", "avatar/modelfile.cpp", flags=ASAN, test=False, **quic),
        py("together/tests/loopback-test.py", "ft-together"),
        # a PC's sender (with canned pictures) at one of them, and its panels at the other
        P("ft-pcsend", "together/ft-pcsend.cpp", *NET, "together/proto/messages.cpp", flags=ASAN, test=False, **quic),
        py("together/tests/pcsend-test.py", "ft-together", "ft-pcsend"),
        # ...and with a streaming host's pictures, which the test hands it
        py("together/tests/pcfeed-test.py", "ft-together", "ft-pcsend"),
        # ft-roompanels between two of them, each with a stand-in ft-screens. (panels-test.py, the video through
        # the hardware encoder and decoder, is run by hand: together/README.md.)
        py("together/tests/roompanels-test.py", "ft-together"),
        # ...and panels moved by someone other than their owner
        py("together/tests/roomcarry-test.py", "ft-together"),
        # ...and the windows of a PC shared as the room's panels, the PC a stand-in that runs ft-pcsend
        py("together/tests/roompcs-test.py", "ft-together", "ft-pcsend"),
        P("ft-roomvoice", "together/ft-roomvoice.cpp", "together/voice/codec.cpp", "together/voice/voices.cpp",
          "together/voice/sender.cpp", "together/voice/player.cpp", "together/voice/headask.cpp",
          "together/voice/pulse.cpp", "avatar/room/client.cpp", "avatar/room/people.cpp", flags=ASAN, test=False,
          pkgs="opus libpulse-simple", libs="-lpthread"),
        py("together/tests/roomvoice-test.py", "ft-together", "ft-roomvoice"),
        py("together/tests/roomaway-test.py", "ft-together"),
        py("together/tests/roomcontrol-test.py", "ft-together"),
        # ft-roomgaze next to one of them, with stand-ins for the pointer helper and the gaze service
        py("together/tests/roomgaze-test.py", "ft-together"),
        # ft-roominput next to one of them, with stand-ins for ft-roompanels and ft-screens
        py("together/tests/roominput-test.py", "ft-together"),
        # ft-roommove next to one of them and a real ft-roompanels, with a stand-in ft-screens
        py("together/tests/roommove-test.py", "ft-together"),
    ]
    # ft-roompoints at one of three of them, with a stand-in for ft-roompanels. It draws through SteamVR, so it
    # wants OpenVR's header (together/build.sh fetches it) and library to build; with --probe it draws nothing.
    if has("together/build/include/openvr.h") and os.path.exists(f"{openvr}/libopenvr_api.so"):
        out += [
            P("ft-roompoints", "together/ft-roompoints.cpp", "together/points/asker.cpp", "together/points/dots.cpp",
              flags=ASAN, test=False, cflags="-isystem together/build/include",
              libs=f"-L{shlex.quote(openvr)} -lopenvr_api -Wl,-rpath,{shlex.quote('/opt/steamvr/bin/linuxarm64:' + openvr)}"),
            py("together/tests/roompoints-test.py", "ft-together", "ft-roompoints"),
        ]
    else:
        out.append(Skip("together/tests/roompoints-test.py: SKIPPED: no together/build/include/openvr.h or no "
                        f"libopenvr_api.so in {openvr}"))
    out += [
        # ft-peerpanel's hand without its decoder, from one of them to the other's desktop
        P("together-handwords", "together/tests/handwords.cpp", flags=ASAN, test=False),
        py("together/tests/panelhand-test.py", "ft-together", "together-handwords"),
    ]
    return out
