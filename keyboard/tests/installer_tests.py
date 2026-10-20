#!/usr/bin/env python3
"""Exercise the real installer with a host binary and an isolated install root."""
import os
import fcntl
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]).resolve()
binary = Path(sys.argv[2]).resolve()

with tempfile.TemporaryDirectory(prefix="full-keyboard-installer-") as directory:
    root = Path(directory)
    package = root / "package"
    (package / "bin").mkdir(parents=True)
    shutil.copy2(source / "scripts/install-local.sh", package / "install.sh")
    for kind in ("layouts", "languages", "themes"):
        shutil.copytree(source / kind, package / "share/framekeyboard" / kind)
    (package / "share/framekeyboard/update").mkdir()
    shutil.copy2(source / "update/update_helper.py",
                 package / "share/framekeyboard/update/update_helper.py")
    # Keep --check real. Only version and target architecture are test fixtures.
    wrapper = package / "bin/framekeyboard"
    wrapper.write_text('''#!/bin/sh
if [ "$1" = --version ]; then
    echo "framekeyboard $(cat "$(dirname "$0")/version")"
else
    exec "$TEST_BINARY" "$@"
fi
''')
    wrapper.chmod(0o755)
    version = package / "bin/version"
    version.write_text("9.0.1")
    commands = root / "commands"
    commands.mkdir()
    (commands / "uname").write_text('#!/bin/sh\necho aarch64\n')
    real_mv = shutil.which("mv")
    real_cp = shutil.which("cp")
    (commands / "cp").write_text('''#!/bin/sh
"$REAL_CP" "$@" || exit $?
if [ "${ASSERT_INSTANCE_LOCK:-}" = 1 ]; then
    python3 "$LOCK_CHECK" || exit $?
fi
[ "${FAIL_COPY:-}" != 1 ]
''')
    (commands / "check-lock.py").write_text('''import fcntl,os
from pathlib import Path
lock=(Path(os.environ['FRAMEKEYBOARD_INSTALL_RUNTIME_DIR'])/'vr.lock').open('a')
try:
    fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
except BlockingIOError:
    raise SystemExit(0)
raise SystemExit('installer did not hold instance exclusion during copy')
''')
    (commands / "mv").write_text('''#!/bin/sh
for last do :; done
fixture_current="$(realpath -m -- "$FRAMEKEYBOARD_INSTALL_HOME/.local/share/framekeyboard")/current"
if [ "${FAIL_ACTIVATION:-}" = 1 ] && [ "$last" = "$fixture_current" ]; then
    exit 1
fi
"$REAL_MV" "$@" || exit $?
if [ "${STOP_AFTER_SWITCH:-}" = 1 ] && [ "$last" = "$fixture_current" ]; then
    kill -TERM "$PPID"
fi
''')
    for path in commands.iterdir():
        path.chmod(0o755)
    install_home = root / "install"
    config = install_home / ".config/framekeyboard/config.json"
    config.parent.mkdir(parents=True)
    config.write_text('user configuration must remain untouched')
    runtime = root / "runtime"
    runtime.mkdir(mode=0o700)
    env = dict(
        os.environ,
        FRAMEKEYBOARD_INSTALL_HOME=str(install_home),
        FRAMEKEYBOARD_INSTALL_RUNTIME_DIR=str(runtime),
        TEST_BINARY=str(binary),
        REAL_MV=real_mv,
        REAL_CP=real_cp,
        LOCK_CHECK=str(commands / "check-lock.py"),
        PATH=f"{commands}:{os.environ['PATH']}",
    )
    releases = install_home / ".local/share/framekeyboard/releases"
    current = releases.parent / "current"

    def install(success, package_override=None, **extra):
        arguments = ["bash", str(package / "install.sh")]
        if package_override:
            arguments.append(str(package_override))
        result = subprocess.run(
            arguments,
            env=dict(env, **extra), capture_output=True, text=True,
        )
        assert (result.returncode == 0) == success, result.stdout + result.stderr
        assert config.read_text() == 'user configuration must remain untouched'
        assert not list(releases.glob(".install-*")), "failed install left staging directory"
        assert not list(releases.parent.glob(".links.*")), "temporary links leaked"
        return result

    theme = package / "share/framekeyboard/themes/graphite.json"
    good_theme = theme.read_bytes()
    theme.write_text('{broken')
    install(False)
    assert not (releases / "9.0.1").exists() and not current.exists()
    theme.write_bytes(good_theme)
    install(True)
    first = current.resolve()
    assert first.name.startswith("9.0.1-install-")
    assert (first / ".full-keyboard-production.json").is_file()
    install(True)  # Reinstallation stages a fresh copy of the same version.
    second = current.resolve()
    assert second != first and not first.exists()
    assert (second / "bin/version").read_text() == "9.0.1"
    dev = releases / "dev-unmarked"
    dev.mkdir()
    (dev / "source").write_text("manual deployment")
    moved = releases / "dev-copy"
    moved.mkdir()
    shutil.copy2(second / ".full-keyboard-production.json", moved / ".full-keyboard-production.json")
    (moved / "source").write_text("copied development deployment")
    previous = releases.parent / "previous"
    previous.symlink_to(os.readlink(current))
    version.write_text("9.0.2")
    theme.write_text('{broken')
    install(False)
    assert current.resolve() == second and not list(releases.glob("9.0.2-*"))
    theme.write_bytes(good_theme)
    install(False, FAIL_COPY="1")
    assert current.resolve() == second and not list(releases.glob("9.0.2-*"))
    install(False, FAIL_ACTIVATION="1")
    assert current.resolve() == second and not list(releases.glob("9.0.2-*"))
    install(True, ASSERT_INSTANCE_LOCK="1")
    third = current.resolve()
    assert third.name.startswith("9.0.2-install-") and not second.exists()
    assert not previous.is_symlink(), "known deleted production previous link was retained"
    assert (dev / "source").read_text() == "manual deployment"
    assert (moved / "source").read_text() == "copied development deployment"
    previous.symlink_to("releases/dev-unmarked")
    # Use the modern installer for an older package, rather than its legacy script/helper.
    legacy = root / "legacy"
    shutil.copytree(package, legacy)
    (legacy / "bin/version").write_text("9.0.0")
    (legacy / "share/framekeyboard/update/update_helper.py").write_text("raise SystemExit(1)\n")
    install(True, package_override=legacy)
    fourth = current.resolve()
    assert fourth.name.startswith("9.0.0-install-") and not third.exists()
    assert os.readlink(previous) == "releases/dev-unmarked", "unmarked previous link was changed"
    # Acquire the real instance-lock inode in this fixture, never the desktop's lock.
    before = {str(p.relative_to(install_home)): p.read_bytes() for p in install_home.rglob("*")
              if p.is_file() and not p.is_symlink()}
    with (runtime / "vr.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        result = install(False)
        assert "Full Keyboard is running" in result.stderr
    after = {str(p.relative_to(install_home)): p.read_bytes() for p in install_home.rglob("*")
             if p.is_file() and not p.is_symlink()}
    assert before == after and current.resolve() == fourth, "running-instance refusal changed files"
    version.write_text("9.0.3")
    alias = root / "home-alias"
    alias.symlink_to(install_home)
    install(False, STOP_AFTER_SWITCH="1", FRAMEKEYBOARD_INSTALL_HOME=str(alias))
    assert current.exists() and (current / "bin/version").read_text() == "9.0.3", \
        "interrupted activation removed the release now named by current"
    assert fourth.exists(), "interrupted install removed the old recovery release"
    assert "Name=Full Keyboard" in (install_home / ".local/share/applications/framekeyboard.desktop").read_text()
    print("Validation, production replacement/reinstall/downgrade, dev preservation, instance exclusion and interruption passed.")
