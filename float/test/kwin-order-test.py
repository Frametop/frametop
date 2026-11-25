#!/usr/bin/env python3
"""frametop-float.js against KWin's order of signals (kwin-order.js, run by node): a snap key or maximizing
leaves a floating window's size alone, in KWin 6.2 and 6.6. Skipped without node.

  float/test/kwin-order-test.py
"""
import os
import shutil
import subprocess
import sys

node = shutil.which("node")
if not node:
    print("no node here")
    sys.exit(77)
sys.exit(subprocess.run([node, os.path.join(os.path.dirname(os.path.realpath(__file__)), "kwin-order.js")]).returncode)
