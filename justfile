# just — the runner for the actions that happen inside the image.
# The CI smoke job runs these with: docker run ... IMAGE just test
# Host-side image management (build, shell, update) lives in ./ft; these
# recipes only ever run where the toolchain and the venv are: in the image.

# default: list the recipes
default:
    @just --list

# one `just test`'s id: two at once in one tree each record their own start
test_run := uuid()

# everything, strict (the recipes are the source of truth); when it passes, the commit it passed
# on is where `just test-changed` starts
test: _mark-start lint test-python test-c test-keyboard test-bash test-testbench test-fast-install
    @python3 scripts/testgate/changed.py --mark-green {{test_run}}

# what `just test` tests: the tree as it starts (scripts/testgate/changed.py)
_mark-start:
    @python3 scripts/testgate/changed.py --mark-start {{test_run}}

# while working: only the tests a change since the last green `just test` (or since BASE)
# reaches, with lint and the shell scripts' check (scripts/testgate/changed.py). `just test`
# is still the gate before a push.
test-changed base="":
    python3 scripts/testgate/changed.py {{base}}

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
    mkdir -p build/tests
    python3 scripts/testgate/testgate.py --times build/tests/times-python.json @scripts/testgate/python-suites.txt || status=1
    pytest -q session/tests || status=1
    # test_accessibility.py needs the host's AT-SPI and PyGObject, test_notifyfwd.py and
    # test_gtksettings.py the host's dbus-daemon and PyGObject
    echo "session/test/*.py: SKIPPED: they need the host's AT-SPI, dbus-daemon and PyGObject (run them on the Frame)"
    exit $status

# the C/C++ unit tests, built into build/tests: scripts/testgate/cprograms.py lists them (header-only
# logic with made-up events, the screens' and avatars' parts, Together under the sanitizers, the
# hands' skeleton fit, Together's loopback tests), and scripts/testgate/ctests.py builds them with
# ninja, each shared source once and only what changed, several at once (FT_TEST_JOBS). A test
# that fails fails the gate, with its output, and so does one that doesn't build; one that can't
# build here (no ncnn, no OpenVR header) says SKIPPED and why.
test-c:
    python3 scripts/testgate/ctests.py

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
