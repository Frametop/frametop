"""OpenVR right-handed poses. Metres; +X right, +Y up, forward -Z.

Quaternions are [w, x, y, z]. Euler input is degrees [yaw, pitch, roll],
composed Y * X * Z. Relative offsets use world axes, not head-local axes.
"""
import math


def vector(values, length):
    if len(values) != length or any(isinstance(v, bool) or not isinstance(v, (int, float))
                                    or not math.isfinite(v) for v in values):
        raise ValueError(f'expected {length} finite numbers')
    return [float(v) for v in values]


def normalize(q):
    q = vector(q, 4)
    norm = math.hypot(*q)
    if norm < 1e-12:
        raise ValueError('zero quaternion')
    return [v / norm for v in q]


def multiply(a, b):
    w, x, y, z = a
    v, i, j, k = b
    return [w*v-x*i-y*j-z*k, w*i+x*v+y*k-z*j,
            w*j-x*k+y*v+z*i, w*k+x*j-y*i+z*v]


def from_euler(position, angles):
    position = vector(position, 3)
    yaw, pitch, roll = [math.radians(v) / 2 for v in vector(angles, 3)]
    qy = [math.cos(yaw), 0, math.sin(yaw), 0]
    qx = [math.cos(pitch), math.sin(pitch), 0, 0]
    qz = [math.cos(roll), 0, 0, math.sin(roll)]
    return {'position': position, 'quaternion': normalize(multiply(multiply(qy, qx), qz))}


def rotate(q, v):
    q = normalize(q)
    v = vector(v, 3)
    return multiply(multiply(q, [0] + v), [q[0], -q[1], -q[2], -q[3]])[1:]


def offset(pose, translation, angles):
    delta = from_euler(translation, angles)
    return {'position': [a+b for a, b in zip(vector(pose['position'], 3), delta['position'])],
            'quaternion': normalize(multiply(delta['quaternion'], normalize(pose['quaternion'])))}


def compose(parent, child):
    rotated = rotate(parent['quaternion'], child['position'])
    return {'position': [a+b for a, b in zip(vector(parent['position'], 3), rotated)],
            'quaternion': normalize(multiply(normalize(parent['quaternion']), normalize(child['quaternion'])))}


def inverse(pose):
    w, x, y, z = normalize(pose['quaternion'])
    q = [w, -x, -y, -z]
    return {'position': rotate(q, [-v for v in vector(pose['position'], 3)]), 'quaternion': q}


def to_matrix(pose):
    w, x, y, z = normalize(pose['quaternion'])
    px, py, pz = vector(pose['position'], 3)
    return [1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w), px,
            2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w), py,
            2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y), pz]


def from_matrix(values):
    a = vector(values, 12)
    r = [a[0:3], a[4:7], a[8:11]]
    for i in range(3):
        for j in range(3):
            if abs(sum(r[i][k]*r[j][k] for k in range(3)) - int(i == j)) > 1e-3:
                raise ValueError('pose rotation is not orthonormal')
    det = (r[0][0]*(r[1][1]*r[2][2]-r[1][2]*r[2][1])
           - r[0][1]*(r[1][0]*r[2][2]-r[1][2]*r[2][0])
           + r[0][2]*(r[1][0]*r[2][1]-r[1][1]*r[2][0]))
    if abs(det - 1) > 1e-3:
        raise ValueError('pose rotation is reflected')
    trace = r[0][0] + r[1][1] + r[2][2]
    if trace > 0:
        s = math.sqrt(trace+1)*2
        q = [s/4, (r[2][1]-r[1][2])/s, (r[0][2]-r[2][0])/s, (r[1][0]-r[0][1])/s]
    else:
        i = max(range(3), key=lambda k: r[k][k])
        j, k = (i+1) % 3, (i+2) % 3
        s = math.sqrt(1+r[i][i]-r[j][j]-r[k][k])*2
        q = [(r[k][j]-r[j][k])/s, 0, 0, 0]
        q[i+1], q[j+1], q[k+1] = s/4, (r[j][i]+r[i][j])/s, (r[k][i]+r[i][k])/s
    return {'position': [a[3], a[7], a[11]], 'quaternion': normalize(q)}
