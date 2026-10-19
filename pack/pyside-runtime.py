#!/usr/bin/env python3
"""Export Frametop's Fedora PySide bindings, or verify the installed app runtime.

Native Qt, QML, platform/style plugins and python3-shiboken6 remain dnf packages.
Only the unused Python Qt bindings and SDK tool links are omitted; ELF files are
copied unchanged to their original Fedora paths. This is a private app runtime.
"""
import argparse
import ast
import compileall
import hashlib
import importlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

DIRECT = ("QtCore", "QtGui", "QtQml", "QtQuick", "QtQuickControls2")
BACKENDS = ("display-settings/ft_display_settings.py", "input-settings/ft_input_settings.py",
            "remote-displays/ft_remote_displays.py", "hands/rec/ft_handrec.py", "gaze/rec/ft_eyerec.py",
            "gaze/bench/gazebench.py")
MANIFEST = Path("usr/share/frametop/pyside-runtime.json")


def run(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs).strip()


def imports(repo):
    for name in DIRECT:
        importlib.import_module("PySide6." + name)
    for i, name in enumerate(BACKENDS):
        path = repo / name
        sys.path.insert(0, str(path.parent))
        spec = importlib.util.spec_from_file_location(f"_ft_pyside_{i}", path)
        spec.loader.exec_module(importlib.util.module_from_spec(spec))
        sys.path.pop(0)


def check_elf(paths):
    for path in paths:
        if "not found" in run("ldd", str(path)):
            raise RuntimeError(f"unresolved ELF dependency: {path}")


def export(dest, repo):
    imports(repo)
    import PySide6
    from PySide6 import _config
    from PySide6.QtCore import qVersion

    package = Path(PySide6.__file__).parent.resolve()
    modules = {name.split(".")[-1]: Path(m.__file__).resolve()
               for name, m in sys.modules.items() if name.startswith("PySide6.Qt")
               and isinstance(getattr(m, "__file__", None), str) and m.__file__.endswith(".so")}
    if not set(DIRECT) <= modules.keys():
        raise RuntimeError("a required binding was not imported")
    if (dest / package.relative_to("/")).exists() or (dest / MANIFEST).exists():
        raise RuntimeError(f"PySide was already exported into {dest}")
    copied, elf = [], []
    for name in run("rpm", "-ql", "python3-pyside6").splitlines():
        src = Path(name)
        if not src.is_file() and not src.is_symlink():
            continue
        if src.name == "entry_points.txt":
            continue  # this private runtime does not advertise the omitted SDK commands
        if src.is_relative_to(package):
            rel = src.relative_to(package)
            if "__pycache__" in rel.parts or rel.parts[0] in ("Qt", "scripts"):
                continue
            if len(rel.parts) == 1 and rel.name.startswith("Qt") and rel.name.split(".")[0] not in modules:
                continue
            if src.is_symlink() and ".so" not in src.name:
                continue  # SDK command links, not the package's Python/private helpers
        elif not ((src.name.startswith("libpyside6") and ".so" in src.name)
                  or any(part.startswith("PySide6-") and part.endswith((".dist-info", ".egg-info"))
                         for part in src.parts)
                  or name.startswith("/usr/share/licenses/python3-pyside6/")):
            continue
        out = dest / src.relative_to("/")
        if out.exists() or out.is_symlink():
            raise RuntimeError(f"export would replace a file: {out}")
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, out, follow_symlinks=False)
        copied.append(name)
        if ".so" in src.name:
            elf.append(name)
            if not src.is_symlink() and src.read_bytes() != out.read_bytes():
                raise RuntimeError(f"ELF changed during export: {src}")

    # __all__ discovers the available extension files. The build metadata must agree.
    cfg = dest / (package / "_config.py").relative_to("/")
    cfg_stat = cfg.stat()
    text = cfg.read_text()
    nodes = [n for n in ast.parse(text).body if isinstance(n, ast.Assign)
             and any(isinstance(t, ast.Name) and t.id == "built_modules" for t in n.targets)]
    if len(nodes) != 1 or nodes[0].col_offset:
        raise RuntimeError("unrecognized PySide built_modules metadata")
    available = [n for n in _config.built_modules if (n if n.startswith("Qt") else "Qt" + n) in modules]
    if len(available) != len(modules):
        raise RuntimeError("PySide module metadata and imported bindings disagree")
    node, lines = nodes[0], text.splitlines(keepends=True)
    cfg.write_text("".join(lines[:node.lineno - 1]) + f"built_modules = {available!r}\n"
                   + "".join(lines[node.end_lineno:]))
    os.utime(cfg, ns=(cfg_stat.st_atime_ns, cfg_stat.st_mtime_ns))
    staged = dest / package.relative_to("/")
    check_elf([dest / n.lstrip("/") for n in elf])
    probe = ("import PySide6; from PySide6 import _config; "
             f"assert set(PySide6.__all__) == {set(modules)!r}; "
             f"assert _config.built_modules == {available!r}")
    run(sys.executable, "-c", probe, env=dict(os.environ, PYTHONPATH=str(staged.parent),
                                             PYTHONDONTWRITEBYTECODE="1"))
    if not compileall.compile_dir(str(staged), ddir=str(package), quiet=1, optimize=[0, 1]):
        raise RuntimeError("PySide bytecode compilation failed")
    for pyc in staged.rglob("*.pyc"):
        stat = Path(importlib.util.source_from_cache(str(pyc))).stat()
        os.utime(pyc, ns=(stat.st_atime_ns, stat.st_mtime_ns))
    manifest = {"private_app_runtime": True, "pyside": PySide6.__version__, "qt": qVersion(), "modules": sorted(modules),
                "source_rpms": run("rpm", "-q", "python3-pyside6", "python3-shiboken6").splitlines(),
                "metadata_change": "_config.built_modules narrowed; omitted SDK scripts and entry_points.txt",
                "elf": elf, "sha256": {n: hashlib.sha256((dest / n.lstrip("/")).read_bytes()).hexdigest()
                                       for n in copied if not (dest / n.lstrip("/")).is_symlink()}}
    target = dest / MANIFEST
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"modules": sorted(modules), "files": len(copied), "destination": str(dest)}))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    action = p.add_mutually_exclusive_group(required=True)
    action.add_argument("--destination", "--export", dest="export", type=Path,
                        help="staging directory, later copied to / (may already contain /opt)")
    action.add_argument("--verify", type=Path, help="installed pyside-runtime.json")
    p.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    a = p.parse_args()
    if a.export:
        export(a.export.resolve(), a.repo.resolve())
    else:
        manifest = json.loads(a.verify.read_text())
        for path, sha in manifest["sha256"].items():
            if hashlib.sha256(Path(path).read_bytes()).hexdigest() != sha:
                raise RuntimeError(f"runtime file changed: {path}")
        check_elf(manifest["elf"])
        for name in manifest["modules"]:
            importlib.import_module("PySide6." + name)
        imports(a.repo.resolve())
        import PySide6
        from PySide6.QtCore import qVersion
        if qVersion() != manifest["qt"] or run("rpm", "-q", "python3-shiboken6") not in manifest["source_rpms"]:
            raise RuntimeError("runtime Qt/Shiboken version differs from the builder")
        if set(PySide6.__all__) != set(manifest["modules"]):
            raise RuntimeError("runtime module advertisement differs")
        print("PySide runtime: all checks passed")


if __name__ == "__main__":
    main()
