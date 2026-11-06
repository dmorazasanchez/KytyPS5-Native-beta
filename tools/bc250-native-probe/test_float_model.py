#!/usr/bin/env python3
"""Independent exact integer oracle and CPU interpreter of emitted gfx1013 code."""
from pathlib import Path
import random,struct,subprocess,json
root=Path(__file__).resolve().parents[2]
out=root/'_Build/instruction-tests/float-model';out.mkdir(parents=True,exist_ok=True)
cli=root/'_Build/instruction-tests/patch-float-blob'
def u32(x):return x&0xffffffff
def f64(lo,hi):return struct.unpack('<d',struct.pack('<II',lo,hi))[0]
def bits64(x):return struct.unpack('<II',struct.pack('<d',x))
def exact(a,b,c):
    def half(x):
        exp=(x>>10)&31;mant=x&1023;neg=(x>>15)&1
        return x,exp,mant,neg,(mant if exp==0 else mant+1024),(-24 if exp==0 else exp-25)
    hs=[half(a&65535),half(b&65535),half(a>>16),half(b>>16)]
    for x,e,m,s,_,_ in hs:
        if e==31 and m:return (s<<31)|0x7fc00000|(m<<13)
    if c&0x7fffffff>0x7f800000:return c|0x400000
    sl=hs[0][3]^hs[1][3];sh=hs[2][3]^hs[3][3];sc=c>>31
    def product(i,j):
        x,y=hs[i],hs[j]
        invalid=(x[1]==31 and y[1]==0 and y[2]==0) or (y[1]==31 and x[1]==0 and x[2]==0)
        return invalid,x[1]==31 or y[1]==31
    badl,il=product(0,1);badh,ih=product(2,3);ic=(c&0x7fffffff)==0x7f800000
    if badl or badh or (il and ih and sl!=sh) or (il and ic and sl!=sc) or (ih and ic and sh!=sc):return 0xffc00000
    if il or ih or ic:return ((sl if il else sh if ih else sc)<<31)|0x7f800000
    total=0
    for i,j,sign in ((0,1,sl),(2,3,sh)):
        term=(hs[i][4]*hs[j][4])<<(hs[i][5]+hs[j][5]+149)
        total+=-term if sign else term
    exp=(c>>23)&255
    if exp:
        term=((c&0x7fffff)|0x800000)<<(exp-1)
        total+=-term if sc else term
    if not total:return (sl&sh&sc)<<31
    sign=0x80000000 if total<0 else 0;mag=abs(total);high=mag.bit_length()-1
    shift=max(high-23,0);sig=mag>>shift
    rest=mag&((1<<shift)-1)
    if shift and (rest>(1<<(shift-1)) or (rest==(1<<(shift-1)) and sig&1)):sig+=1
    if sig>=1<<24:sig>>=1;high+=1
    exp=high-22
    return sign|(0x7f800000 if exp>=255 else (exp<<23)|(sig&0x7fffff) if exp>0 else sig)
def execute(code,a,b,c,dst,active=True,mode=0):
    s=[0x24680000+i for i in range(128)];v=[0xdead0000+i for i in range(256)]
    v[0]=c;v[1]=a;v[2]=b;v[3]=c;s[1]=a
    saved=s.copy();savedmode=mode;scc=True;spill=[0]*64
    def read(x,lit=0):
        if x>=256:return v[x-256]
        if x<128:return s[x]
        if x<=192:return x-128
        if x<=208:return u32(192-x)
        if 240<=x<=248:return (0x3f000000,0xbf000000,0x3f800000,0xbf800000,0x40000000,0xc0000000,0x40800000,0xc0800000,0x3e22f983)[x-240]
        if x==255:return lit
        raise AssertionError(('source',x))
    pc=0
    for step in range(20000):
        w=code[pc];pc+=1
        if w==0xbf810000:
            assert mode==savedmode and s[:106]==saved[:106] and s[106:108]==saved[106:108] and scc
            return v[dst]
        if w>>16==0xbf82:
            delta=w&65535;pc+=delta-(65536 if delta&32768 else 0);continue
        if w in (0xbf8c0070,0xbbfd0000,0xbfa30000):continue
        if w&0xff800000==0xb9000000:s[(w>>16)&127]=mode;continue
        if w&0xff800000==0xb9800000:mode=s[(w>>16)&127];continue
        if w>>23==0x17d and (w>>8)&255==4:
            d=(w>>16)&127;x=w&255;s[d:d+2]=s[x:x+2];continue
        if w&0xff800000==0x85000000:
            s[(w>>16)&127]=read((w&255) if scc else (w>>8)&255);continue
        if w&0xff800000==0x87000000:
            d=(w>>16)&127;s[d]=read(w&255)&read((w>>8)&255);scc=s[d]!=0;continue
        if w&0xffffff00==0xbf078000:scc=read(w&255)!=0;continue
        if w>>25==0x3f:
            d=(w>>17)&255;op=(w>>9)&255;src=w&511;lit=code[pc] if src==255 else 0;pc+=int(src==255)
            if not active:continue
            if op==1:v[d]=read(src,lit)
            elif op==6:v[d]=struct.unpack('<I',struct.pack('<f',float(read(src,lit))))[0]
            elif op==16:
                val=struct.unpack('<f',struct.pack('<I',read(src,lit)))[0]
                v[d:d+2]=bits64(val)
            else:raise AssertionError(('VOP1',hex(w)))
            continue
        assert w>>26==0x35,('encoding',hex(w))
        x=code[pc];pc+=1;op=(w>>16)&1023;d=w&255
        aa=x&511;bb=(x>>9)&511;cc=(x>>18)&511
        lit=code[pc] if 255 in (aa,bb,cc) else 0;pc+=int(255 in (aa,bb,cc))
        if op==0x361:spill[read(bb)]=read(aa);continue
        if op==0x360:s[d]=spill[read(bb)];continue
        if not active:continue
        ar=read(aa,lit);br=read(bb,lit)
        if op in (0x164,0x165):
            av=f64(ar,v[aa-256+1]);bv=f64(br,v[bb-256+1])
            if x&(1<<29):av=-av
            if x&(1<<30):bv=-bv
            v[d:d+2]=bits64(av*bv if op==0x165 else av+bv);continue
        if op in (0xc1,0xc2,0xc4,0xc5):
            s[106]=int(ar<br if op==0xc1 else ar==br if op==0xc2 else ar>br if op==0xc4 else ar!=br);s[107]=0;continue
        if op==0x101:v[d]=br if s[106]&1 else ar
        elif op==0x11b:v[d]=ar&br
        elif op==0x11c:v[d]=ar|br
        elif op==0x11d:v[d]=ar^br
        elif op==0x125:v[d]=u32(ar+br)
        elif op==0x126:v[d]=u32(ar-br)
        elif op==0x11a:v[d]=u32(br<<(ar&31))
        elif op==0x116:v[d]=br>>(ar&31)
        else:raise AssertionError(('VOP3',op))
    raise AssertionError('instruction budget')
def patch(words,name='dot2'):
    original=out/(name+'.original.bin');patched=out/(name+'.patched.bin')
    original.write_bytes(struct.pack('<'+str(len(words))+'I',*words))
    r=subprocess.run([str(cli),str(original),str(patched),'16'],check=True,capture_output=True,text=True)
    print(r.stdout.strip(),flush=True)
    return struct.unpack('<'+str(patched.stat().st_size//4)+'I',patched.read_bytes())
if __name__=='__main__':
    code=patch([0xcc134000,0x18000000|257|(258<<9)|(259<<18),0xbf810000])
    r=random.Random(61234);cases=[]
    for _ in range(25000):cases.append((r.getrandbits(32),r.getrandbits(32),r.getrandbits(32)))
    specials=(0,0x8000,1,0x3ff,0x400,0x3c00,0xbc00,0x7bff,0xfbff,0x7c00,0xfc00,0x7e01,0x7c01)
    for al in specials:
        for ah in specials:
            for bl in specials:
                for c in (0,0x80000000,1,0x800000,0x3f800000,0x7f800000,0xff800000,0x7f800123):
                    cases.append((al|(ah<<16),bl|(0x3c00<<16),c))
    for i,(a,b,c) in enumerate(cases):
        got=execute(code,a,b,c,0,mode=(i%4)<<2)
        want=exact(a,b,c)
        assert got==want,(i,hex(a),hex(b),hex(c),hex(got),hex(want))
        if i and i%10000==0:print('CPU cases',i,flush=True)
    c2=patch([0x04000000|(2<<9)|257,0xbf810000],'dot2c')
    for a,b,c in cases[:2000]:
        assert execute(c2,a,b,c,0)==exact(a,b,c)
    print('PASS',len(cases),'exact floating cases',flush=True)
    (out/'results.json').write_text(json.dumps(dict(cases=len(cases),status='PASS')))
