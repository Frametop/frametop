"""The docs' references, offline: every relative Markdown link in a tracked .md file names a file
that's in the repo (and a heading there, for a #anchor), and every repo path a doc or a source
comment names as `dir/.../file.md` (or, in a doc, as `dir/file.ext` in backticks) exists.

A path may be relative to the file's folder or any folder above it (keyboard/ is vendored with its
own root). Skipped: frame-hands/... (the separate frame-hands project's notes, not this repo),
paths with a placeholder (*, <, $, ~, {), build outputs (a build/ folder, a .venv), a path followed by
"removed" (a file a change took out), made-up paths in tests' fixtures, and the series' history in docs/maintainability."""
import os
import pathlib
import re
import subprocess
import unittest
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[2]
EXTERNAL = ("frame-hands/",)
LINK = re.compile(r"\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
TICKED = re.compile(r"`([A-Za-z0-9_.-]+/[A-Za-z0-9_./@-]*\.[A-Za-z0-9]+)`")
MD_REF = re.compile(r"(?<![\w./~$-])([A-Za-z0-9_-]+/[A-Za-z0-9_./-]*\.md)\b")
FENCE = re.compile(r"^\s*(```|~~~)")


def tracked():
    """The repo's files, or None without git (a checkout mounted without its .git). The dev
    container runs as root, so the checkout is someone else's: safe.directory says it's fine."""
    try:
        r = subprocess.run(["git", "-c", f"safe.directory={ROOT}", "ls-files", "-z"], cwd=ROOT,
                           capture_output=True, timeout=60)
    except OSError:
        return None
    if r.returncode != 0:
        return None
    return [p for p in r.stdout.decode().split("\0") if p and (ROOT / p).is_file()]


def slug(heading):
    """GitHub's anchor for a heading: lower case, punctuation dropped, spaces to dashes."""
    text = re.sub(r"`|\*\*|\[([^\]]*)\]\([^)]*\)", r"\1", heading.strip()).lower()
    return re.sub(r"[^\w\- ]", "", text).replace(" ", "-")


def anchors(path, cache={}):
    if path not in cache:
        seen, found, fence = {}, set(), False
        for line in path.read_text(errors="replace").splitlines():
            if FENCE.match(line):
                fence = not fence
            elif not fence and (m := re.match(r"#{1,6}\s+(.*?)\s*#*\s*$", line)):
                s = slug(m.group(1))
                n = seen.get(s, 0)
                seen[s] = n + 1
                found.add(s if n == 0 else f"{s}-{n}")
        found |= set(re.findall(r'<a (?:name|id)="([^"]+)"', path.read_text(errors="replace")))
        cache[path] = found
    return cache[path]


def prose(text):
    """The text outside fenced code blocks, with line numbers."""
    fence = False
    for n, line in enumerate(text.splitlines(), 1):
        if FENCE.match(line):
            fence = not fence
        elif not fence:
            yield n, line


class DocLinks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.files = tracked()
        if cls.files is None:
            raise unittest.SkipTest("no git here to list the repo's files")
        cls.tops = {p.split("/")[0] for p in cls.files if "/" in p}
        # Only what's in the repo counts: a file left on one machine isn't in a clone.
        cls.known = set(cls.files) | {"/".join(p.split("/")[:i]) for p in cls.files for i in range(1, p.count("/") + 1)}

    def in_repo(self, path):
        path = pathlib.Path(os.path.normpath(path))
        return (path == ROOT or ROOT in path.parents) and (path == ROOT or str(path.relative_to(ROOT)) in self.known)

    def exists(self, base, target):
        return any(self.in_repo(d / target) for d in (base, *base.parents) if d == ROOT or ROOT in d.parents)

    def placeholder(self, target):
        return (any(c in target for c in "*<>${}~") or target.startswith(EXTERNAL)
                or "build" in target.split("/")[:-1] or ".venv" in target.split("/"))

    def test_markdown_links(self):
        bad = []
        for rel in (p for p in self.files if p.endswith(".md")):
            path = ROOT / rel
            for n, line in prose(path.read_text(errors="replace")):
                for target in LINK.findall(re.sub(r"`[^`]*`", "", line)):
                    if re.match(r"[a-z][a-z0-9+.-]*:", target) or self.placeholder(target):
                        continue
                    file, _, frag = target.partition("#")
                    dest = pathlib.Path(os.path.normpath(path.parent / urllib.parse.unquote(file))) if file else path
                    if not self.in_repo(dest):
                        bad.append(f"{rel}:{n}: {target} (no such file)")
                    elif frag and dest.suffix == ".md" and urllib.parse.unquote(frag).lower() not in anchors(dest):
                        bad.append(f"{rel}:{n}: {target} (no such heading)")
        self.assertEqual(bad, [], "\n" + "\n".join(bad))

    def test_named_paths(self):
        bad = []
        for rel in self.files:
            path = ROOT / rel
            if {"test", "tests"} & set(rel.split("/")[:-1]) or rel.startswith("docs/maintainability/"):
                continue
            if path.suffix in {".png", ".webp", ".gif", ".jpg", ".npz", ".bin", ".onnx", ".zip"}:
                continue
            try:
                text = path.read_text()
            except (UnicodeDecodeError, OSError):
                continue
            lines = prose(text) if rel.endswith(".md") else enumerate(text.splitlines(), 1)
            for n, line in lines:
                refs = [(m.group(1), m.end()) for m in MD_REF.finditer(line)]
                if rel.endswith(".md"):
                    refs += [(m.group(1), m.end()) for m in TICKED.finditer(line)]
                for target, end in refs:
                    if target.split("/")[0] not in self.tops or self.placeholder(target):
                        continue
                    if "removed" in line[end:end + 40]:  # "(`screens/keyboard.cpp`, removed in a cleanup)"
                        continue
                    if not self.exists(path.parent, target):
                        bad.append(f"{rel}:{n}: {target}")
        self.assertEqual(bad, [], "\n" + "\n".join(bad))


if __name__ == "__main__":
    unittest.main()
