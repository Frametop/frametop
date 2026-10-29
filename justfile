# just — the runner for the actions that happen inside the image.
# The CI smoke job runs these with: docker run ... IMAGE just test
# Host-side image management (build, shell, update) lives in ./ft; these
# recipes only ever run where the toolchain and the venv are: in the image.

# default: list the recipes
default:
    @just --list

# everything, strict (the recipes are the source of truth)
test: test-python test-c test-bash test-testbench test-fast-install

# the Python suites as a strict gate: every failure fails the run. The image's
# python3 has to import the dnf Qt stack and the locked packages together;
# without that check the Qt tests would only skip. session/test is not here:
# test_accessibility.py needs the host's AT-SPI and PyGObject, test_notifyfwd.py
# and test_gtksettings.py the host's dbus-daemon and PyGObject.
test-python:
    #!/usr/bin/env bash
    set -uo pipefail
    python3 -c 'import PySide6, numpy, cv2, sherpa_onnx' || { echo "python3 can't import PySide6, numpy, cv2, and sherpa_onnx"; exit 1; }
    status=0
    layout/countdown/deps.sh || status=1
    for t in layout/countdown/tests/test_*.py models/test/*.py commands/test/*.py input/test/*.py hands/tests/test_*.py hands/rec/tests/*.py recorder/tests/*.py gaze/test/*.py gaze/rec/tests/*.py pack/test/*.py voice/test/*.py frunner/test/*.py tour/test/*.py float/test/*.py; do
        [ -f "$t" ] || continue
        if python3 "$t" >/dev/null; then echo "$t: ok"; else echo "$t: FAILED"; status=1; fi
    done
    pytest -q session/tests || status=1
    exit $status

# the C/C++ unit tests that need no model runtime: header-only logic with
# made-up events (pointer/tests: the helper's hand interaction logic). sides_test.cpp is NOT here: it pulls in ncnn (hands'
# heavy build) and stays with `make check` in the hands build
# (hands/Makefile).
test-c:
    #!/usr/bin/env bash
    set -e
    mkdir -p build/tests
    gcc -std=c11 -Wall -Wextra -Werror -I. screens/tests/controller-click-test.c \
      -lm -o build/tests/controller-click-test
    build/tests/controller-click-test && echo "screens/tests/controller-click-test.c: ok"
    gcc -std=c11 -Wall -Wextra -Werror screens/tests/relay-buttons-test.c \
      -o build/tests/relay-buttons-test
    build/tests/relay-buttons-test && echo "screens/tests/relay-buttons-test.c: ok"
    gcc -std=c11 -Wall -Wextra -Werror screens/tests/held-keys-test.c \
      -o build/tests/held-keys-test
    build/tests/held-keys-test && echo "screens/tests/held-keys-test.c: ok"
    gcc -std=c11 -Wall -Wextra -Werror screens/tests/held-keys-wlr-test.c \
      $(pkg-config --cflags --libs wlroots-0.20 xkbcommon wayland-server) -o build/tests/held-keys-wlr-test
    build/tests/held-keys-wlr-test && echo "screens/tests/held-keys-wlr-test.c: ok"
    g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror screens/tests/handinput_test.cpp \
      screens/handcut.cpp screens/handinput.cpp screens/palmmenu.cpp screens/grabthat.cpp screens/steampanels.cpp -o build/tests/handinput-test
    build/tests/handinput-test && echo "screens/tests/handinput_test.cpp: ok"
    g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror screens/tests/handview_test.cpp \
      screens/handmesh.cpp screens/handskin.cpp screens/handrobot.cpp screens/handcut.cpp -o build/tests/handview-test
    build/tests/handview-test >/dev/null && echo "screens/tests/handview_test.cpp: ok"
    python3 -I screens/tools/hand-asset.py --check
    for t in pointer/tests/*-test.cpp; do
        n=$(basename "$t" .cpp)
        g++ -std=c++17 -Wall -Wextra -Werror -I. "$t" -o "build/tests/$n"
        "build/tests/$n" >/dev/null && echo "$t: ok"
    done
    g++ -std=c++17 -O2 -Wall -Wextra -Werror screens/tests/rounded-test.cpp \
      -o build/tests/rounded-test
    build/tests/rounded-test && echo "screens/tests/rounded-test.cpp: ok"
    g++ -std=c++17 -O2 -Wall -Wextra -Werror screens/tests/trackwatch-test.cpp \
      -o build/tests/trackwatch-test
    build/tests/trackwatch-test >/dev/null && echo "screens/tests/trackwatch-test.cpp: ok"
    g++ -std=c++17 -O2 -Wall -Wextra -Werror screens/tests/levelpin-test.cpp \
      -o build/tests/levelpin-test
    build/tests/levelpin-test >/dev/null && echo "screens/tests/levelpin-test.cpp: ok"

# shell scripts: syntax-gate everything that ships. No git: the image has
# none, and a mounted checkout can trip "dubious ownership" — find lists
# what the repo actually ships. The count guard makes a silently short
# list fail the gate instead of passing it.
test-bash:
    #!/usr/bin/env bash
    set -e
    mapfile -t scripts < <(find . -name '*.sh' -type f \
        -not -path './.git/*' -not -path '*/build/*' | sort)
    [ "${#scripts[@]}" -ge 20 ] || { echo "only ${#scripts[@]} scripts found — the list is broken"; exit 1; }
    status=0
    for f in "${scripts[@]}" ft; do
        bash -n "$f" && echo "bash -n $f: ok" || status=1
    done
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

# lint the Python side (ruff from the locked dev group). Report only for now:
# the codebase has pre-existing findings; making it a gate is its own cleanup.
lint:
    -ruff check .

# the whole gate: lint, then tests
check: lint test
