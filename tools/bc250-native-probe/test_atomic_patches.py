#!/usr/bin/env python3
"""Hardware checks: only validated native gfx1013 replacements are submitted."""
from pathlib import Path
import subprocess, struct, json, os
root=Path(__file__).resolve().parents[2]
out=root/'_Build/instruction-tests/hardware-atomic'
out.mkdir(parents=True,exist_ok=True)
cli=root/'_Build/instruction-tests/patch-integer-blob'
probe=root/'_Build/instruction-tests/integer-patch-probe'
results=[]
def ensure_idle():
    for p in Path('/proc').glob('[0-9]*/exe'):
        try:
            if Path(os.readlink(p)).name=='kyty_emulator':
                raise SystemExit('Stop: a game is running; no GPU probe was submitted.')
        except (FileNotFoundError,PermissionError,ProcessLookupError): pass
def run(name, asm, wave, check):
    ensure_idle()
    src=out/(name+f'-wave{wave}.s'); src.write_text(asm)
    obj=src.with_suffix('.o'); original=src.with_suffix('.original.bin')
    patched=src.with_suffix('.patched.bin'); actual=src.with_suffix('.actual.bin')
    subprocess.run(['llvm-mc','-triple=amdgcn','-mcpu=gfx1013',
                    '-mattr='+('+wavefrontsize64,-wavefrontsize32' if wave==64 else '+wavefrontsize32,-wavefrontsize64'),
                    '-filetype=obj',str(src),'-o',str(obj)],check=True)
    subprocess.run(['llvm-objcopy','-O','binary','--only-section=.text',str(obj),str(original)],check=True)
    subprocess.run([str(cli),str(original),str(patched),'16'],check=True)
    result=subprocess.run([str(probe),str(patched),'-',str(wave),str(actual)],capture_output=True,text=True,timeout=12)
    entry=dict(case=name,wave=wave,exit=result.returncode,stdout=result.stdout,stderr=result.stderr)
    results.append(entry); (out/'results.json').write_text(json.dumps(results,indent=2))
    print(name,wave,result.returncode,result.stdout.strip(),result.stderr.strip(),flush=True)
    if result.returncode: raise SystemExit(result.returncode)
    got=struct.unpack('<64I',actual.read_bytes())
    check(got)
def case(family,wave,mask_mode,glc,shared,sub,memory,output,soffset=128,offset=0,scc=1):
    full=(1<<wave)-1
    mask=full if mask_mode=='full' else 0 if mask_mode=='empty' else full & 0x5555555555555555
    address='v_mov_b32 v1, 0' if shared else 'v_lshlrev_b32 v1, 2, v0'
    # Shift the scalar base opposite the instruction offset, keeping all access inside the BO.
    setup=f's_add_u32 s8,s0,{-offset}\ns_addc_u32 s9,s1,0' if offset<0 else 's_mov_b64 s[8:9],s[0:1]'
    if family=='buffer':
        setup+='\ns_mov_b32 s4,s8\ns_and_b32 s5,s9,0xffff\ns_mov_b32 s6,512\ns_mov_b32 s7,0x31016fac'
        raw0=0xe0d01000 | (int(glc)<<14) | (offset & 4095)
        raw1=(soffset<<24)|(1<<16)|(2<<8)|1
        dest=2
    elif family=='global':
        raw0=0xdcd08000 | (int(glc)<<16) | (offset & 4095)
        raw1=(5<<24)|(8<<16)|(2<<8)|1
        dest=5
    else:
        carry='vcc' if wave==64 else 'vcc_lo'
        setup+=f'\nv_add_co_u32 v8,{carry},s8,v1\nv_add_co_ci_u32 v9,{carry},s9,0,{carry}'
        raw0=0xdcd08000 | (int(glc)<<16) | (offset & 4095)
        raw1=(5<<24)|(125<<16)|(2<<8)|8
        dest=5
    sentinels={r:0x12340000+r for r in list(range(88,93))+list(range(96,101))}
    if soffset<106: sentinels[soffset]=0
    setup+='\n'+'\n'.join(f's_mov_b32 s{r}, {v:#x}' for r,v in sentinels.items())
    # Original destination is initialized so inactive and no-return results are testable.
    before=f'''v_lshlrev_b32 v7,2,v0
{address}
v_mov_b32 v2,{sub:#x}
v_mov_b32 v5,0xdeadbeef
v_mov_b32 v6,{memory:#x}
global_store_dword v1,v6,s[0:1]
s_waitcnt_vscnt null,0
s_barrier
{setup}
s_mov_b32 vcc_lo,0x13579bdf
s_mov_b32 vcc_hi,0x2468ace0
s_mov_b32 exec_lo,{mask&0xffffffff:#x}
s_mov_b32 exec_hi,{mask>>32:#x}
s_cmp_eq_u32 0,{0 if scc else 1}
.long {raw0:#x},{raw1:#x}
s_cselect_b32 s10,1,0
s_mov_b64 s[12:13],exec
s_mov_b64 s[14:15],vcc
s_mov_b64 exec,-1
s_waitcnt vmcnt(0)
s_barrier
'''
    if output=='memory':
        after='global_load_dword v6,v1,s[0:1]\ns_waitcnt vmcnt(0)'
    elif output=='old':
        after=f'v_mov_b32 v6,v{dest}'
    else:
        checks={**sentinels,10:scc,12:mask&0xffffffff,13:mask>>32,14:0x13579bdf,15:0x2468ace0}
        after='v_mov_b32 v6,0\n'
        for reg,value in checks.items():
            after+=f'v_xor_b32 v14,{value:#x},s{reg}\nv_or_b32 v6,v6,v14\n'
    asm=before+after+'\nglobal_store_dword v7,v6,s[2:3]\ns_endpgm\n'
    def check(got):
        if output=='flags':
            expected=[0]*64
        elif output=='memory':
            if shared: expected=[max(memory-sub*mask.bit_count()*(64//wave),0)]*64
            else: expected=[max(memory-sub,0) if (mask>>(i%wave))&1 else memory for i in range(64)]
        elif not shared:
            expected=[memory if glc and (mask>>(i%wave))&1 else sub if family=='buffer' else 0xdeadbeef for i in range(64)]
        else:
            active=[got[i] for i in range(64) if (mask>>(i%wave))&1]
            inactive=[got[i] for i in range(64) if not ((mask>>(i%wave))&1)]
            assert all(x==(sub if family=='buffer' else 0xdeadbeef) for x in inactive),(name,'inactive',got)
            if glc:
                expected=sorted([max(memory-sub*j,0) for j in range(len(active))])
                assert sorted(active)==expected,(name,'nonlinear returned history',got,expected)
            else: assert all(x==(sub if family=='buffer' else 0xdeadbeef) for x in active)
            return
        assert list(got)==expected,(name,got,expected)
    name=f'{family}-{mask_mode}-glc{int(glc)}-shared{int(shared)}-sub{sub:x}-mem{memory:x}-{output}-soff{soffset}-offset{offset}-scc{scc}'
    run(name,asm,wave,check)
if __name__=='__main__':
    # First bounded smoke checks; then edge, contention and preservation matrices.
    for family in ('global','buffer','global64'):
        for wave in (32,64):
            case(family,wave,'full',True,False,7,100,'memory')
            case(family,wave,'full',True,False,7,100,'old')
    for family in ('global','buffer','global64'):
        for wave in (32,64):
            for mode in ('full','partial','empty'):
                for glc in (False,True):
                    for output in ('memory','old','flags'):
                        case(family,wave,mode,glc,False,0xffffffff,0x80000000,output,96 if family=='buffer' else 128,scc=int(glc))
            for sub,mem in ((0,0),(0,0xffffffff),(1,0xffffffff),(0xffffffff,0xffffffff),(0x7fffffff,0x80000000)):
                case(family,wave,'full',True,False,sub,mem,'memory')
            for mode in ('full','partial'):
                for glc in (False,True):
                    for output in ('memory','old'):
                        case(family,wave,mode,glc,True,7,1000,output)
            if family!='buffer':
                case(family,wave,'full',True,False,7,100,'memory',offset=-28)
    print('PASS',len(results),'dispatches,',len(results)*64,'lane results')
