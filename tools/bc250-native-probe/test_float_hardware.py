#!/usr/bin/env python3
"""Native float-dot replacement checks against an exact integer sum oracle."""
from pathlib import Path
import subprocess,struct,random,json,re
from test_float_model import exact,root
from test_atomic_patches import ensure_idle
out=root/'_Build/instruction-tests/hardware-float';out.mkdir(parents=True,exist_ok=True)
cli=root/'_Build/instruction-tests/patch-float-blob'
probe=root/'_Build/instruction-tests/integer-patch-probe'
results=[]
def case(wave,form='packed',mask_kind='full',mode=0,flags=False,modifier=0,source='vgpr',seed=0):
    ensure_idle();r=random.Random(seed)
    cases=[(r.getrandbits(32),r.getrandbits(32),r.getrandbits(32)) for _ in range(64)]
    specials=(0,0x8000,1,0x3ff,0x400,0x3c00,0xbc00,0x7bff,0x7c00,0xfc00,0x7e01,0x7c01)
    for i,x in enumerate(specials):
        cases[i]=(x|(x<<16),0x3c003c00,0x80000000 if i&1 else 0)
    mask=(1<<wave)-1 if mask_kind=='full' else 0 if mask_kind=='empty' else 0x5555555555555555&((1<<wave)-1)
    dst=3 if form=='accum' else 1 if form=='alias' else 5
    src=1 if source=='sgpr' else 255 if source=='literal' else 240 if source=='inline' else 257
    literal=0xbc003c00
    setup=f's_mov_b32 s1,{literal:#x}' if source=='sgpr' else ''
    a_value=literal if source in ('sgpr','literal') else 0x3800 if source=='inline' else None
    if form=='accum':
        inst=[0x04000000|(dst<<17)|(2<<9)|src]
    else:
        inst=[0xcc134000|dst,0x18000000|src|(258<<9)|(259<<18)]
        if modifier:
            # Exchange A's half order and negate B's low/high components.
            inst[0]^=(1<<11)|(1<<9)
            inst[1]^=(1<<27)|(1<<30)
    if source=='literal':inst.append(literal)
    asm=f'''v_lshlrev_b32 v8,4,v0
s_getpc_b64 s[4:5]
s_add_u32 s4,s4,0xf0e1d2c3
s_addc_u32 s5,s5,0
global_load_dwordx4 v[1:4],v8,s[4:5]
s_waitcnt vmcnt(0)
v_mov_b32 v5,0x12345678
{setup}
s_getreg_b32 s10,hwreg(HW_REG_MODE)
s_and_b32 s11,s10,0xfffffff3
s_or_b32 s11,s11,{mode<<2}
s_setreg_b32 hwreg(HW_REG_MODE),s11
s_mov_b32 s96,0x12000060
s_mov_b32 s97,0x12000061
s_mov_b32 s98,0x12000062
s_mov_b32 s99,0x12000063
s_mov_b32 s100,0x12000064
s_mov_b32 vcc_lo,0x13579bdf
s_mov_b32 vcc_hi,0x2468ace0
s_mov_b32 exec_lo,{mask&0xffffffff:#x}
s_mov_b32 exec_hi,{mask>>32:#x}
s_cmp_eq_u32 0,0
.long {','.join(hex(x) for x in inst)}
s_cselect_b32 s13,1,0
s_getreg_b32 s12,hwreg(HW_REG_MODE)
s_mov_b64 s[14:15],vcc
s_mov_b64 exec,-1
'''
    if flags:
        asm+='v_mov_b32 v5,0\n'
        checks={**{i:0x12000000+i for i in range(96,101)},13:1,14:0x13579bdf,15:0x2468ace0}
        for reg,val in checks.items():
            asm+=f'v_xor_b32 v14,{val:#x},s{reg}\nv_or_b32 v5,v5,v14\n'
        asm+='v_xor_b32 v14,s11,s12\nv_or_b32 v5,v5,v14\n'
    else:asm+=f'v_mov_b32 v5,v{dst}\n'
    asm+='s_setreg_b32 hwreg(HW_REG_MODE),s10\nv_lshlrev_b32 v8,2,v0\nglobal_store_dword v8,v5,s[2:3]\ns_endpgm\n'
    name=f'{form}-{source}-wave{wave}-{mask_kind}-mode{mode}-flags{int(flags)}-mod{modifier}-seed{seed}'
    srcpath=out/(name+'.s');srcpath.write_text(asm)
    obj=srcpath.with_suffix('.o');original=srcpath.with_suffix('.original.bin');patched=srcpath.with_suffix('.patched.bin');ref=srcpath.with_suffix('.expected.bin')
    subprocess.run(['llvm-mc','-triple=amdgcn','-mcpu=gfx1013','-filetype=obj',str(srcpath),'-o',str(obj)],check=True)
    subprocess.run(['llvm-objcopy','-O','binary','--only-section=.text',str(obj),str(original)],check=True)
    words=list(struct.unpack('<'+str(original.stat().st_size//4)+'I',original.read_bytes()))
    literal_index=words.index(0xf0e1d2c3);getpc=literal_index-2
    assert (words[getpc]>>8)&255==0x1f
    words[literal_index]=(len(words)-getpc-1)*4
    expected=[]
    for i,(a,b,c) in enumerate(cases):
        if a_value is not None:a=a_value
        if modifier:
            a=((a&65535)<<16)|(a>>16);b^=0x80008000
        want=exact(a,b,c)
        active=(mask>>(i%wave))&1
        old=cases[i][0] if form=='alias' else cases[i][2] if form=='accum' else 0x12345678
        expected.append(0 if flags else want if active else old)
        words.extend((*cases[i],0))
    original.write_bytes(struct.pack('<'+str(len(words))+'I',*words))
    p=subprocess.run([str(cli),str(original),str(patched),'16'],check=True,capture_output=True,text=True)
    extra=int(re.search(r'scratch (\d+)',p.stdout).group(1));ref.write_bytes(struct.pack('<64I',*expected))
    result=subprocess.run([str(probe),str(patched),str(ref),str(wave),str(ref.with_suffix('.actual.bin')),str(16+extra)],capture_output=True,text=True,timeout=12)
    results.append(dict(case=name,exit=result.returncode,patch=p.stdout,stdout=result.stdout,stderr=result.stderr))
    (out/'results.json').write_text(json.dumps(results,indent=2))
    print(name,result.returncode,result.stdout.strip(),result.stderr.strip(),flush=True)
    if result.returncode:raise SystemExit(result.returncode)
if __name__=='__main__':
    for wave in (32,64):
        for form in ('packed','accum','alias'):
            for mask in ('full','partial','empty'):
                case(wave,form,mask)
                case(wave,form,mask,mode=3,flags=True)
        for mode in (1,2,3):
            case(wave,mode=mode,seed=100+mode)
        for source in ('sgpr','literal','inline'):
            case(wave,source=source)
        case(wave,modifier=1)
        for seed in range(16):case(wave,seed=1000+seed)
    print('PASS',len(results),'dispatches,',64*len(results),'lane results',flush=True)
