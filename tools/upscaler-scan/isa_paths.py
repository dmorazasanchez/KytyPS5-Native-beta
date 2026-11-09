#!/usr/bin/env python3
"""Descriptor access paths of a PS5 (GFX10) shader binary, from its own instructions.

Every image/buffer instruction names its resource by scalar registers. Following those registers back through
s_load_dword* (and simple copies / pointer additions) to the shader's user-data registers gives an exact path:

    ["ud", 0, 8, 256]  =  T# at  *(*(ud[0:1] + 8) + 256)      (64-bit pointers, 48-bit addresses)
    ["ud", 4]          =  T# in user data s[4:11]

together with what the instruction does with it: load / sample / store / atomic / resinfo, its channel mask, and
how many instructions use it. Nothing is guessed from formats or names; an access whose register cannot be
followed is reported as "unresolved". Disassembly: llvm-mc (amdgcn, gfx1013).
"""
import json
import re
import subprocess
import sys
from collections import Counter, defaultdict

LLVM_MC = 'llvm-mc'
_REG = re.compile(r'^s\[(\d+):(\d+)\]$|^s(\d+)$')


def disassemble(code: bytes):
    hexs = ' '.join('0x%02x' % b for b in code)
    r = subprocess.run([LLVM_MC, '--disassemble', '-triple=amdgcn', '-mcpu=gfx1013'], input=hexs, capture_output=True, text=True)
    out = []
    for line in r.stdout.splitlines():
        line = line.split('//')[0].split(';')[0].strip()
        if line and not line.startswith('.'):
            out.append(line)
    return out


def regs(op):
    """SGPR numbers of an operand (s5, s[4:11]); None for anything else."""
    op = op.strip().split()[0] if op.strip() else ''   # operand without its modifiers (dmask:, dim:, offset:)
    m = _REG.match(op)
    if not m:
        return None
    if m.group(3) is not None:
        return [int(m.group(3))]
    return list(range(int(m.group(1)), int(m.group(2)) + 1))


def split_ops(text):
    parts = text.split(None, 1)
    mnem = parts[0]
    rest = parts[1] if len(parts) > 1 else ''
    ops, depth, cur = [], 0, ''
    for ch in rest:
        if ch == '[':
            depth += 1
        elif ch == ']':
            depth -= 1
        if ch == ',' and depth == 0:
            ops.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        ops.append(cur.strip())
    return mnem, ops


def imm(op):
    op = op.strip()
    if op in ('null', '0'):
        return 0
    try:
        return int(op, 0)
    except ValueError:
        return None


class Analyzer:
    def __init__(self, user_sgprs=16):
        # value of each SGPR: ('ud', i) | ('ld', base_path, off) | ('add', ...) | None (unknown)
        self.s = {i: ('ud', i) for i in range(user_sgprs)}
        self.access = defaultdict(lambda: {'ops': Counter(), 'dmask': set(), 'dim': set()})
        self.unresolved = Counter()

    def pointer(self, lo):
        """Path of the 64-bit pointer in s[lo:lo+1], or None."""
        a, b = self.s.get(lo), self.s.get(lo + 1)
        if a is None or b is None:
            return None
        if a[0] == 'ud' and b == ('ud', a[1] + 1):
            return ['ud', a[1]]
        if a[0] == 'ld' and b[0] == 'ld' and a[1] == b[1] and b[2] == a[2] + 4:
            return a[1] + [a[2]]
        if a[0] == 'ptr' and b[0] == 'ptrhi' and a[1] == b[1]:
            return a[1]
        return None

    def resource(self, first, count):
        """Path of a descriptor held in s[first : first+count] (T# 8, V#/S# 4)."""
        vals = [self.s.get(first + k) for k in range(count)]
        if any(v is None for v in vals):
            return None
        if vals[0][0] == 'ud' and all(v == ('ud', vals[0][1] + k) for k, v in enumerate(vals)):
            return ['udinline', vals[0][1]]
        if vals[0][0] == 'ld' and all(v[0] == 'ld' and v[1] == vals[0][1] and v[2] == vals[0][2] + 4 * k for k, v in enumerate(vals)):
            return vals[0][1] + [vals[0][2]]
        return None

    def write(self, regs_, value_fn):
        for k, r in enumerate(regs_):
            self.s[r] = value_fn(k)

    def step(self, line):
        mnem, ops = split_ops(line)
        if mnem.startswith('s_load_dword') or mnem.startswith('s_buffer_load_dword'):
            dst, base = regs(ops[0]), regs(ops[1]) if len(ops) > 1 else None
            off = imm(ops[2]) if len(ops) > 2 else 0
            if mnem.startswith('s_buffer'):  # constants out of a V#: values, not descriptors
                path = self.resource(base[0], 4) if base else None
                if path is not None:
                    a = self.access[('buffer', tuple(path))]
                    a['ops']['s_buffer_load'] += 1
                    if off is not None:
                        a['dmask'].add(off)  # (for constant buffers: the byte offsets read)
                if dst is not None:
                    self.write(dst, lambda k: None)
                return
            if dst is None:
                return
            p = self.pointer(base[0]) if base and len(base) == 2 else None
            if p is None or off is None:
                self.write(dst, lambda k: None)
                return
            self.write(dst, lambda k: ('ld', p, off + 4 * k))
            return
        if mnem in ('s_mov_b32', 's_mov_b64'):
            dst, src = regs(ops[0]), regs(ops[1])
            if dst is None:
                return
            if src is not None:
                vals = [self.s.get(r) for r in src]
                self.write(dst, lambda k: vals[k] if k < len(vals) else None)
            else:
                self.write(dst, lambda k: None)
            return
        if mnem == 's_and_b32' and len(ops) == 3 and regs(ops[0]) == regs(ops[1]) and imm(ops[2]) in (0xffff, 0xffffff):
            return  # masking a pointer's high word: same pointer
        if mnem in ('s_add_u32',) and len(ops) == 3:
            dst, src, k = regs(ops[0]), regs(ops[1]), imm(ops[2])
            if dst and src and k is not None:
                p = self.pointer(src[0])
                if p is not None:
                    self.s[dst[0]] = ('ptr', p[:-1] + [p[-1] + k] if p[0] != 'ud' or len(p) > 2 else p + [k], None)
                    self._pending_add = (dst[0], self.s[dst[0]][1])
                    return
            if dst:
                self.write(dst, lambda _k: None)
            return
        if mnem == 's_addc_u32' and len(ops) == 3:
            dst = regs(ops[0])
            pend = getattr(self, '_pending_add', None)
            if dst and pend and dst[0] == pend[0] + 1:
                self.s[dst[0]] = ('ptrhi', pend[1])
                self.s[pend[0]] = ('ptr', pend[1])
                self._pending_add = None
                return
            if dst:
                self.write(dst, lambda _k: None)
            return
        if mnem.startswith('image_'):
            # image_<op> vdata, vaddr, srsrc[, ssamp] ...
            rs = [regs(o) for o in ops]
            srsrc = next((r for r in rs[1:] if r is not None and len(r) == 8), None)
            dmask = re.search(r'dmask:(0x[0-9a-f]+|\d+)', line)
            dim = re.search(r'dim:(\S+)', line)
            path = self.resource(srsrc[0], 8) if srsrc else None
            kind = 'store' if mnem.startswith('image_store') else 'atomic' if mnem.startswith('image_atomic') else \
                'sample' if mnem.startswith('image_sample') or mnem.startswith('image_gather') else \
                'resinfo' if mnem.startswith('image_get') else 'load'
            if path is None:
                self.unresolved[kind] += 1
                return
            a = self.access[('image', tuple(path))]
            a['ops'][kind] += 1
            if dmask:
                a['dmask'].add(int(dmask.group(1), 0))
            if dim:
                a['dim'].add(dim.group(1))
            return
        if mnem.startswith(('buffer_', 'tbuffer_')):
            rs = [regs(o) for o in ops]
            srsrc = next((r for r in rs[1:] if r is not None and len(r) == 4), None)
            path = self.resource(srsrc[0], 4) if srsrc else None
            kind = 'store' if 'store' in mnem else 'atomic' if 'atomic' in mnem else 'load'
            if path is None:
                self.unresolved['buffer_' + kind] += 1
                return
            self.access[('buffer', tuple(path))]['ops'][kind] += 1
            return
        # Any other scalar write: unknown value.
        if ops and mnem.startswith('s_') and not mnem.startswith(('s_cmp', 's_bitcmp', 's_cbranch', 's_branch', 's_waitcnt',
                                                                    's_nop', 's_endpgm', 's_barrier', 's_setprio', 's_sendmsg',
                                                                    's_dcache', 's_icache', 's_inst_prefetch', 's_clause',
                                                                    's_delay', 's_sleep', 's_setreg', 's_denorm', 's_round',
                                                                    's_code_end', 's_ttrace', 's_trap')):
            dst = regs(ops[0])
            if dst:
                self.write(dst, lambda _k: None)


def analyse_code(code: bytes, user_sgprs=16):
    lines = disassemble(code)
    a = Analyzer(user_sgprs)
    for line in lines:
        a.step(line)
    accesses = []
    for (cls, path), v in sorted(a.access.items(), key=lambda kv: (kv[0][0], kv[0][1])):
        accesses.append({'class': cls, 'path': list(path), 'ops': dict(v['ops']), 'dmask': sorted(v['dmask']),
                         'dim': sorted(v['dim'])})
    return {'instructions': len(lines), 'accesses': accesses, 'unresolved': dict(a.unresolved)}


def capture_lines(name, result):
    """KYTY_PASS_CAPTURE_PATHS lines: <hash> image|buffer <ud> <off1> ... <offN>."""
    lines = []
    for a in result['accesses']:
        p = a['path']
        if p[0] != 'ud' or len(p) < 3:
            continue
        lines.append(' '.join([name, a['class'], str(p[1])] + [hex(x) for x in p[2:]]))
    return lines


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('shaders', nargs='+')
    ap.add_argument('--capture-paths', metavar='FILE', help='write KYTY_PASS_CAPTURE_PATHS lines for all shaders')
    a = ap.parse_args()
    out, lines = {}, []
    for path in a.shaders:
        code = open(path, 'rb').read()
        name = path.rsplit('/', 1)[-1].replace('shader-', '').replace('.bin', '')
        out[name] = analyse_code(code)
        lines += capture_lines(name, out[name])
    if a.capture_paths:
        open(a.capture_paths, 'w').write('\n'.join(lines) + '\n')
        print(f'{len(lines)} paths for {len(out)} shaders -> {a.capture_paths}', file=sys.stderr)
    else:
        print(json.dumps(out, indent=1))


if __name__ == '__main__':
    main()
