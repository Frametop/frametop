#!/usr/bin/env python3
"""models/catalog.py: the catalogs in the repo are valid and agree with hands/models.pin, a bad catalog
is refused whole, downloads keep only files that match, and settings resolve as the runtimes expect.
No network: downloads go to a fake urlopen. Run: python3 models/test/test_catalog.py"""
import copy
import hashlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import catalog  # noqa: E402

REPO = HERE.parents[1]


def sha(b):
    return hashlib.sha256(b).hexdigest()


FILES = {"model.json": b'{"name": "eyes-t1"}', "model.ncnn.param": b"param", "model.ncnn.bin": b"weights",
         "NOTICE": b"notice", "LICENSE": b"licence", "README.md": b"card"}
REV = "a" * 40


def eye_catalog(**over):
    cat = {"catalog": 1, "kind": "eyes", "latest": "eyes-t1", "default": "classic", "baseline": "steamvr",
           "metrics": [{"key": "fresh", "label": "Fresh", "unit": "°", "better": "lower"}],
           "models": [
               {"name": "classic", "title": "Classic", "kind": "classic", "status": "builtin", "bench": {"fresh": 2.8}},
               {"name": "steamvr", "title": "SteamVR", "kind": "valve", "status": "builtin"},
               {"name": "eyes-t1", "title": "T1", "kind": "custom", "status": "testing", "format": 1, "rev": REV,
                "files": {k: sha(v) for k, v in FILES.items()}, "bench": {"fresh": 1.6}},
               {"name": "eyes-t0", "title": "T0", "kind": "custom", "status": "older", "format": 1, "rev": REV,
                "files": {k: sha(v) for k, v in FILES.items()}},
           ]}
    cat.update(over)
    return cat


class FakeHF:
    """urlopen for huggingface.co/DeeJanuz/frametop-eyes-models/resolve/REV/...: serves FILES."""

    def __init__(self, files=FILES):
        self.files, self.urls = dict(files), []

    def __call__(self, url, timeout=None):
        self.urls.append(url)
        prefix = f"https://huggingface.co/{catalog.SPECS['eyes']['hf']}/resolve/{REV}/"
        assert url.startswith(prefix), url
        name = url[len(prefix):].split("/")[-1]
        return io.BytesIO(self.files[name])


class RepoCatalogs(unittest.TestCase):
    def test_bundled_valid(self):
        for kind in catalog.SPECS:
            cat = catalog.bundled(kind)
            self.assertEqual(cat["kind"], kind)

    def test_hands_latest_matches_models_pin(self):
        """hands/models.sh fetches latest by models.pin; the catalog's latest must be the same files."""
        pin = {"files": {}}
        for line in (REPO / "hands" / "models.pin").read_text().splitlines():
            parts = line.split()
            if not parts or parts[0].startswith("#"):
                continue
            if parts[0] == "file":
                pin["files"][parts[1]] = parts[2]
            else:
                pin[parts[0]] = parts[1]
        cat = catalog.bundled("hands")
        e = catalog.entry(cat, cat["latest"])
        self.assertEqual((e["name"], e["rev"], e["files"]), (pin["model"], pin["rev"], pin["files"]))

    def test_texts_have_no_markup(self):
        """Shown as plain text, but keep the catalogs free of markup anyway."""
        for kind in catalog.SPECS:
            for e in catalog.bundled(kind)["models"]:
                for k in ("title", "summary", "notes", "cost"):
                    self.assertNotIn("<", e.get(k, ""), (kind, e["name"], k))


class Validate(unittest.TestCase):
    def bad(self, cat):
        with self.assertRaises(catalog.CatalogError):
            catalog.validate(cat, "eyes")

    def test_good(self):
        catalog.validate(eye_catalog(), "eyes")

    def test_refusals(self):
        c = eye_catalog()
        self.bad(dict(c, kind="hands"))
        self.bad(dict(c, catalog=2))
        self.bad(dict(c, latest="classic"))          # latest must be downloadable
        self.bad(dict(c, latest="nope"))
        self.bad(dict(c, default="nope"))
        for change in (
            lambda m: m.update(name="../x"),
            lambda m: m.update(name="a/b"),
            lambda m: m.update(rev="abc"),
            lambda m: m["files"].update({"run.sh": "0" * 64}),           # not a model file
            lambda m: m["files"].update({"model.json": "xyz"}),          # not a sha256
            lambda m: m["files"].pop("model.ncnn.bin"),                  # a required file missing
            lambda m: m.update(bench={"unknown": 1}),
            lambda m: m.update(bench={"fresh": "1"}),
            lambda m: m.update(status="best"),
            lambda m: m.update(notes="x" * 5000),
            lambda m: m.update(format=0),
        ):
            c2 = copy.deepcopy(c)
            change(c2["models"][2])
            self.bad(c2)
        c2 = copy.deepcopy(c)
        c2["models"][0]["rev"] = REV                 # a builtin has nothing to fetch
        self.bad(c2)
        c2 = copy.deepcopy(c)
        c2["models"].append(copy.deepcopy(c2["models"][2]))
        self.bad(c2)                                 # listed twice


class Store(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.tree = Path(self.tmp.name)
        self.cat = eye_catalog()
        env = {"XDG_CACHE_HOME": str(self.tree / "cache"), "FT_EYES_MODEL": "", "FT_EYES_MODELS_UPDATE": ""}
        self.env = mock.patch.dict(os.environ, env)
        self.env.start()
        self.cache = mock.patch.object(catalog, "CACHE", self.tree / "cache" / "frametop" / "models")
        self.cache.start()

    def tearDown(self):
        self.cache.stop()
        self.env.stop()
        self.tmp.cleanup()

    def fetch(self, name="eyes-t1", hf=None):
        hf = hf or FakeHF()
        with mock.patch.object(catalog.urllib.request, "urlopen", hf):
            return catalog.fetch("eyes", name, tree=self.tree, cat=self.cat), hf

    def test_fetch_and_verify(self):
        d, hf = self.fetch()
        self.assertEqual(d, self.tree / "gaze/tracker/models/eyes-t1")
        self.assertEqual({p.name for p in d.iterdir()}, set(FILES) | {"PIN.json"})
        self.assertEqual(catalog.verify("eyes", "eyes-t1", self.tree), [])
        self.assertTrue(any(u.endswith(f"/{REV}/LICENSE") for u in hf.urls))     # top files from the top
        self.assertTrue(any(u.endswith(f"/{REV}/eyes-t1/model.ncnn.bin") for u in hf.urls))
        _, again = self.fetch()                     # already there and matching: nothing downloaded
        self.assertEqual(again.urls, [])
        (d / "model.ncnn.bin").write_bytes(b"changed")
        self.assertEqual(catalog.verify("eyes", "eyes-t1", self.tree), ["model.ncnn.bin"])

    def test_bad_file_not_kept(self):
        with self.assertRaises(catalog.CatalogError):
            self.fetch(hf=FakeHF(dict(FILES, **{"model.ncnn.bin": b"tampered"})))
        self.assertIsNone(catalog.installed("eyes", "eyes-t1", self.tree))
        new = self.tree / "gaze/tracker/models/.eyes-t1.new"
        self.assertFalse(any(p.name.endswith(".part") or p.name == "model.ncnn.bin" for p in new.iterdir()))

    def test_builtin_and_unknown_not_fetched(self):
        for name in ("classic", "nope"):
            with self.assertRaises(catalog.CatalogError):
                self.fetch(name)

    def test_newer_format_not_fetched(self):
        self.cat["models"][2]["format"] = catalog.SPECS["eyes"]["format"] + 1
        with self.assertRaises(catalog.CatalogError):
            self.fetch()

    def test_resolve(self):
        r = lambda v: catalog.resolve("eyes", v, self.tree, self.cat)  # noqa: E731
        self.assertEqual(r("")["name"], "classic")              # unset: the catalog's default
        self.assertIsNone(r("classic")["folder"])
        self.assertIsNone(r("latest")["folder"])                 # nothing downloaded: the pupil finder
        self.assertIsNone(r("eyes-t1")["folder"])
        self.assertEqual(r("/some/folder")["folder"], "/some/folder")
        self.fetch("eyes-t0")
        self.assertEqual(r("latest")["name"], "eyes-t0")         # latest missing: a downloaded custom one
        self.fetch("eyes-t1")
        self.assertEqual(r("latest")["folder"], str(self.tree / "gaze/tracker/models/eyes-t1"))
        self.assertEqual(r("eyes-t0")["name"], "eyes-t0")

    def test_hands_latest_is_student(self):
        cat = catalog.bundled("hands")
        self.assertIsNone(catalog.resolve("hands", "latest", self.tree, cat)["folder"])
        self.assertIsNone(catalog.resolve("hands", "stock", self.tree, cat)["folder"])

    def test_channel(self):
        self.assertIsNone(catalog.channel(self.tree))
        (self.tree / ".git").mkdir()
        (self.tree / ".git" / "HEAD").write_text("ref: refs/heads/experimental\n")
        self.assertEqual(catalog.channel(self.tree), "experimental")
        (self.tree / ".git" / "HEAD").write_text("ref: refs/heads/my-branch\n")
        self.assertIsNone(catalog.channel(self.tree))
        (self.tree / ".frametop-release").write_text("VERSION=x\nCHANNEL=stable\n")
        self.assertEqual(catalog.channel(self.tree), "main")

    def test_auto_off(self):
        with mock.patch.dict(os.environ, {"FT_EYES_MODELS_UPDATE": "0"}):
            self.assertIn("not updating", catalog.auto("eyes", self.tree))

    def test_auto_fetches_setting(self):
        (self.tree / "gaze/tracker").mkdir(parents=True)
        (self.tree / "gaze/tracker/models.json").write_text(json.dumps(self.cat))
        with mock.patch.dict(os.environ, {"FT_EYES_MODEL": "latest"}), \
                mock.patch.object(catalog, "refresh", lambda kind, tree: (False, "offline")), \
                mock.patch.object(catalog.urllib.request, "urlopen", FakeHF()):
            self.assertIn("fetched eyes-t1", catalog.auto("eyes", self.tree))
            self.assertIn("is here", catalog.auto("eyes", self.tree))

    def test_cached_catalog_only_for_its_branch(self):
        (self.tree / "gaze/tracker").mkdir(parents=True)
        (self.tree / "gaze/tracker/models.json").write_text(json.dumps(self.cat))
        newer = eye_catalog(latest="eyes-t0")
        catalog.CACHE.mkdir(parents=True)
        (catalog.CACHE / "eyes.json").write_text(json.dumps({"branch": "experimental", "fetched": 0, "catalog": newer}))
        self.assertEqual(catalog.load("eyes", self.tree)[0]["latest"], "eyes-t1")   # not on a branch: its own
        (self.tree / ".frametop-release").write_text("CHANNEL=experimental\n")
        cat, source = catalog.load("eyes", self.tree)
        self.assertEqual((cat["latest"], source), ("eyes-t0", "github experimental"))
        (catalog.CACHE / "eyes.json").write_text(json.dumps({"branch": "experimental", "fetched": 0,
                                                              "catalog": dict(newer, latest="classic")}))
        self.assertEqual(catalog.load("eyes", self.tree)[0]["latest"], "eyes-t1")   # invalid cache: ignored


if __name__ == "__main__":
    unittest.main()
