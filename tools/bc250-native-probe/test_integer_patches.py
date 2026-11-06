#!/usr/bin/env python3
"""Exercise only the generated gfx1013 replacements, never raw unsupported dots."""
from pathlib import Path
import subprocess,struct,json
root=Path(__file__).resolve().parents[2]
out=root/'_Build/instruction-tests/hardware'
out.mkdir(parents=True,exist_ok=True)
cli=root/'_Build/instruction-tests/patch-integer-blob'
probe=root/'_Build/instruction-tests/integer-patch-probe'
def reference(a,b,c,bits,sign):
 total=c
 for offset in range(0,32,bits):
  mask=(1<<bits)-1
  x=(a>>offset)&mask;y=(b>>offset)&mask
  if sign and x&(1<<(bits-1)):x-=1<<bits
  if sign and y&(1<<(bits-1)):y-=1<<bits
  total+=x*y
 return total&0xffffffff
results=[]
for op in range(0x14,0x1a):
 for alias in [False,True]:
  dst=2 if alias else 5
  word0=0xcc004000|(op<<16)|dst
  word1=0x18000000|258|(259<<9)|(260<<18)
  asm=f'''v_lshlrev_b32 v1, 2, v0
v_xor_b32 v2, 0x80ff7f01, v0
v_xor_b32 v3, 0x017f80ff, v0
v_mov_b32 v4, 0x7ffffff0
.long {word0:#x}, {word1:#x}
global_store_dword v1, v{dst}, s[2:3]
s_endpgm
'''
  name=f'dot-{op:x}-alias{int(alias)}';src=out/(name+'.s')
  src.write_text(asm)
  subprocess.run(['llvm-mc','-triple=amdgcn','-mcpu=gfx1013','-filetype=obj',str(src),'-o',str(out/(name+'.o'))],check=True)
  subprocess.run(['llvm-objcopy','-O','binary','--only-section=.text',str(out/(name+'.o')),str(out/(name+'.original.bin'))],check=True)
  subprocess.run([str(cli),str(out/(name+'.original.bin')),str(out/(name+'.patched.bin')),'16'],check=True)
  bits=16 if op<=0x15 else 8 if op<=0x17 else 4
  expected=[reference(0x80ff7f01^i,0x017f80ff^i,0x7ffffff0,bits,op%2==0) for i in range(64)]
  ref=out/(name+'.expected.bin');ref.write_bytes(struct.pack('<64I',*expected))
  for wave in [64,32]:
   run=subprocess.run([str(probe),str(out/(name+'.patched.bin')),str(ref),str(wave)],capture_output=True,text=True,timeout=12)
   print(name,'wave',wave,'exit',run.returncode,run.stdout.strip(),run.stderr.strip(),flush=True)
   results.append({'case':name,'wave':wave,'exit':run.returncode,'stdout':run.stdout,'stderr':run.stderr})
   if run.returncode:
    (out/'results.json').write_text(json.dumps(results,indent=2))
    raise SystemExit(run.returncode)
# VOP2 accumulating signed dot4.
name='dot4c'
asm='''v_lshlrev_b32 v1, 2, v0
v_xor_b32 v2, 0x80ff7f01, v0
v_xor_b32 v3, 0x017f80ff, v0
v_mov_b32 v5, 0x7ffffff0
.long 0x1a0a0702
global_store_dword v1, v5, s[2:3]
s_endpgm
'''
src=out/(name+'.s');src.write_text(asm)
subprocess.run(['llvm-mc','-triple=amdgcn','-mcpu=gfx1013','-filetype=obj',str(src),'-o',str(out/(name+'.o'))],check=True)
subprocess.run(['llvm-objcopy','-O','binary','--only-section=.text',str(out/(name+'.o')),str(out/(name+'.original.bin'))],check=True)
subprocess.run([str(cli),str(out/(name+'.original.bin')),str(out/(name+'.patched.bin')),'16'],check=True)
ref=out/(name+'.expected.bin');ref.write_bytes(struct.pack('<64I',*[reference(0x80ff7f01^i,0x017f80ff^i,0x7ffffff0,8,True) for i in range(64)]))
for wave in [64,32]:
 run=subprocess.run([str(probe),str(out/(name+'.patched.bin')),str(ref),str(wave)],capture_output=True,text=True,timeout=12)
 print(name,'wave',wave,'exit',run.returncode,run.stdout.strip(),run.stderr.strip(),flush=True)
 results.append({'case':name,'wave':wave,'exit':run.returncode,'stdout':run.stdout,'stderr':run.stderr})
 if run.returncode: raise SystemExit(run.returncode)
(out/'results.json').write_text(json.dumps(results,indent=2))
print('PASS',len(results),'hardware dispatches,',len(results)*64,'lane results',flush=True)
