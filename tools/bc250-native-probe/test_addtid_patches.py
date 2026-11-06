#!/usr/bin/env python3
"""Verify lane-addressed replacements using native loads/stores and bounded memory."""
from pathlib import Path
import subprocess, struct, json
root = Path(__file__).resolve().parents[2]
out = root / '_Build/instruction-tests/hardware-addtid'
out.mkdir(parents=True, exist_ok=True)
cli = root / '_Build/instruction-tests/patch-integer-blob'
probe = root / '_Build/instruction-tests/integer-patch-probe'
results = []
def run_case(name, asm, expected):
    src = out / (name + '.s')
    src.write_text(asm)
    subprocess.run(['llvm-mc', '-triple=amdgcn', '-mcpu=gfx1013', '-filetype=obj', str(src), '-o', str(src.with_suffix('.o'))], check=True)
    original = src.with_suffix('.original.bin')
    patched = src.with_suffix('.patched.bin')
    subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=.text', str(src.with_suffix('.o')), str(original)], check=True)
    subprocess.run([str(cli), str(original), str(patched), '16'], check=True)
    for wave in (32, 64):
        ref = out / (name + f'-wave{wave}.expected.bin')
        ref.write_bytes(struct.pack('<64I', *expected(wave)))
        result = subprocess.run([str(probe), str(patched), str(ref), str(wave)], capture_output=True, text=True, timeout=12)
        results.append(dict(case=name, wave=wave, exit=result.returncode, stdout=result.stdout, stderr=result.stderr))
        (out / 'results.json').write_text(json.dumps(results, indent=2))
        print(name, wave, result.returncode, result.stdout.strip(), result.stderr.strip(), flush=True)
        if result.returncode:
            raise SystemExit(result.returncode)
for offset, adjust in ((0, 0), (16, 0), (-28, 64)):
    for masked in (False, True):
        raw0 = 0xdc588000 | (offset & 0xfff)
        raw1 = 0x05080000
        before = 's_mov_b32 exec_lo, 0x55555555' if masked else ''
        after = 's_mov_b64 exec, -1' if masked else ''
        asm = f'''s_add_u32 s8, s0, {adjust}
s_addc_u32 s9, s1, 0
v_lshlrev_b32 v1, 2, v0
v_mov_b32 v5, 0x12345678
{before}
.long {raw0:#x}, {raw1:#x}
s_waitcnt vmcnt(0)
{after}
global_store_dword v1, v5, s[2:3]
s_endpgm
'''
        def expected(wave, offset=offset, adjust=adjust, masked=masked):
            return [0xabc00000 + (adjust + offset) // 4 + (i % wave)
                    if not masked or i % wave >= 32 or i % 2 == 0 else 0x12345678 for i in range(64)]
        # Initialize enough source values for positive offsets and wave64.
        run_case(f'load-{offset}-mask{int(masked)}', asm, expected)
for masked in (False, True):
    before = 's_mov_b32 exec_lo, 0x55555555' if masked else ''
    after = 's_mov_b64 exec, -1' if masked else ''
    asm = f'''v_lshlrev_b32 v1, 2, v0
v_and_b32 v5, 31, v0
v_or_b32 v5, 0x12340000, v5
{before}
.long 0xdc5c8000, 0x00000500
s_waitcnt vmcnt(0)
{after}
s_barrier
global_load_dword v6, v1, s[0:1]
s_waitcnt vmcnt(0)
global_store_dword v1, v6, s[2:3]
s_endpgm
'''
    def expected(wave, masked=masked):
        return [0x12340000 + i % 32 if i < wave and (not masked or i >= 32 or i % 2 == 0)
                else 0xabc00000 + i for i in range(64)]
    run_case(f'store-mask{int(masked)}', asm, expected)
print('PASS', len(results), 'dispatches,', len(results) * 64, 'lane results', flush=True)
