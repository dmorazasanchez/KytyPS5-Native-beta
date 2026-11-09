"""Small, conservative register dataflow over a linear instruction list (one function)."""
from __future__ import annotations

from dataclasses import dataclass

from .x86 import Insn, Mem, canon

ARG_REGS = ('rdi', 'rsi', 'rdx', 'rcx', 'r8', 'r9')
CALLER_SAVED = {'rax', 'rcx', 'rdx', 'rsi', 'rdi', 'r8', 'r9', 'r10', 'r11'}
STORE_MNEMS = ('mov', 'movq', 'movd', 'movabs', 'movups', 'movaps', 'movdqu', 'movdqa', 'movss', 'movsd', 'movlps',
               'movhps', 'movnti', 'vmovups', 'vmovaps', 'vmovdqu', 'vmovdqa', 'vmovss', 'vmovsd', 'vmovq', 'vmovd',
               'vmovlps', 'vmovhps', 'vmovdqu8', 'vmovdqu16', 'vmovdqu32', 'vmovdqu64', 'vextractf128', 'vextracti128')
_SIZES = {'BYTE': 1, 'WORD': 2, 'DWORD': 4, 'QWORD': 8, 'XMMWORD': 16, 'YMMWORD': 32, 'ZMMWORD': 64}


@dataclass(frozen=True)
class Value:
    kind: str                 # 'address' (lea), 'load' (mov from memory), 'arg' (incoming), 'imm', 'unknown'
    mem: Mem | None = None
    arg: str | None = None
    imm: int | None = None
    at: int | None = None     # defining instruction
    why: str = ''


def mem_size(insn: Insn, operand: int = 0) -> int | None:
    ops = insn.operands()
    if operand >= len(ops):
        return None
    first = ops[operand].split()[0].upper() if ops[operand] else ''
    return _SIZES.get(first)


def is_store(insn: Insn) -> bool:
    return (insn.mnem in STORE_MNEMS or insn.mnem.startswith('mov')) and insn.mem_operand_index() == 0


def defines(insn: Insn) -> str | None:
    """64-bit register written by `insn` as its first operand (None for stores/compares/branches)."""
    if insn.mnem.startswith(('cmp', 'test', 'j', 'call', 'push', 'ret', 'nop', 'ud2', 'int3')) or not insn.ops:
        return None
    if insn.mnem.startswith('pop'):
        return insn.reg_operand(0)
    return insn.reg_operand(0)


def value_of(insns: list[Insn] | tuple[Insn, ...], index: int, reg: str, depth: int = 0) -> Value:
    """The value `reg` holds just before insns[index], scanning backwards linearly (branches ignored)."""
    reg = canon(reg) or reg
    for j in range(index - 1, -1, -1):
        i = insns[j]
        if i.mnem.startswith('call'):
            if reg in CALLER_SAVED:
                return Value('unknown', at=i.addr, why=f'{reg} clobbered by call at {i.addr:#x}')
            continue
        if defines(i) != reg:
            continue
        ops = i.operands()
        if i.mnem == 'lea':
            return Value('address', mem=i.mem(1), at=i.addr)
        if i.mnem in ('mov', 'movabs') and len(ops) == 2:
            if '[' in ops[1]:
                return Value('load', mem=i.mem(1), at=i.addr)
            src = canon(ops[1])
            if src is not None:
                if depth > 12:
                    return Value('unknown', at=i.addr, why='copy chain too long')
                return value_of(insns, j, src, depth + 1)
            try:
                return Value('imm', imm=int(ops[1], 0), at=i.addr)
            except ValueError:
                pass
        if i.mnem == 'xor' and len(ops) == 2 and canon(ops[0]) == canon(ops[1]):
            return Value('imm', imm=0, at=i.addr)
        return Value('unknown', at=i.addr, why=f'{i.mnem} {i.ops}')
    if reg in ARG_REGS:
        return Value('arg', arg=reg)
    return Value('unknown', why='no definition before function start')


def block_stores(insns, block: Value, size: int, exclude_after: int | None = None):
    """Stores into [block, block+size) in `insns`: through the block's own addressing (rip/rbp form) or through a
    register that holds its address (lea reg, block; mov reg2, reg). Returns (offset, bytes, insn, source)."""
    out = []
    holders: dict[str, int] = {}     # register -> offset of the block it points at
    bm = block.mem

    def block_offset(m: Mem) -> int | None:
        if m is None:
            return None
        if bm.base == 'rip' and m.base == 'rip' and m.rip_target is not None and bm.rip_target is not None:
            off = m.rip_target - bm.rip_target
        elif bm.base not in (None, 'rip') and m.base == bm.base and m.index is None and bm.index is None:
            off = m.disp - bm.disp
        elif m.base in holders and m.index is None:
            off = holders[m.base] + m.disp
        else:
            return None
        return off if 0 <= off < size else None

    for i in insns:
        if exclude_after is not None and i.addr > exclude_after:
            break
        ops = i.operands()
        d = defines(i)
        if i.mnem == 'lea' and d is not None:
            off = block_offset(i.mem(1))
            if off is not None:
                holders[d] = off
                continue
        if i.mnem == 'mov' and d is not None and len(ops) == 2 and canon(ops[1]) in holders:
            holders[d] = holders[canon(ops[1])]
            continue
        if is_store(i):
            off = block_offset(i.mem(0))
            if off is not None:
                n = mem_size(i, 0) or 0
                src = ops[1] if len(ops) > 1 else ''
                out.append((off, n, i, src))
                continue
        if d is not None and d in holders:
            del holders[d]
        if i.mnem.startswith('call'):
            for r in list(holders):
                if r in CALLER_SAVED:
                    del holders[r]
    return out
