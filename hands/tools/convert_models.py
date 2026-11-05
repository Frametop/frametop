"""Convert the OpenCV Zoo ONNX ports of MediaPipe's hand models to ncnn.

The ONNX files are Apache-2.0 ports of MediaPipe's palm detector and hand
landmark models (huggingface.co/opencv/palm_detection_mediapipe and
huggingface.co/opencv/handpose_estimation_mediapipe). pnnx does the
conversion; two fix-ups follow:

- The palm detector widens channels with ONNX Pad on the channel axis. pnnx
  emits an ncnn layer called "Pad", which ncnn doesn't have, so rewrite those
  as ncnn Padding with the channel-end amount (param 8 = behind).
- Both models take NHWC input and start with a Permute to NCHW. Drop it, so we
  can hand ncnn planar CHW Mats straight from the preprocessing step.

usage: python convert_models.py   (writes models/ncnn/{palm,hand}.ncnn.{param,bin})
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
PNNX = os.path.join(sys.prefix, 'lib', 'python%d.%d' % sys.version_info[:2], 'site-packages', 'pnnx', 'pnnx')
MODELS = [('palm', 'palm_detection_mediapipe_2023feb', 192),
          ('hand', 'handpose_estimation_mediapipe_2023feb', 224)]


def patch(param_text, pnnx_param_text):
    lines = param_text.splitlines()
    assert lines[0] == '7767517'
    nlayers, nblobs = map(int, lines[1].split())
    body = lines[2:]

    # Channel pads: amounts come from the pnnx graph, which keeps the pads tuple.
    pads = dict(re.findall(r'^Pad\s+(\S+)\s.*pads=\(0,0,0,0,0,(\d+),0,0\)', pnnx_param_text, re.M))
    for i, line in enumerate(body):
        f = line.split()
        if f[0] == 'Pad':
            amount = pads[f[1]]
            body[i] = 'Padding %s %s %s %s %s 0=0 1=0 2=0 3=0 4=0 5=0.000000e+00 7=0 8=%s' % (
                f[1], f[2], f[3], f[4], f[5], amount)

    # Input permute: feed its consumers from in0 instead.
    perm = next(i for i, line in enumerate(body) if line.split()[0] == 'Permute')
    f = body[perm].split()
    assert f[4] == 'in0' and f[6] == '0=4', body[perm]
    blob = f[5]
    del body[perm]
    for i, line in enumerate(body):
        f = line.split()
        if f[0] == 'Input':
            continue
        nin = int(f[2])
        ins = ['in0' if b == blob else b for b in f[4:4 + nin]]
        body[i] = ' '.join(f[:4] + ins + f[4 + nin:])
    return '\n'.join(['7767517', '%d %d' % (nlayers - 1, nblobs - 1)] + body) + '\n'


def main():
    out = os.path.join(ROOT, 'models', 'ncnn')
    os.makedirs(out, exist_ok=True)
    for short, name, size in MODELS:
        src = os.path.join(ROOT, 'models', 'onnx', name + '.onnx')
        with tempfile.TemporaryDirectory() as tmp:
            shutil.copy(src, tmp)
            subprocess.run([PNNX, name + '.onnx', 'inputshape=[1,%d,%d,3]' % (size, size), 'fp16=1'],
                           cwd=tmp, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            with open(os.path.join(tmp, name + '.ncnn.param')) as f:
                param = f.read()
            with open(os.path.join(tmp, name + '.pnnx.param')) as f:
                pparam = f.read()
            with open(os.path.join(out, short + '.ncnn.param'), 'w') as f:
                f.write(patch(param, pparam))
            shutil.copy(os.path.join(tmp, name + '.ncnn.bin'), os.path.join(out, short + '.ncnn.bin'))
        print('wrote', short)


if __name__ == '__main__':
    main()
