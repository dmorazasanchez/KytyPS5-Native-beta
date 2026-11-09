"""Synthetic code (assembled with llvm-mc) for the cases a new game can present: the PSML parameter block as a global
or a stack block behind a wrapper, and the cases that must NOT verify (another revision's layout, a clobbered or
unresolvable parameter register). No game files are needed."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from kyty_upscaler_scan import psml, selfelf, x86  # noqa: E402
from kyty_upscaler_scan.x86 import parse_mem  # noqa: E402

LLVM_MC = shutil.which('llvm-mc')
OBJCOPY = shutil.which('llvm-objcopy') or shutil.which('objcopy')
VA = 0x10000


def assemble(body: str) -> bytes:
    src = '.intel_syntax noprefix\n.text\n.fill 16, 1, 0xcc\n' + body
    with tempfile.TemporaryDirectory() as d:
        s, o, b = Path(d, 'a.s'), Path(d, 'a.o'), Path(d, 'a.bin')
        s.write_text(src)
        subprocess.run([LLVM_MC, '-triple=x86_64', '-filetype=obj', '-o', str(o), str(s)], check=True)
        subprocess.run([OBJCOPY, '-O', 'binary', '--only-section=.text', str(o), str(b)], check=True)
        return b.read_bytes()


class CodeImage(selfelf.SelfImage):
    """A bare code segment with one import whose PLT stub is the first instruction after the leading padding."""

    def __init__(self, code: bytes, nid: str):
        seg = selfelf.Segment(0, (selfelf.PT_LOAD, 5, 0, VA, 0, len(code), len(code) + 0x1000, 0x10), code)
        self.segments = self.loaded = [seg]
        self._starts = [VA]
        self._relocated = {}
        self.relative = {}
        imp = selfelf.Import(nid, 'libScePsml', 1, 'libScePsml', 0, nid + '#A#A')
        imp.plt_rvas = [VA + 16]
        self.imports = {nid: imp}
        self.plt = {VA + 16: imp}


PLT = """
plt:
  jmp qword ptr [rip + 0x2000]
.p2align 4, 0xcc
"""


def run(body, nid='94iBp3KvIuI'):
    img = CodeImage(assemble(PLT + body), nid)
    return psml.analyse_dispatch(img, x86.Disassembler(img), nid)


@unittest.skipUnless(LLVM_MC and OBJCOPY and os.path.exists(x86.OBJDUMP), 'llvm-mc/objcopy/objdump needed')
class PsmlSynthetic(unittest.TestCase):
    def test_global_block_r2(self):
        r = run("""
game:
  push rbp
  mov rbp, rsp
  mov qword ptr [rip + block + 0x00], rcx
  mov qword ptr [rip + block + 0x08], rax
  mov qword ptr [rip + block + 0x10], rdx
  mov qword ptr [rip + block + 0x20], rsi
  vmovups xmmword ptr [rip + block + 0x100], xmm0
  mov byte ptr [rip + block + 0x11c], 1
  lea rdx, [rip + block]
  call plt
  pop rbp
  ret
.p2align 4, 0xcc
block:
  .fill 0x160, 1, 0
""")
        self.assertEqual(r['state'], 'static_layout_verified', r['checks'])
        site = r['callsites'][0]
        self.assertEqual(site['parameter_block']['kind'], 'global')
        self.assertTrue(site['resources']['motion_vectors']['stored_from_register'])

    def test_stack_block_through_wrapper_r1(self):
        r = run("""
wrapper:
  push rbp
  mov rbp, rsp
  push r14
  mov r14, rdx
  call other
  mov rdx, r14
  call plt
  pop r14
  pop rbp
  ret
.p2align 4, 0xcc
other:
  ret
.p2align 4, 0xcc
game:
  push rbp
  mov rbp, rsp
  sub rsp, 0x200
  mov qword ptr [rbp - 0x180], rcx
  mov qword ptr [rbp - 0x178], rax
  mov qword ptr [rbp - 0x170], rdx
  mov qword ptr [rbp - 0x160], r13
  vmovups xmmword ptr [rbp - 0x180 + 0xf8], xmm1
  mov byte ptr [rbp - 0x180 + 0x114], 1
  lea rdx, [rbp - 0x180]
  call wrapper
  leave
  ret
""", nid='RUNLFro+qok')
        self.assertEqual(r['state'], 'static_layout_verified', r['checks'])
        site = r['callsites'][0]
        self.assertEqual(site['parameter_block']['frame_offset'], -0x180)
        self.assertEqual(len(site['chain']), 2)  # wrapper -> game

    def test_other_revision_layout_rejected(self):
        # Imports the 1.007 dispatcher but fills the older block (jitter 0xf8, reset 0x114).
        r = run("""
game:
  push rbp
  mov rbp, rsp
  mov qword ptr [rip + block + 0x00], rcx
  mov qword ptr [rip + block + 0x08], rax
  mov qword ptr [rip + block + 0x10], rdx
  mov qword ptr [rip + block + 0x20], rsi
  vmovsd qword ptr [rip + block + 0xf8], xmm0
  mov byte ptr [rip + block + 0x114], 1
  lea rdx, [rip + block]
  call plt
  pop rbp
  ret
.p2align 4, 0xcc
block:
  .fill 0x160, 1, 0
""")
        self.assertNotEqual(r['state'], 'static_layout_verified')

    def test_clobbered_parameter_register_unresolved(self):
        r = run("""
helper:
  ret
.p2align 4, 0xcc
game:
  push rbp
  mov rbp, rsp
  lea rdx, [rip + block]
  call helper
  call plt
  pop rbp
  ret
.p2align 4, 0xcc
block:
  .fill 0x160, 1, 0
""")
        self.assertEqual(r['state'], 'candidate')
        self.assertEqual(r['callsites'][0]['parameter_value'], 'unknown')

    def test_pointer_loaded_from_memory_unresolved(self):
        r = run("""
game:
  push rbp
  mov rbp, rsp
  mov rdx, qword ptr [rdi + 0x40]
  call plt
  pop rbp
  ret
""")
        self.assertEqual(r['state'], 'candidate')
        self.assertEqual(r['callsites'][0]['parameter_block']['kind'], 'pointer_loaded')

    def test_no_callers(self):
        r = run("""
game:
  ret
""")
        self.assertEqual(r['state'], 'candidate')


class Units(unittest.TestCase):
    def test_encode_id(self):
        self.assertEqual(selfelf.encode_id(0), 'A')
        self.assertEqual(selfelf.encode_id(63), '-')
        self.assertEqual(selfelf.encode_id(64), 'BA')

    def test_parse_mem(self):
        m = parse_mem('rbp-0x180')
        self.assertEqual((m.base, m.disp), ('rbp', -0x180))
        m = parse_mem('r12+rax*8+0x10')
        self.assertEqual((m.base, m.index, m.scale, m.disp), ('r12', 'rax', 8, 0x10))


if __name__ == '__main__':
    unittest.main()
