"""Private config only. Layer preference is separate from geometry and visibility."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/"layout"))
import ft_layout

class PortalLayerTests(unittest.TestCase):
    def test_default_strict_boolean_and_atomic_preservation(self):
        self.assertTrue(callable(getattr(ft_layout, "portal_above_steam", None)), "missing layer preference")
        for value in (None, 0, 1, "true", [], {}):
            self.assertFalse(ft_layout.portal_above_steam({"portal_above_steam": value}))
        self.assertFalse(ft_layout.portal_above_steam({}))
        self.assertTrue(ft_layout.portal_above_steam({"portal_above_steam": True}))
        with tempfile.TemporaryDirectory(dir=os.environ["TMPDIR"]) as directory:
            path=Path(directory)/"layout.json"
            saved={"passthrough":dict(ft_layout.PASSTHROUGH,pitch=23,roll=-31),
                   "screens":{"1":{"width":2}},"unknown":{"keep":True},"passthrough_controls":True}
            path.write_text(json.dumps(saved))
            with patch.object(ft_layout,"LAYOUT_PATH",str(path)), patch.object(ft_layout,"LOCK_PATH",str(path)+".lock"):
                self.assertIs(ft_layout.load_layout()["portal_above_steam"],False)
                for enabled in (True,False):
                    ft_layout.save_portal_above_steam(enabled)
                    self.assertEqual(json.loads(path.read_text()),dict(saved,portal_above_steam=enabled))
                for bad in (1,"false",None):
                    with self.assertRaises(ValueError):ft_layout.save_portal_above_steam(bad)
    def test_typed_reply_and_matching_readback(self):
        self.assertTrue(callable(getattr(ft_layout,"send_portal_above_steam",None)), "missing native layer client")
        class Socket:
            def __init__(self):self.commands=[];self.enabled=False
            def ask(self, text, **kw):
                self.commands.append(text)
                if text!="passthrough layer state":self.enabled=text=="passthrough layer 1"
                return "ok layer "+str(int(self.enabled))
        sock=Socket()
        for enabled in (True,False):
            self.assertIs(ft_layout.send_portal_above_steam(sock,enabled),enabled)
        self.assertEqual(sock.commands,["passthrough layer 1","passthrough layer state","passthrough layer 0","passthrough layer state"])
        for reply in ("ok","ok layer 2","ok layer 1 extra","ok layer 01"):
            with self.assertRaises(ValueError):ft_layout.parse_portal_above_steam(reply)
