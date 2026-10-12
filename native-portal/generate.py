#!/usr/bin/env python3
"""Add projective mask coordinates and clipping to the pinned native camera shaders.

The native 528-byte UBO and camera processing stay unchanged. The vertex stage
reads four vec4 push constants: plane center, two half-extent vectors, control.
Location 3 carries homogeneous plane coordinates, 4 the raw eye ray, and 5
the eye origin and capsule count. Flat location 6 carries control.w: values above
0.5 bypass our mask for full-room passthrough. The fragment mask unions the rectangle with
upstream hand capsules supplied through a separate 2304-byte uniform buffer,
including conservative group bounds and capsule ranges computed on the CPU.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

HERE = Path(__file__).resolve().parent
MANIFEST = json.loads((HERE / 'native-shaders.json').read_text())


def run(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True, stderr=subprocess.STDOUT)


def instructions(text):
    return [line.strip() for line in text.splitlines() if line.strip() and not line.lstrip().startswith(';')]


class Assembly:
    def __init__(self, text):
        self.lines = instructions(text)
        self.next_id = max(map(int, re.findall(r'%(\d+)', text))) + 1
        self.declarations = []

    def fresh(self):
        value = f'%{self.next_id}'
        self.next_id += 1
        return value

    def declaration(self, rhs):
        for line in self.lines + self.declarations:
            if line.endswith(' = ' + rhs):
                return line.split()[0]
        value = self.fresh()
        self.declarations.append(f'{value} = {rhs}')
        return value


def verify_preservation(before, after):
    """Every native instruction survives in order; only entry interfaces grow."""
    new = iter(instructions(after))
    for old in instructions(before):
        if not any(line == old or old.startswith('OpEntryPoint ') and line.startswith(old+' ')
                   for line in new):
            raise ValueError('native instruction changed: ' + old)


def patch(text, stage, raw_position=None):
    if stage not in ('vertex', 'fragment'):
        raise ValueError('unsupported stage')
    a = Assembly(text)
    lines = a.lines
    definitions = {line.split()[0]: line.split(' = ', 1)[1] for line in lines if ' = ' in line}
    entries = [line for line in lines if line.startswith('OpEntryPoint ')]
    if len(entries) != 1 or (' Vertex ' if stage == 'vertex' else ' Fragment ') not in entries[0]:
        raise ValueError('expected one matching entrypoint')
    if any('EarlyFragmentTests' in line or ' PushConstant' in line for line in lines):
        raise ValueError('unsupported native execution mode or push constants')
    function = entries[0].split()[2]
    storage = 'Output' if stage == 'vertex' else 'Input'
    for line in lines:
        if line.endswith(' Location 3') and definitions[line.split()[1]].endswith(' ' + storage):
            raise ValueError('mask coordinate location already occupied')
    f32 = a.declaration('OpTypeFloat 32')
    u32 = a.declaration('OpTypeInt 32 0')
    v3 = a.declaration(f'OpTypeVector {f32} 3')
    v4 = a.declaration(f'OpTypeVector {f32} 4')
    boolean = a.declaration('OpTypeBool')
    b3 = a.declaration(f'OpTypeVector {boolean} 3')
    zero = a.declaration(f'OpConstant {f32} 0')
    one = a.declaration(f'OpConstant {f32} 1')
    varying_ptr = a.declaration(f'OpTypePointer {storage} {v3}')
    varying = a.fresh()
    a.declarations.append(f'{varying} = OpVariable {varying_ptr} {storage}')
    lines[lines.index(entries[0])] += ' ' + varying
    ray_ptr = a.declaration(f'OpTypePointer {storage} {v3}')
    eye_ptr = a.declaration(f'OpTypePointer {storage} {v4}')
    flag_ptr = a.declaration(f'OpTypePointer {storage} {f32}')
    ray_var, eye_var, full_var = a.fresh(), a.fresh(), a.fresh()
    a.declarations += [f'{ray_var} = OpVariable {ray_ptr} {storage}',
                       f'{eye_var} = OpVariable {eye_ptr} {storage}',
                       f'{full_var} = OpVariable {flag_ptr} {storage}']
    for location in (4, 5, 6):
        if any(line.endswith(f' Location {location}') for line in lines):
            raise ValueError('hand varying location occupied')
    lines[lines.index(entries[0] + ' ' + varying)] += f' {ray_var} {eye_var} {full_var}'
    annotations = [f'OpDecorate {varying} Location 3', f'OpDecorate {ray_var} Location 4',
                   f'OpDecorate {eye_var} Location 5', f'OpDecorate {eye_var} Flat',
                   f'OpDecorate {full_var} Location 6', f'OpDecorate {full_var} Flat']
    code = []

    def emit(rhs):
        result = a.fresh()
        code.append(f'{result} = {rhs}')
        return result

    if stage == 'vertex':
        bindings = [line.split()[1] for line in lines
                    if re.fullmatch(r'OpDecorate %\d+ Binding 0', line)]
        if len(bindings) != 1:
            raise ValueError('expected unique native camera UBO')
        ubo = bindings[0]
        ptr = definitions[ubo].split()[1]
        if f'OpDecorate {ubo} DescriptorSet 0' not in lines or not definitions[ptr].startswith('OpTypePointer Uniform '):
            raise ValueError('unexpected camera descriptor')
        block = definitions[ptr].split()[2]
        members = definitions[block].split()[1:]
        if len(members) != 26 or f'OpMemberDecorate {block} 25 Offset 464' not in lines:
            raise ValueError('expected unchanged native 528-byte UBO')
        matrix = members[1]
        if definitions[matrix] != f'OpTypeMatrix {v4} 4':
            raise ValueError('M1 is not float4x4')
        for decoration in (f'OpMemberDecorate {block} 1 Offset 64',
                           f'OpMemberDecorate {block} 1 RowMajor',
                           f'OpMemberDecorate {block} 1 MatrixStride 16'):
            if decoration not in lines:
                raise ValueError('unexpected native matrix layout: ' + decoration)
        raw = f'%{raw_position}'
        if raw not in definitions or definitions[raw].split()[1] != v4:
            raise ValueError('native reference position must be vec4')
        glsl = next(line.split()[0] for line in lines if '= OpExtInstImport "GLSL.std.450"' in line)
        matrix_ptr = a.declaration(f'OpTypePointer Uniform {matrix}')
        index = a.declaration(f'OpConstant {u32} 1')
        address = emit(f'OpAccessChain {matrix_ptr} {ubo} {index}')
        m1 = emit(f'OpLoad {matrix} {address}')
        inverse = emit(f'OpExtInst {matrix} {glsl} MatrixInverse {m1}')
        origin = a.declaration(f'OpConstantComposite {v4} {zero} {zero} {zero} {one}')
        e4 = emit(f'OpMatrixTimesVector {v4} {inverse} {origin}')
        e3 = emit(f'OpVectorShuffle {v3} {e4} {e4} 0 1 2')
        ew = emit(f'OpCompositeExtract {f32} {e4} 3')
        reciprocal_w = emit(f'OpFDiv {f32} {one} {ew}')
        eye = emit(f'OpVectorTimesScalar {v3} {e3} {reciprocal_w}')

        push_block = a.declaration(f'OpTypeStruct {v4} {v4} {v4} {v4}')
        push_ptr = a.declaration(f'OpTypePointer PushConstant {push_block}')
        push = a.declaration(f'OpVariable {push_ptr} PushConstant')
        vector_ptr = a.declaration(f'OpTypePointer PushConstant {v4}')
        annotations += [f'OpDecorate {push_block} Block']
        annotations += [f'OpMemberDecorate {push_block} {i} Offset {16*i}' for i in range(4)]
        vectors = []
        for i in range(4):
            member = a.declaration(f'OpConstant {u32} {i}')
            address = emit(f'OpAccessChain {vector_ptr} {push} {member}')
            vectors.append(emit(f'OpLoad {v4} {address}'))
        center, u, v = [emit(f'OpVectorShuffle {v3} {value} {value} 0 1 2') for value in vectors[:3]]
        active = emit(f'OpCompositeExtract {f32} {vectors[3]} 0')
        full = emit(f'OpCompositeExtract {f32} {vectors[3]} 3')
        code.append(f'OpStore {full_var} {full}')
        w = emit(f'OpFSub {v3} {center} {eye}')
        normal = emit(f'OpExtInst {v3} {glsl} Cross {u} {v}')
        determinant = emit(f'OpDot {f32} {normal} {w}')
        reciprocal = emit(f'OpFDiv {f32} {one} {determinant}')
        numerators = [emit(f'OpExtInst {v3} {glsl} Cross {v} {w}'),
                      emit(f'OpExtInst {v3} {glsl} Cross {w} {u}'), normal]
        coefficients = []
        for numerator in numerators:
            row = emit(f'OpVectorTimesScalar {v3} {numerator} {reciprocal}')
            dot = emit(f'OpDot {f32} {row} {eye}')
            offset = emit(f'OpFNegate {f32} {dot}')
            xyz = [emit(f'OpCompositeExtract {f32} {row} {i}') for i in range(3)]
            coefficient = emit(f'OpCompositeConstruct {v4} {" ".join(xyz)} {offset}')
            coefficients.append(emit(f'OpVectorTimesScalar {v4} {coefficient} {active}'))
        xyz = [emit(f'OpCompositeExtract {f32} {raw} {i}') for i in range(3)]
        raw3 = emit(f'OpCompositeConstruct {v3} {" ".join(xyz)}')
        ray = emit(f'OpFSub {v3} {raw3} {eye}')
        count = emit(f'OpCompositeExtract {f32} {vectors[3]} 2')
        eye_xyz = [emit(f'OpCompositeExtract {f32} {eye} {i}') for i in range(3)]
        eye_count = emit(f'OpCompositeConstruct {v4} {" ".join(eye_xyz)} {count}')
        code += [f'OpStore {ray_var} {ray}', f'OpStore {eye_var} {eye_count}']
        homogeneous = emit(f'OpCompositeConstruct {v4} {" ".join(xyz)} {one}')
        dots = [emit(f'OpDot {f32} {coefficient} {homogeneous}') for coefficient in coefficients]
        q = emit(f'OpCompositeConstruct {v3} {" ".join(dots)}')
        code.append(f'OpStore {varying} {q}')
        position = next(i for i, line in enumerate(lines) if line.startswith(raw+' = '))
        lines[position+1:position+1] = code
    else:
        # Reject outside the rectangle in homogeneous plane coordinates. Future
        # mask predicates can use q.xy/q.z here without touching camera sampling.
        entry = next(i for i, line in enumerate(lines) if line.startswith(function+' = OpFunction '))
        first_label = lines[entry+1]
        if not first_label.endswith(' = OpLabel') or 'OpVariable' in lines[entry+2]:
            raise ValueError('unexpected native fragment entry structure')
        label = first_label.split()[0]
        entry_label = a.fresh()
        code.append(f'{entry_label} = OpLabel')
        q = emit(f'OpLoad {v3} {varying}')
        nan = emit(f'OpIsNan {b3} {q}')
        inf = emit(f'OpIsInf {b3} {q}')
        invalid = emit(f'OpLogicalOr {b3} {nan} {inf}')
        bad = emit(f'OpAny {boolean} {invalid}')
        qx, qy, qz = [emit(f'OpCompositeExtract {f32} {q} {i}') for i in range(3)]
        behind = emit(f'OpFOrdLessThanEqual {boolean} {qz} {zero}')
        bad = emit(f'OpLogicalOr {boolean} {bad} {behind}')
        for component in (qx, qy):
            negative = emit(f'OpFNegate {f32} {component}')
            positive_out = emit(f'OpFOrdGreaterThan {boolean} {component} {qz}')
            negative_out = emit(f'OpFOrdGreaterThan {boolean} {negative} {qz}')
            outside = emit(f'OpLogicalOr {boolean} {positive_out} {negative_out}')
            bad = emit(f'OpLogicalOr {boolean} {bad} {outside}')
        # Union of the desk and predicted world-space tapered capsules. Test the
        # eye ray against the swept spheres A+t(B-A), radius mix(ra,rb,t).
        # Minimizing squared ray distance minus radius squared is a quadratic;
        # endpoints plus its stationary point give exact coverage for this shape.
        glsl = next(line.split()[0] for line in lines if '= OpExtInstImport "GLSL.std.450"' in line)
        n144 = a.declaration(f'OpConstant {u32} 144')
        array = a.declaration(f'OpTypeArray {v4} {n144}')
        hand_block = a.declaration(f'OpTypeStruct {array}')
        hand_ptr = a.declaration(f'OpTypePointer Uniform {hand_block}')
        hand = a.declaration(f'OpVariable {hand_ptr} Uniform')
        member_ptr = a.declaration(f'OpTypePointer Uniform {v4}')
        annotations += [f'OpDecorate {array} ArrayStride 16', f'OpDecorate {hand_block} Block',
                        f'OpMemberDecorate {hand_block} 0 Offset 0',
                        f'OpDecorate {hand} DescriptorSet 1', f'OpDecorate {hand} Binding 0']
        izero = a.declaration(f'OpConstant {u32} 0')
        ione = a.declaration(f'OpConstant {u32} 1')
        itwo = a.declaration(f'OpConstant {u32} 2')
        near = a.declaration(f'OpConstant {f32} 0.12')
        eps = a.declaration(f'OpConstant {f32} 1e-8')
        eye4 = emit(f'OpLoad {v4} {eye_var}')
        eye3 = emit(f'OpVectorShuffle {v3} {eye4} {eye4} 0 1 2')
        countf = emit(f'OpCompositeExtract {f32} {eye4} 3')
        count = emit(f'OpConvertFToU {u32} {countf}')
        ray = emit(f'OpLoad {v3} {ray_var}')
        direction = emit(f'OpExtInst {v3} {glsl} Normalize {ray}')
        good = emit(f'OpLogicalNot {boolean} {bad}')
        half = a.declaration(f'OpConstant {f32} 0.5')
        full = emit(f'OpLoad {f32} {full_var}')
        bypass = emit(f'OpFOrdGreaterThan {boolean} {full} {half}')
        previous_hit = emit(f'OpLogicalOr {boolean} {good} {bypass}')
        original_hit, original_entry = previous_hit, entry_label
        has_capsules = emit(f'OpUGreaterThan {boolean} {count} {izero}')
        needs_hit = emit(f'OpLogicalNot {boolean} {previous_hit}')
        needs_capsules = emit(f'OpLogicalAnd {boolean} {has_capsules} {needs_hit}')
        hand_entry, hand_done = a.fresh(), a.fresh()
        code += [f'OpSelectionMerge {hand_done} None',
                 f'OpBranchConditional {needs_capsules} {hand_entry} {hand_done}',
                 f'{hand_entry} = OpLabel']
        entry_label = hand_entry
        # Static group indices avoid costly per-fragment group indexing. Bounds
        # and ranges are computed from the same immutable CPU snapshot.
        for group in range(8):
            bound_index = a.declaration(f'OpConstant {u32} {128+group}')
            address = emit(f'OpAccessChain {member_ptr} {hand} {izero} {bound_index}')
            sphere = emit(f'OpLoad {v4} {address}')
            center = emit(f'OpVectorShuffle {v3} {sphere} {sphere} 0 1 2')
            radius = emit(f'OpCompositeExtract {f32} {sphere} 3')
            w = emit(f'OpFSub {v3} {center} {eye3}')
            t = emit(f'OpDot {f32} {w} {direction}')
            far = emit(f'OpFAdd {f32} {t} {radius}')
            forward = emit(f'OpFOrdGreaterThanEqual {boolean} {far} {near}')
            length2 = emit(f'OpDot {f32} {w} {w}')
            t2 = emit(f'OpFMul {f32} {t} {t}')
            distance2 = emit(f'OpFSub {f32} {length2} {t2}')
            radius2 = emit(f'OpFMul {f32} {radius} {radius}')
            inside = emit(f'OpFOrdLessThanEqual {boolean} {distance2} {radius2}')
            eligible = emit(f'OpLogicalAnd {boolean} {forward} {inside}')
            range_index = a.declaration(f'OpConstant {u32} {136+group}')
            address = emit(f'OpAccessChain {member_ptr} {hand} {izero} {range_index}')
            span = emit(f'OpLoad {v4} {address}')
            beginf = emit(f'OpCompositeExtract {f32} {span} 0')
            endf = emit(f'OpCompositeExtract {f32} {span} 1')
            first = emit(f'OpConvertFToU {u32} {beginf}')
            last = emit(f'OpConvertFToU {u32} {endf}')
            header, body, continuing, done = [a.fresh() for _ in range(4)]
            index_id, next_index, hit, next_hit = [a.fresh() for _ in range(4)]
            code += [f'OpBranch {header}', f'{header} = OpLabel',
                     f'{index_id} = OpPhi {u32} {first} {entry_label} {next_index} {continuing}',
                     f'{hit} = OpPhi {boolean} {previous_hit} {entry_label} {next_hit} {continuing}']
            remaining = emit(f'OpULessThan {boolean} {index_id} {last}')
            missed = emit(f'OpLogicalNot {boolean} {hit}')
            candidate = emit(f'OpLogicalAnd {boolean} {remaining} {eligible}')
            again = emit(f'OpLogicalAnd {boolean} {candidate} {missed}')
            code += [f'OpLoopMerge {done} {continuing} None', f'OpBranchConditional {again} {body} {done}',
                     f'{body} = OpLabel']
            ai = emit(f'OpIMul {u32} {index_id} {itwo}')
            bi = emit(f'OpIAdd {u32} {ai} {ione}')
            ends = []
            for idx in (ai, bi):
                address = emit(f'OpAccessChain {member_ptr} {hand} {izero} {idx}')
                ends.append(emit(f'OpLoad {v4} {address}'))
            av, bv = [emit(f'OpVectorShuffle {v3} {x} {x} 0 1 2') for x in ends]
            ra, rb = [emit(f'OpCompositeExtract {f32} {x} 3') for x in ends]
            def op(name, typ, *values):
                return emit(f'Op{name} {typ} ' + ' '.join(values))
            def scalar(name, *values):
                return op(name, f32, *values)
            def ext(name, *values):
                return emit(f'OpExtInst {f32} {glsl} {name} ' + ' '.join(values))
            w = op('FSub', v3, av, eye3)
            bone = op('FSub', v3, bv, av)
            depth = scalar('Dot', w, direction)
            slope = scalar('Dot', bone, direction)
            p = op('FSub', v3, w, op('VectorTimesScalar', v3, direction, depth))
            b = op('FSub', v3, bone, op('VectorTimesScalar', v3, direction, slope))
            dr = scalar('FSub', rb, ra)
            qa = scalar('FSub', scalar('Dot', b, b), scalar('FMul', dr, dr))
            qb = scalar('FSub', scalar('Dot', p, b), scalar('FMul', ra, dr))
            qc = scalar('FSub', scalar('Dot', p, p), scalar('FMul', ra, ra))
            positive = op('FOrdGreaterThan', boolean, slope, eps)
            negative = op('FOrdLessThan', boolean, slope, scalar('FNegate', eps))
            moving = op('LogicalOr', boolean, positive, negative)
            safe_slope = op('Select', f32, moving, slope, one)
            crossing = scalar('FDiv', scalar('FSub', near, depth), safe_slope)
            lo = op('Select', f32, positive, ext('FMax', zero, crossing), zero)
            hi = op('Select', f32, negative, ext('FMin', one, crossing), one)
            interval = op('FOrdLessThanEqual', boolean, lo, hi)
            forward = op('LogicalOr', boolean, moving, op('FOrdGreaterThanEqual', boolean, depth, near))
            valid = op('LogicalAnd', boolean, interval, forward)
            convex = op('FOrdGreaterThan', boolean, qa, eps)
            safe_a = op('Select', f32, convex, qa, one)
            stationary = ext('FClamp', scalar('FDiv', scalar('FNegate', qb), safe_a), lo, hi)
            def quadratic(t):
                twice_b = scalar('FAdd', qb, qb)
                return scalar('FAdd', scalar('FMul', scalar('FAdd', scalar('FMul', qa, t), twice_b), t), qc)
            minimum = ext('FMin', quadratic(lo), quadratic(hi))
            minimum = op('Select', f32, convex, ext('FMin', minimum, quadratic(stationary)), minimum)
            inside = op('FOrdLessThanEqual', boolean, minimum, zero)
            code += [f'{next_hit} = OpLogicalAnd {boolean} {valid} {inside}',
                     f'OpBranch {continuing}', f'{continuing} = OpLabel',
                     f'{next_index} = OpIAdd {u32} {index_id} {ione}', f'OpBranch {header}', f'{done} = OpLabel']
            previous_hit = hit
            entry_label = done
        final_hit = a.fresh()
        code += [f'OpBranch {hand_done}', f'{hand_done} = OpLabel',
                 f'{final_hit} = OpPhi {boolean} {original_hit} {original_entry} {previous_hit} {entry_label}']
        bad = emit(f'OpLogicalNot {boolean} {final_hit}')
        kill = a.fresh()
        code += [f'OpSelectionMerge {label} None', f'OpBranchConditional {bad} {kill} {label}',
                 f'{kill} = OpLabel', 'OpKill']
        lines[entry+1:entry+1] = code

    first_type = next(i for i, line in enumerate(lines) if ' = OpType' in line)
    lines[first_type:first_type] = annotations
    first_function = next(i for i, line in enumerate(lines) if ' = OpFunction ' in line)
    lines[first_function:first_function] = a.declarations
    result = '\n'.join(lines)+'\n'
    verify_preservation(text, result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path('/opt/steamvr/resources/shaders/vulkan'))
    parser.add_argument('--output', type=Path, default=HERE / 'build/current')
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if source == output or source in output.parents:
        parser.error('output must be separate from native shader inputs')
    inputs = {}
    for ident, info in MANIFEST['shaders'].items():
        data = (source / info['filename']).read_bytes()
        if hashlib.sha256(data).hexdigest() != info['sha256']:
            raise ValueError('native shader hash mismatch: '+info['filename'])
        inputs[ident] = data
    output.mkdir(parents=True, exist_ok=True)
    manifest_path = output/'shaders.json'
    manifest_path.unlink(missing_ok=True)
    report = {'geometry_mode': 'frametop-push-constants', 'push_constant_bytes': 64,
              'ubo_bytes': 528, 'mask_coordinate_location': 3, 'full_room_location': 6,
              'hand_ubo_bytes': 2304, 'hand_descriptor_set': 1, 'shaders': {}}
    for ident, info in MANIFEST['shaders'].items():
        native = output/(ident+'.original.spv')
        native.write_bytes(inputs[ident])
        text = run('spirv-dis', '--raw-id', native)
        modified = patch(text, info['stage'], info['raw_position_id'])
        assembly = output/(ident+'.mask.spvasm')
        target = output/(ident+'.portal.spv')
        assembly.write_text(modified)
        run('spirv-as', '--preserve-numeric-ids', '--target-env', 'vulkan1.0', assembly, '-o', target)
        run('spirv-val', '--target-env', 'vulkan1.0', target)
        verify_preservation(text, run('spirv-dis', '--raw-id', target))
        report['shaders'][ident] = {'file': target.name, 'sha256': hashlib.sha256(target.read_bytes()).hexdigest()}
    manifest_path.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
