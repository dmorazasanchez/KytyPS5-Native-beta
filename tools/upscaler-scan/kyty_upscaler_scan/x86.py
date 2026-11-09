"""Bounded, instruction-aligned x86-64 disassembly through GNU objdump (binutils), plus small operand helpers.

Only ranges of a few KiB are disassembled (one function at a time); byte searches elsewhere are candidates
that this module confirms on instruction boundaries.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from functools import lru_cache

OBJDUMP = os.environ.get('KYTY_SCAN_OBJDUMP') or shutil.which('objdump') or '/usr/bin/objdump'
_LINE = re.compile(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$')
_MEM = re.compile(r'\[([^\]]+)\]')
_REG64 = {'rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rbp', 'rsp', 'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15'}
_ALIASES = {}
for _r in ('ax', 'bx', 'cx', 'dx', 'si', 'di', 'bp', 'sp'):
    _ALIASES['r' + _r] = 'r' + _r
    _ALIASES['e' + _r] = 'r' + _r
    _ALIASES[_r] = 'r' + _r
for _r in ('al', 'bl', 'cl', 'dl'):
    _ALIASES[_r] = 'r' + _r[0] + 'x'
for _r in ('sil', 'dil', 'bpl', 'spl'):
    _ALIASES[_r] = 'r' + _r[:2]
for _n in range(8, 16):
    for _s in ('', 'd', 'w', 'b'):
        _ALIASES[f'r{_n}{_s}'] = f'r{_n}'


class DisassemblyUnavailable(RuntimeError):
    pass


def canon(reg: str) -> str | None:
    """64-bit register a (sub)register belongs to, else None."""
    return _ALIASES.get(reg.strip().lower())


@dataclass(frozen=True)
class Mem:
    base: str | None
    index: str | None
    scale: int
    disp: int
    rip_target: int | None = None   # absolute (module RVA) target of a RIP-relative operand


@dataclass
class Insn:
    addr: int
    raw: bytes
    mnem: str
    ops: str
    comment: str

    @property
    def size(self):
        return len(self.raw)

    @property
    def end(self):
        return self.addr + len(self.raw)

    def operands(self) -> list[str]:
        out, depth, cur = [], 0, ''
        for ch in self.ops:
            if ch == '[':
                depth += 1
            elif ch == ']':
                depth -= 1
            if ch == ',' and depth == 0:
                out.append(cur.strip())
                cur = ''
            else:
                cur += ch
        if cur.strip():
            out.append(cur.strip())
        return out

    def mem(self, which: int | None = None) -> Mem | None:
        ops = self.operands()
        cand = ops if which is None else ops[which:which + 1]
        for op in cand:
            m = _MEM.search(op)
            if m:
                return parse_mem(m.group(1), self)
        return None

    def mem_operand_index(self) -> int | None:
        for i, op in enumerate(self.operands()):
            if '[' in op:
                return i
        return None

    def reg_operand(self, i: int) -> str | None:
        ops = self.operands()
        if i < len(ops) and '[' not in ops[i]:
            return canon(ops[i])
        return None

    def target(self) -> int | None:
        """Direct branch/call target."""
        if self.mnem.startswith(('call', 'j')) and self.raw[:1] in (b'\xe8', b'\xe9', b'\xeb') or \
                (self.mnem.startswith('j') and self.raw[:1] in (b'\x0f',) + tuple(bytes([b]) for b in range(0x70, 0x80))):
            m = re.match(r'^(0x)?([0-9a-f]+)', self.ops.strip())
            if m:
                return int(m.group(2), 16)
        return None

    def __str__(self):
        return f'{self.addr:#x}: {self.raw.hex(" "):<30} {self.mnem} {self.ops}'


def parse_mem(text: str, insn: Insn | None = None) -> Mem:
    text = text.replace(' ', '')
    base = index = None
    scale, disp = 1, 0
    terms = re.findall(r'[+-]?[^+-]+', text)
    for t in terms:
        sign = -1 if t.startswith('-') else 1
        t = t.lstrip('+-')
        if '*' in t:
            r, s = t.split('*')
            index, scale = canon(r) or r, int(s, 0)
        elif t.startswith('0x') or t.isdigit():
            disp += sign * int(t, 0)
        elif t == 'rip':
            base = 'rip'
        else:
            r = canon(t)
            if base is None:
                base = r or t
            else:
                index = r or t
    rip_target = None
    if base == 'rip' and insn is not None:
        rip_target = insn.end + disp
    return Mem(base, index, scale, disp, rip_target)


def _run_objdump(code: bytes, vma: int) -> list[Insn]:
    if not os.path.exists(OBJDUMP):
        raise DisassemblyUnavailable(f'GNU objdump not found ({OBJDUMP}); set KYTY_SCAN_OBJDUMP')
    with tempfile.NamedTemporaryFile(prefix='kyty-scan-', suffix='.bin') as f:
        f.write(code)
        f.flush()
        r = subprocess.run([OBJDUMP, '-D', '-b', 'binary', '-m', 'i386:x86-64', '-M', 'intel', '--insn-width=16',
                            f'--adjust-vma={vma:#x}', f.name], capture_output=True, text=True, check=False)
    if r.returncode != 0:
        raise DisassemblyUnavailable(r.stderr.strip()[:400])
    out = []
    for line in r.stdout.splitlines():
        m = _LINE.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        raw = bytes.fromhex(m.group(2).replace(' ', ''))
        rest = m.group(3)
        comment = ''
        if '#' in rest:
            rest, comment = rest.split('#', 1)
        parts = rest.strip().split(None, 1)
        mnem = parts[0] if parts else '(bad)'
        ops = parts[1] if len(parts) > 1 else ''
        # Prefixes objdump prints as separate words (e.g. "rep stos", "lock xadd", "notrack jmp").
        while mnem in ('rep', 'repz', 'repnz', 'lock', 'notrack', 'bnd', 'data16', 'cs', 'ds') and ops:
            p2 = ops.split(None, 1)
            mnem, ops = p2[0], (p2[1] if len(p2) > 1 else '')
        out.append(Insn(addr, raw, mnem, ops.strip(), comment.strip()))
    return out


class Disassembler:
    def __init__(self, image):
        self.img = image
        self._cache: dict[tuple[int, int], list[Insn]] = {}

    def range(self, start: int, end: int) -> list[Insn]:
        key = (start, end)
        if key not in self._cache:
            self._cache[key] = _run_objdump(self.img.read(start, end - start, relocated=False), start)
        return self._cache[key]

    # ---- function bounds (clang/PS5 SDK code: 16-byte aligned, int3 padded, frame pointers) ----
    def function_start(self, addr: int, max_back: int = 0x40000) -> int | None:
        """Nearest preceding 16-byte-aligned start that follows int3/ret padding, with no padding run between it
        and `addr` (so `addr` is inside that function)."""
        seg = self.img.segment_at(addr)
        if seg is None:
            return None
        lo = max(addr - max_back, seg.vaddr)
        data = self.img.read(lo, addr - lo + 1, relocated=False)
        p = (addr & ~0xf)
        while p >= lo + 1:
            off = p - lo
            prev = data[off - 1]
            if prev in (0xcc,) and data[off] != 0xcc:
                return p
            p -= 0x10
        return None

    def function_end(self, start: int, max_len: int = 0x20000) -> int:
        """First 16-byte-aligned address after `start` that begins an int3 run preceded by a ret/jmp, else a bound."""
        data = self.img.read(start, max_len, relocated=False)
        p = 1
        while p < len(data) - 1:
            # An int3 at an aligned boundary or padding run that ends at an aligned boundary.
            if data[p] == 0xcc and data[p - 1] != 0xcc:
                q = p
                while q < len(data) and data[q] == 0xcc:
                    q += 1
                if q % 0x10 == 0 and q - p >= 1:
                    return start + p
                p = q
            p += 1
        return start + max_len

    @lru_cache(maxsize=256)
    def function(self, addr: int) -> tuple[int, int, tuple[Insn, ...]] | None:
        start = self.function_start(addr)
        if start is None:
            return None
        end = self.function_end(start)
        if end <= addr:
            return None
        insns = self.range(start, end)
        if not any(i.addr == addr for i in insns) and not any(i.addr <= addr < i.end for i in insns):
            return None
        return start, end, tuple(insns)

    def insn_at(self, addr: int) -> Insn | None:
        f = self.function(addr)
        if f is None:
            # Not inside a padded function: decode a short window starting exactly here.
            insns = self.range(addr, addr + 16)
            return insns[0] if insns and insns[0].addr == addr else None
        for i in f[2]:
            if i.addr == addr:
                return i
        return None


def is_boundary(insns, addr):
    return any(i.addr == addr for i in insns)
