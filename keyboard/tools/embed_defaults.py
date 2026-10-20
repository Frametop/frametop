#!/usr/bin/env python3
"""Embed shipped JSON profiles so damaged user files cannot remove the safe defaults."""
import json
from pathlib import Path
import sys

root, output = map(Path, sys.argv[1:])
lines = ['// Generated from bundled JSON; do not edit.', '#pragma once',
         '#include <string_view>', 'namespace framekeyboard::bundled {',
         'struct Resource { std::string_view kind, json; };',
         'inline constexpr Resource profiles[] = {']
for kind in ('layouts', 'languages', 'themes'):
    for path in sorted((root / kind).glob('*.json')):
        # Encode as a C++ string, preserving UTF-8 while escaping quotes and controls.
        content = json.dumps(json.loads(path.read_text()), ensure_ascii=False)
        lines.append('{' + json.dumps(kind) + ', ' + json.dumps(content, ensure_ascii=False) + '},')
lines += ['};', '}']
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text('\n'.join(lines) + '\n')
