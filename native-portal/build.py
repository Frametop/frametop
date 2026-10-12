#!/usr/bin/env python3
"""Build the native passthrough layer from already generated, private shader files."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=HERE / 'build/current')
    args = parser.parse_args()
    output = args.output.resolve()
    native = json.loads((HERE / 'native-shaders.json').read_text())
    pins = native['shaders']
    report = json.loads((output / 'shaders.json').read_text())['shaders']
    lines = ['#pragma once', '#include <cstdint>', '#include <cstddef>',
             'struct Pin { int id; const uint32_t* data; size_t size; const uint32_t* replacement; size_t replacement_size; };']
    entries = []
    for ident, pin in pins.items():
        for suffix in ('original', 'portal'):
            path = output / f'{ident}.{suffix}.spv'
            data = path.read_bytes()
            expected = pin['sha256'] if suffix == 'original' else report[ident]['sha256']
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError(f'shader hash mismatch: {path}')
            # generate.py validates on the host; compilation only needs those
            # exact bytes, so the container does not need SPIR-V tools again.
            words = struct.unpack('<' + 'I' * (len(data) // 4), data)
            lines.append(f'const uint32_t {suffix}_{ident}[] = {{' + ','.join(map(hex, words)) + '};')
        entries.append(f'{{{ident},original_{ident},sizeof(original_{ident}),portal_{ident},sizeof(portal_{ident})}}')
    lines.append('const Pin pins[] = {' + ','.join(entries) + '};')
    lines.append('const int camera_pairs[][2] = {' +
                 ','.join('{' + ','.join(map(str, pair)) + '}' for pair in native['pairs']) + '};')
    (output / 'pinned.hpp').write_text('\n'.join(lines) + '\n')
    library = output / 'libft_passthrough.so'
    subprocess.run(['c++', '-std=c++17', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fPIC', '-fvisibility=hidden', '-shared',
                    '-static-libstdc++', '-static-libgcc', '-Wl,-Bsymbolic-functions,-z,defs',
                    '-I', str(output), str(HERE / 'layer.cpp'), '-pthread', '-lcrypto', '-ldl',
                    '-o', str(library)], check=True)
    manifest = {'file_format_version': '1.2.0', 'layer': {
        'name': 'VK_LAYER_FT_passthrough', 'type': 'GLOBAL', 'library_path': './libft_passthrough.so',
        'api_version': '1.3.0', 'implementation_version': '1',
        'description': 'Frametop native masked passthrough'}}
    (output / 'VkLayer_FT_passthrough.json').write_text(json.dumps(manifest, indent=2) + '\n')
    subprocess.run(['ldd', '-r', str(library)], check=True)
    gate = output / 'libft_passthrough_gate.so'
    subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-Wno-misleading-indentation', '-fPIC', '-fvisibility=hidden', '-shared',
                    '-static-libstdc++', '-static-libgcc', '-Wl,-z,defs', str(HERE / 'gate.cpp'), '-lcrypto', '-ldl',
                    '-o', str(gate)], check=True)
    subprocess.run(['ldd', '-r', str(gate)], check=True)
    print(library)


if __name__ == '__main__':
    main()
