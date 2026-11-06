#!/usr/bin/env python3
"""Measured 2D BY reads, native T# conversion, bounds/alignment and mip selection."""
from pathlib import Path
import subprocess,struct,json
from test_atomic_patches import root,ensure_idle
out=root/'_Build/instruction-tests/hardware-image-by';out.mkdir(parents=True,exist_ok=True)
cli=root/'_Build/instruction-tests/patch-image-blob';probe=root/'_Build/instruction-tests/integer-patch-probe'
results=[]
def case(wave,op=0x42,dmask=3,component=0,mask_kind='full',width=64,y=0,lod=0,flags=False,nsa=False):
    ensure_idle()
    count=4 if op&1 else 2;components=(4 if dmask==15 else 2)//count;mip=bool(op&8)
    mask=(1<<wave)-1 if mask_kind=='full' else 0 if mask_kind=='empty' else 0x5555555555555555&((1<<wave)-1)
    raw0=0xf0000008|(op<<18)|(dmask<<8)|(2 if nsa else 0)
    raw1=0x00010a01
    inst=[raw0,raw1]
    if nsa:
        inst[1]=(raw1&~255)|3;inst.append(0x00000201)
        coords=f'v_mov_b32 v3,v0\nv_mov_b32 v1,{y}\nv_mov_b32 v2,{lod}'
    else:coords=f'v_mov_b32 v1,v0\nv_mov_b32 v2,{y}\nv_mov_b32 v3,{lod}'
    asm=f'''v_lshlrev_b32 v15,2,v0
{coords}
s_lshr_b64 s[4:5],s[0:1],8
s_and_b32 s5,s5,0xff
s_or_b32 s5,s5,{((62 if components==2 else 20)<<20)|(((width-1)&3)<<30):#x}
s_mov_b32 s6,{((width-1)>>2)|0x80000000:#x}
s_mov_b32 s7,0x90000fac
s_mov_b32 s8,0
s_mov_b32 s9,0
s_mov_b32 s10,0
s_mov_b32 s11,0
v_mov_b32 v10,0xcafe0000
v_mov_b32 v11,0xcafe0001
v_mov_b32 v12,0xcafe0002
v_mov_b32 v13,0xcafe0003
s_mov_b32 s96,0x12000060
s_mov_b32 s97,0x12000061
s_mov_b32 s98,0x12000062
s_mov_b32 vcc_lo,0x13579bdf
s_mov_b32 vcc_hi,0x2468ace0
s_mov_b32 exec_lo,{mask&0xffffffff:#x}
s_mov_b32 exec_hi,{mask>>32:#x}
s_cmp_eq_u32 0,0
.long {','.join(hex(x) for x in inst)}
s_cselect_b32 s12,1,0
s_mov_b64 s[14:15],vcc
s_mov_b64 exec,-1
'''
    if flags:
        asm+='v_mov_b32 v14,0\n'
        for reg,val in {96:0x12000060,97:0x12000061,98:0x12000062,12:1,14:0x13579bdf,15:0x2468ace0}.items():
            asm+=f'v_xor_b32 v9,{val:#x},s{reg}\nv_or_b32 v14,v14,v9\n'
    else:asm+=f'v_mov_b32 v14,v{10+component}\n'
    asm+='global_store_dword v15,v14,s[2:3]\ns_endpgm\n'
    name=f'op{op:x}-mask{dmask:x}-c{component}-wave{wave}-{mask_kind}-width{width}-y{y}-lod{lod}-flags{int(flags)}-nsa{int(nsa)}'
    src=out/(name+'.s');src.write_text(asm);obj=src.with_suffix('.o');original=src.with_suffix('.original.bin');patched=src.with_suffix('.patched.bin');ref=src.with_suffix('.expected.bin')
    subprocess.run(['llvm-mc','-triple=amdgcn','-mcpu=gfx1013','-filetype=obj',str(src),'-o',str(obj)],check=True)
    subprocess.run(['llvm-objcopy','-O','binary','--only-section=.text',str(obj),str(original)],check=True)
    p=subprocess.run([str(cli),str(original),str(patched),'16'],capture_output=True,text=True,check=True)
    expected=[]
    for i in range(64):
        if flags:want=0
        elif not (mask>>(i%wave))&1:want=0xcafe0000+component
        elif component>=count*components:want=0xcafe0000+component
        elif i>=width or width-i<count or y!=0 or (mip and lod!=0):want=0
        else:
            element=component//components;channel=component%components
            want=0xabc00000+((i&~(count-1))+element)*components+channel
        expected.append(want)
    ref.write_bytes(struct.pack('<64I',*expected))
    result=subprocess.run([str(probe),str(patched),str(ref),str(wave),str(ref.with_suffix('.actual.bin')),'30'],capture_output=True,text=True,timeout=12)
    results.append(dict(case=name,exit=result.returncode,patch=p.stdout,stdout=result.stdout,stderr=result.stderr))
    (out/'results.json').write_text(json.dumps(results,indent=2))
    print(name,result.returncode,result.stdout.strip(),result.stderr.strip(),flush=True)
    if result.returncode:raise SystemExit(result.returncode)
if __name__=='__main__':
    for wave in (32,64):
        for op,mask in ((0x42,3),(0x42,15),(0x43,15),(0x4a,3),(0x4a,15),(0x4b,15)):
            for c in range(4):case(wave,op,mask,c)
            for mode in ('partial','empty'):case(wave,op,mask,mask_kind=mode)
            case(wave,op,mask,width=61)
            case(wave,op,mask,y=1)
            case(wave,op,mask,lod=1)
            case(wave,op,mask,flags=True)
            case(wave,op,mask,nsa=True)
    print('PASS',len(results),'dispatches,',len(results)*64,'lane results',flush=True)
