#!/usr/bin/env python3
"""A skinned glTF hand (a converter's output, or a .gltf) rewritten as a clean GLB that
screens/tools/hand-asset.py reads: one of the steps that made screens/assets/hands/*/*.glb
(each model's README there says which, with which options).

    screens/tools/hand-glb-clean.py IN.(glb|gltf) OUT.glb [--scale S] [--mirror]
        [--identity NODE ...] [--rest-from-bind] [--rename A=B ...]

- collapses assimp's '$AssimpFbx$' helper nodes (keeping the world transforms exact)
- --scale S: a uniform scale (0.01: centimetres to metres) of the vertices, the nodes'
  translations and the inverse bind matrices
- --mirror: across x (the other hand): positions, normals, nodes, inverse bind matrices, and
  the triangles' winding
- --identity NODE: that node's transform dropped (the skinned mesh's node's always is, as glTF
  skinning ignores it)
- --rest-from-bind: every joint's rest pose made its bind pose
- --rename A=B: in node names
- the skin weights renormalised to add up to 1, nodes written as TRS, one plain material

Written for Frametop's hand models (2026-10-09). Standard library only; the input is read as
data, nothing in it is run.
"""
import base64
import json
import math
import os
import struct
import sys


def load(path):
    d = open(path, 'rb').read()
    bins = []
    if d[:4] == b'glTF':
        _, _, length = struct.unpack_from('<4sII', d, 0)
        off = 12; ch = []
        while off < length:
            cl, ct = struct.unpack_from('<II', d, off); ch.append((ct, d[off+8:off+8+cl])); off += 8 + cl
        g = json.loads(ch[0][1]); glbbin = ch[1][1] if len(ch) > 1 else b''
    else:
        g = json.loads(d); glbbin = None
    for b in g.get('buffers', []):
        uri = b.get('uri')
        if uri is None:
            bins.append(glbbin)
        elif uri.startswith('data:'):
            bins.append(base64.b64decode(uri.split(',', 1)[1]))
        else:
            p = os.path.join(os.path.dirname(path), uri)
            bins.append(open(p, 'rb').read() if os.path.exists(p) else None)
    return g, bins

def accessor(g, bins, i):
    a = g['accessors'][i]; v = g['bufferViews'][a['bufferView']]
    n = {'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}[a['type']]
    f = {5126:'f',5121:'B',5123:'H',5125:'I',5120:'b',5122:'h'}[a['componentType']]
    sz = struct.calcsize(f); stride = v.get('byteStride', sz*n)
    b = bins[v['buffer']]; base = v.get('byteOffset',0) + a.get('byteOffset',0)
    return [struct.unpack_from('<'+f*n, b, base + k*stride) for k in range(a['count'])]


def mmul(a, b):  # column-major 4x4
    return [sum(a[k*4+r]*b[c*4+k] for k in range(4)) for c in range(4) for r in range(4)]

def trs_to_m(n):
    if 'matrix' in n: return list(n['matrix'])
    t = n.get('translation', [0,0,0]); q = n.get('rotation', [0,0,0,1]); s = n.get('scale', [1,1,1])
    x, y, z, w = q
    r = [1-2*(y*y+z*z), 2*(x*y+z*w), 2*(x*z-y*w),
         2*(x*y-z*w), 1-2*(x*x+z*z), 2*(y*z+x*w),
         2*(x*z+y*w), 2*(y*z-x*w), 1-2*(x*x+y*y)]
    return [r[0]*s[0], r[1]*s[0], r[2]*s[0], 0, r[3]*s[1], r[4]*s[1], r[5]*s[1], 0,
            r[6]*s[2], r[7]*s[2], r[8]*s[2], 0, t[0], t[1], t[2], 1]

def inv_affine(m):
    a = [[m[c*4+r] for c in range(3)] for r in range(3)]
    det = (a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1]) - a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0]) + a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]))
    inv = [[(a[(j+1)%3][(i+1)%3]*a[(j+2)%3][(i+2)%3]-a[(j+1)%3][(i+2)%3]*a[(j+2)%3][(i+1)%3])/det for j in range(3)] for i in range(3)]
    t = m[12:15]
    ti = [-sum(inv[r][k]*t[k] for k in range(3)) for r in range(3)]
    return [inv[0][0], inv[1][0], inv[2][0], 0, inv[0][1], inv[1][1], inv[2][1], 0, inv[0][2], inv[1][2], inv[2][2], 0, ti[0], ti[1], ti[2], 1]

def m_to_trs(m):
    t = m[12:15]
    cols = [m[0:3], m[4:7], m[8:11]]
    s = [math.sqrt(sum(v*v for v in c)) for c in cols]
    det = (cols[0][0]*(cols[1][1]*cols[2][2]-cols[2][1]*cols[1][2])
           - cols[1][0]*(cols[0][1]*cols[2][2]-cols[2][1]*cols[0][2])
           + cols[2][0]*(cols[0][1]*cols[1][2]-cols[1][1]*cols[0][2]))
    if det < 0: s[0] = -s[0]
    c = [[v/s[i] for v in cols[i]] for i in range(3)]
    # R[row][col] = c[col][row]
    m00, m11, m22 = c[0][0], c[1][1], c[2][2]
    tr = m00 + m11 + m22
    if tr > 0:
        S = math.sqrt(tr+1)*2; w = S/4; x = (c[1][2]-c[2][1])/S; y = (c[2][0]-c[0][2])/S; z = (c[0][1]-c[1][0])/S
    elif m00 > m11 and m00 > m22:
        S = math.sqrt(1+m00-m11-m22)*2; w = (c[1][2]-c[2][1])/S; x = S/4; y = (c[1][0]+c[0][1])/S; z = (c[2][0]+c[0][2])/S
    elif m11 > m22:
        S = math.sqrt(1+m11-m00-m22)*2; w = (c[2][0]-c[0][2])/S; x = (c[1][0]+c[0][1])/S; y = S/4; z = (c[2][1]+c[1][2])/S
    else:
        S = math.sqrt(1+m22-m00-m11)*2; w = (c[0][1]-c[1][0])/S; x = (c[2][0]+c[0][2])/S; y = (c[2][1]+c[1][2])/S; z = S/4
    # shear check
    shear = max(abs(sum(c[i][k]*c[j][k] for k in range(3))) for i, j in ((0,1),(0,2),(1,2)))
    return list(t), [x, y, z, w], s, shear

def main():
    a = sys.argv[1:]
    src, dst = a[0], a[1]
    scale = float(a[a.index('--scale')+1]) if '--scale' in a else 1.0
    mirror = '--mirror' in a
    renames = [x.split('=', 1) for i, x in enumerate(a) if i > 0 and a[i-1] == '--rename']
    g, bins = load(src)
    nodes = g['nodes']
    local = [trs_to_m(n) for n in nodes]
    children = [list(n.get('children', [])) for n in nodes]
    parent = {c: i for i, ch in enumerate(children) for c in ch}
    roots = list(g['scenes'][g.get('scene', 0)]['nodes'])
    # collapse helpers
    for h in [i for i, n in enumerate(nodes) if '$AssimpFbx$' in (n.get('name') or '')]:
        p = parent.get(h)
        for c in children[h]:
            local[c] = mmul(local[h], local[c]); parent[c] = p
            if p is None: roots.append(c)
        if p is None: roots.remove(h)
        else:
            k = children[p].index(h); children[p][k:k+1] = children[h]
        children[h] = []; parent.pop(h, None); local[h] = None
    I4 = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]
    idnames = [x for i, x in enumerate(a) if i > 0 and a[i-1] == '--identity']
    for i, n in enumerate(nodes):
        if local[i] is not None and (n.get('name') in idnames or 'skin' in n):
            local[i] = list(I4)  # skinned mesh node transform is ignored by glTF anyway
    if '--rest-from-bind' in a:
        # make every joint's rest pose equal its bind pose (inverse IBM, mesh space)
        sk = g['skins'][0]
        bind = {j: inv_affine(list(b)) for j, b in zip(sk['joints'], accessor(g, bins, sk['inverseBindMatrices']))}
        def world(i):
            return mmul(world(parent[i]), local[i]) if i in parent else local[i]
        def depth(i):
            d = 0
            while i in parent: i = parent[i]; d += 1
            return d
        order = sorted(bind, key=depth)  # parents before children
        for j in order:
            pw = world(parent[j]) if j in parent else I4
            local[j] = mmul(inv_affine(pw), bind[j])
    keep = [i for i in range(len(nodes)) if local[i] is not None]
    remap = {o: n for n, o in enumerate(keep)}
    M = [-1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]
    def fixm(m):
        m = list(m); m[12] *= scale; m[13] *= scale; m[14] *= scale
        if mirror: m = mmul(M, mmul(m, M))
        return m
    out_nodes = []; worst_shear = 0
    for o in keep:
        n = nodes[o]; nn = {'name': n.get('name', f'node{o}')}
        for x, y in renames: nn['name'] = nn['name'].replace(x, y)
        t, r, s, sh = m_to_trs(fixm(local[o])); worst_shear = max(worst_shear, sh)
        if any(abs(v) > 1e-7 for v in t): nn['translation'] = t
        if abs(r[3]) < 1 - 1e-7: nn['rotation'] = r
        if any(abs(v-1) > 1e-5 for v in s): nn['scale'] = s
        if children[o]: nn['children'] = [remap[c] for c in children[o]]
        if 'mesh' in n: nn['mesh'] = n['mesh']
        if 'skin' in n: nn['skin'] = n['skin']
        out_nodes.append(nn)
    buf = bytearray(); views = []; accs = []
    def add(data, fmt, typ, ctype, target=None, minmax=False):
        while len(buf) % 4: buf.append(0)
        off = len(buf)
        for row in data: buf.extend(struct.pack('<' + fmt*len(row), *row))
        v = {'buffer': 0, 'byteOffset': off, 'byteLength': len(buf)-off}
        if target: v['target'] = target
        views.append(v)
        acc = {'bufferView': len(views)-1, 'componentType': ctype, 'count': len(data), 'type': typ}
        if minmax:
            acc['min'] = [min(r[i] for r in data) for i in range(len(data[0]))]
            acc['max'] = [max(r[i] for r in data) for i in range(len(data[0]))]
        accs.append(acc); return len(accs)-1
    meshes = []
    for m in g['meshes']:
        prims = []
        for p in m['primitives']:
            at = p['attributes']; na = {}
            P = [(x*scale*(-1 if mirror else 1), y*scale, z*scale) for x, y, z in accessor(g, bins, at['POSITION'])]
            na['POSITION'] = add(P, 'f', 'VEC3', 5126, 34962, True)
            if 'NORMAL' in at:
                N = [(-x if mirror else x, y, z) for x, y, z in accessor(g, bins, at['NORMAL'])]
                na['NORMAL'] = add(N, 'f', 'VEC3', 5126, 34962)
            if 'TEXCOORD_0' in at:
                na['TEXCOORD_0'] = add(accessor(g, bins, at['TEXCOORD_0']), 'f', 'VEC2', 5126, 34962)
            if 'JOINTS_0' in at:
                J = accessor(g, bins, at['JOINTS_0']); W = accessor(g, bins, at['WEIGHTS_0'])
                W = [tuple(w/sum(ww) for w in ww) if sum(ww) > 0 else (1,0,0,0) for ww in W]
                na['JOINTS_0'] = add(J, 'H', 'VEC4', 5123, 34962)
                na['WEIGHTS_0'] = add(W, 'f', 'VEC4', 5126, 34962)
            I = [i[0] for i in accessor(g, bins, p['indices'])]
            T = [tuple(I[k:k+3]) for k in range(0, len(I), 3)]
            if mirror: T = [(c, b, a_) for a_, b, c in T]
            ii = add([(x,) for t in T for x in t], 'I', 'SCALAR', 5125, 34963)
            prims.append({'attributes': na, 'indices': ii, 'material': 0})
        meshes.append({'name': m.get('name'), 'primitives': prims})
    skins = []
    for s in g.get('skins', []):
        ibm = [fixm(x) for x in accessor(g, bins, s['inverseBindMatrices'])]
        sk = {'joints': [remap[j] for j in s['joints']], 'inverseBindMatrices': add(ibm, 'f', 'MAT4', 5126)}
        if 'skeleton' in s and s['skeleton'] in remap: sk['skeleton'] = remap[s['skeleton']]
        skins.append(sk)
    while len(buf) % 4: buf.append(0)
    out = {'asset': {'version': '2.0', 'generator': 'frametop screens/tools/hand-glb-clean.py from ' + os.path.basename(src)},
           'scene': 0, 'scenes': [{'nodes': [remap[r] for r in roots]}], 'nodes': out_nodes,
           'meshes': meshes, 'skins': skins, 'accessors': accs, 'bufferViews': views,
           'buffers': [{'byteLength': len(buf)}],
           'materials': [{'name': 'hand', 'pbrMetallicRoughness': {'baseColorFactor': [0.8, 0.8, 0.8, 1], 'metallicFactor': 0, 'roughnessFactor': 0.6}}]}
    js = json.dumps(out, separators=(',', ':')).encode()
    js += b' ' * (-len(js) % 4)
    total = 12 + 8 + len(js) + 8 + len(buf)
    with open(dst, 'wb') as f:
        f.write(struct.pack('<4sII', b'glTF', 2, total))
        f.write(struct.pack('<II', len(js), 0x4E4F534A)); f.write(js)
        f.write(struct.pack('<II', len(buf), 0x004E4942)); f.write(buf)
    print(dst, 'nodes', len(out_nodes), 'worst shear', round(worst_shear, 6))

if __name__ == "__main__":
    main()
