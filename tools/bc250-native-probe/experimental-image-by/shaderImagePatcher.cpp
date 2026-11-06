// SPDX-License-Identifier: GPL-2.0-only
// BY load rules checked against AnyPS5 06a90922740d075d3730c5d877a979e8c4d86776:
// RdnaImageOpDecoder, ImageInstructions::imageBy and SpirvImageEmitter::EmitByReadOp.
// 2026-11-06: native gfx1013 image loads and queries, no host texture translation.
#include "shaderImagePatcher.h"
#include "graphics/native_gpu/shaderScan.h"
#include <algorithm>
#include <array>
#include <climits>
#include <string_view>
namespace Libs::Graphics::NativeGpu {
namespace {
void Mov(std::vector<uint32_t>& c,uint32_t dst,uint32_t src,uint32_t literal=0) {
 c.push_back(0x7e000200u|(dst<<17)|src);if(src==255)c.push_back(literal);
}
void V3(std::vector<uint32_t>& c,uint32_t op,uint32_t dst,uint32_t a,uint32_t b=128,uint32_t d=128,uint32_t literal=0) {
 c.push_back(0xd4000000u|(op<<16)|dst);c.push_back(a|(b<<9)|(d<<18));
 if(a==255||b==255||d==255)c.push_back(literal);
}
bool Branch(std::vector<uint32_t>& c,uint32_t at,uint32_t target) {
 int64_t d=int64_t(target)-at-1;if(d<INT16_MIN||d>INT16_MAX)return false;
 c[at]=0xbf820000u|(uint32_t(d)&65535);return true;
}
bool LocalPc(std::span<const uint32_t> c,uint32_t dw) {
 if(dw+3>=c.size())return false;uint32_t r=(c[dw]>>16)&127;
 if(r>=104||(r&1))return false;
 uint32_t add=c[dw+1],carry=c[dw+3],base=0x80000000u|(r<<16);
 if(add!=(base|(r<<8)|255u)&&add!=(base|(255u<<8)|r))return false;
 int32_t offset=int32_t(c[dw+2]);uint32_t hi=r+1,adjust=offset<0?193u:128u;
 base=0x82000000u|(hi<<16);
 if(carry!=(base|(hi<<8)|adjust)&&carry!=(base|(adjust<<8)|hi))return false;
 int64_t target=(int64_t(dw)+1)*4+offset;return target>=0&&target<int64_t(c.size())*4;
}
struct Site {uint32_t dw,len,raw0,raw1,dst,elements,components;bool mip;std::array<uint32_t,3> addr;};
}
ShaderPatchResult PatchNativeImageInstructions(std::span<const uint32_t> code,uint32_t n) {
 ShaderPatchResult r;r.code.assign(code.begin(),code.end());
 ShaderScanOptions opt;opt.known_dwords=opt.max_dwords=uint32_t(code.size());
 const auto scan=ScanShader(code,opt);std::vector<Site> sites;bool safe=true;
 for(uint32_t dw=0;dw<code.size();++dw)if(scan.IsStart(dw)) {
  uint32_t w=code[dw];uint16_t op;auto enc=ScanDecodeOpcode(code.subspan(dw),&op);
  if((w>>23)==0x17d&&((w>>8)&255)==0x1f)safe &= LocalPc(code,dw);
  if(const char* name=Gfx1013Mnemonic(enc,op)) {
   std::string_view m(name);if(m.starts_with("v_movrel")||m.starts_with("v_swaprel"))safe=false;
  }
  if(enc!=ScanEnc::MIMG || (op!=0x42&&op!=0x43&&op!=0x4a&&op!=0x4b))continue;
  Site s{};s.dw=dw;s.len=InstructionDwords(code.subspan(dw));s.raw0=w;
  bool valid=s.len>=2&&n>0&&n<=242;
  if(s.len>=2) {
   s.raw1=code[dw+1];s.dst=(s.raw1>>8)&255;
   s.elements=(op&1)?4:2;s.mip=(op&8)!=0;
   uint32_t mask=(w>>8)&15,count=mask==15?4:mask==3?2:0;
   s.components=count/s.elements;
   uint32_t resource=((s.raw1>>16)&31)*4;
   valid &= ((w>>3)&7)==1 && !(w&0x0003c040u) &&
            !(s.raw1&0xfc000000u) && resource<=96 && s.components!=0 &&
            (mask==15 || (mask==3 && s.elements==2)) && s.dst+count<=n;
   s.addr[0]=s.raw1&255;
   for(unsigned i=1;i<2+unsigned(s.mip);++i)
    s.addr[i]=((w>>1)&3)?(code[dw+2]>>((i-1)*8))&255:s.addr[0]+i;
   for(unsigned i=0;i<2+unsigned(s.mip);++i)valid &= s.addr[i]<n;
  }
  if(valid)sites.push_back(s);else ++r.unsupported_sites;
 }
 if(sites.empty())return r;
 if(!safe||scan.decode_error||scan.truncated||scan.Count(ScanKind::BranchOutOfRange)||
    scan.Count(ScanKind::OverlapDecode)||scan.Count(ScanKind::IndirectBranch)||scan.Count(ScanKind::PcRelOutside)) {
  r.decode_error=true;r.unsupported_sites+=uint32_t(sites.size());return r;
 }
 if(r.unsupported_sites)return r;
 bool ok=true;
 for(const auto& s:sites) {
  uint32_t stub=uint32_t(r.code.size());ok &= Branch(r.code,s.dw,stub);
  uint32_t resource=((s.raw1>>16)&31)*4,sg=96;
  while(sg<resource+8 && sg+3>resource)sg-=8;
  const uint32_t x=n,y=n+1,lod=n+2,info=n+3,valid=n+7,tmp=n+8,aligned=n+5,out=n+9,spill=n+13;
  r.code.insert(r.code.end(),{0xbf8c0070u,0xbbfd0000u,0xbfa30000u});
  for(uint32_t i=0;i<3;++i)V3(r.code,0x361,spill,sg+i,128+i,128);
  r.code.push_back(0xbfa30000u);
  r.code.push_back(0xbe80046au|(sg<<16));r.code.push_back(0x85008081u|((sg+2)<<16));
  Mov(r.code,x,256+s.addr[0]);Mov(r.code,y,256+s.addr[1]);Mov(r.code,lod,s.mip?256+s.addr[2]:128);
  auto image=[&](uint32_t op,uint32_t mask,uint32_t dst,uint32_t addr) {
   uint32_t flags=s.raw0&0x02002080u;
   r.code.push_back(0xf0000008u|(op<<18)|(mask<<8)|flags);
   r.code.push_back((s.raw1&0x001f0000u)|(dst<<8)|addr);
  };
  // Query level zero, then derive the selected level's clamped extents.
  Mov(r.code,tmp,128);image(0xe,15,info,tmp);r.code.push_back(0xbf8c3f70u);
  auto cmp=[&](uint32_t op,uint32_t a,uint32_t b) {V3(r.code,op,106,a,b);};
  auto select=[&](uint32_t dst,uint32_t no,uint32_t yes){V3(r.code,0x101,dst,no,yes,106);};
  cmp(0xc1,256+lod,256+info+3);select(valid,128,129);
  V3(r.code,0x116,info,256+lod,256+info);
  V3(r.code,0x116,info+1,256+lod,256+info+1);
  cmp(0xc2,256+info,128);select(info,256+info,129);
  cmp(0xc2,256+info+1,128);select(info+1,256+info+1,129);
  auto inside=[&](uint32_t op,uint32_t a,uint32_t b) {
   cmp(op,a,b);select(tmp,128,129);V3(r.code,0x11b,valid,256+valid,256+tmp);
  };
  inside(0xc1,256+x,256+info);
  V3(r.code,0x126,tmp,256+info,256+x);
  inside(0xc6,256+tmp,128+s.elements);
  inside(0xc1,256+y,256+info+1);
  V3(r.code,0x11b,aligned,256+x,255,128,~(s.elements-1u));
  cmp(0xc5,256+valid,128);
  select(y,128,256+y);select(lod,128,256+lod);
  for(uint32_t i=0;i<s.elements;++i) {
   V3(r.code,0x125,x,256+aligned,128+i);
   select(x,128,256+x);
   image(s.mip?1:0,(1u<<s.components)-1,out+i*s.components,x);
   r.code.push_back(0xbf8c3f70u);
  }
  for(uint32_t i=0;i<s.elements*s.components;++i)select(s.dst+i,128,256+out+i);
  r.code.push_back(0xbfa30000u);
  r.code.push_back(0xbeea0400u|sg);r.code.push_back(0xbf078000u|(sg+2));
  for(uint32_t i=0;i<3;++i)V3(r.code,0x360,sg+i,256+spill,128+i,128);
  r.code.push_back(0xbfa30000u);
  uint32_t back=uint32_t(r.code.size());r.code.push_back(0);ok &= Branch(r.code,back,s.dw+s.len);
 }
 if(!ok) {r.code.assign(code.begin(),code.end());r.unsupported_sites=uint32_t(sites.size());return r;}
 r.patched_sites=uint32_t(sites.size());r.scratch_vgprs=14;
 while(r.code.size()%16)r.code.push_back(0xbf9f0000);
 return r;
}
}
