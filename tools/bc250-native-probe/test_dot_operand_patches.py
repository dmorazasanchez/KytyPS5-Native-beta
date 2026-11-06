#!/usr/bin/env python3
"""Check integer dot operand forms, EXEC masking and unchanged flag state."""
from pathlib import Path
import subprocess, struct, json
root=Path(__file__).resolve().parents[2]
out=root/'_Build/instruction-tests/hardware-operands'
out.mkdir(parents=True, exist_ok=True)
cli=root/'_Build/instruction-tests/patch-integer-blob'
probe=root/'_Build/instruction-tests/integer-patch-probe'
results=[]
def dot(a,b,c):
    values=lambda x: [((x>>(8*j)&255)^128)-128 for j in range(4)]
    return (c+sum(x*y for x,y in zip(values(a),values(b))))&0xffffffff
def run_case(name, opwords, before='', after='', expected=None):
    asm=f'''v_lshlrev_b32 v1, 2, v0
v_xor_b32 v2, 0x80ff7f01, v0
v_xor_b32 v3, 0x017f80ff, v0
v_mov_b32 v5, 0x7ffffff0
s_mov_b32 s8, 0x80ff7f01
s_mov_b64 vcc, 0x12345678
s_cmp_eq_u32 s8, s8
{before}
.long {', '.join(hex(x) for x in opwords)}
{after}
s_cbranch_scc0 fail
s_cmp_eq_u32 vcc_lo, 0x12345678
s_cbranch_scc0 fail
global_store_dword v1, v5, s[2:3]
s_endpgm
fail:
v_mov_b32 v5, 0xbadbad
global_store_dword v1, v5, s[2:3]
s_endpgm
'''
    src=out/(name+'.s');src.write_text(asm)
    subprocess.run(['llvm-mc','-triple=amdgcn','-mcpu=gfx1013','-filetype=obj',str(src),'-o',str(src.with_suffix('.o'))],check=True)
    original=src.with_suffix('.original.bin');patched=src.with_suffix('.patched.bin')
    subprocess.run(['llvm-objcopy','-O','binary','--only-section=.text',str(src.with_suffix('.o')),str(original)],check=True)
    subprocess.run([str(cli),str(original),str(patched),'16'],check=True)
    for wave in (32,64):
        ref=out/(name+f'-wave{wave}.expected.bin')
        ref.write_bytes(struct.pack('<64I',*[expected(i,wave) for i in range(64)]))
        p=subprocess.run([str(probe),str(patched),str(ref),str(wave)],capture_output=True,text=True,timeout=12)
        results.append(dict(case=name,wave=wave,exit=p.returncode,stdout=p.stdout,stderr=p.stderr))
        (out/'results.json').write_text(json.dumps(results,indent=2))
        print(name,wave,p.returncode,p.stdout.strip(),p.stderr.strip(),flush=True)
        if p.returncode: raise SystemExit(p.returncode)
for src,name,extra in ((8,'sgpr',[]),(255,'literal',[0x80ff7f01])):
    run_case(name,[0xcc164005,0x18000000|src|(259<<9)|(261<<18),*extra],
             expected=lambda i,w:dot(0x80ff7f01,0x017f80ff^i,0x7ffffff0))
for marker,ctrl,name in ((250,0xff00b102,'dpp16'),(234,0xde54c102,'dpp8-fi'),(233,0xde54c102,'dpp8')):
    run_case(name,[0x1a0a0600|marker,ctrl],
             expected=lambda i,w:dot(0x80ff7f01^(i^1),0x017f80ff^i,0x7ffffff0))
    # FI=1 reads source values even when that lane is masked. Otherwise zero is fetched.
    run_case(name+'-masked',[0x1a0a0600|marker,ctrl],
             before='s_mov_b32 exec_lo, 0x55555555',after='s_mov_b64 exec, -1',
             expected=lambda i,w,marker=marker: (0x7ffffff0 if i%w<32 and i%2 else
                 dot((0x80ff7f01^(i^1)) if marker==234 or i%w>=32 else 0,
                     0x017f80ff^i,0x7ffffff0)))
for masked in (False, True):
    run_case('readfirstlane-mask'+str(int(masked)),[0xd5820008,0x102],
             before='s_mov_b32 exec_lo, 0x55555554' if masked else '',
             after='v_mov_b32 v5, s8\ns_mov_b64 exec, -1',
             expected=lambda i,w,masked=masked: 0x7ffffff0 if masked and i%w<32 and (i%2 or i%w==0)
                 else 0x80ff7f01 ^ (i//w*w + (2 if masked else 0)))
run_case('local-constant',[0xcc164005,0x18000000|258|(259<<9)|(261<<18)],
    after='''s_getpc_b64 s[8:9]
pc_after:
.long 0x8008ff08, constant_data - pc_after, 0x82098009
s_load_dword s10, s[8:9], 0
s_waitcnt lgkmcnt(0)
v_mov_b32 v5, s10
s_cmp_eq_u32 s8, s8
s_branch continue_test
constant_data:
.long 0x5a1234ef
continue_test:
''',expected=lambda i,w:0x5a1234ef)
print('PASS',len(results),'dispatches,',len(results)*64,'lane results',flush=True)
