// eq_cloud_test.cpp - cloud-only checks of the Equihash(192,7) host code and of the
// CPU emulation of the GPU solver against libsodium and Kerrigan's own solutions.
//   eq_cloud_test <header140hex> [reference solution hex ...]
#include "../src/eq_host.h"
#include <sodium.h>
#include <stdio.h>
#include <string.h>
#include <chrono>
#include <set>
static std::vector<uint8_t> unhex(const std::string& h){std::vector<uint8_t> v(h.size()/2);for(size_t i=0;i<v.size();i++)v[i]=(uint8_t)strtol(h.substr(2*i,2).c_str(),0,16);return v;}
static std::string hex(const uint8_t*p,size_t n){static const char*d="0123456789abcdef";std::string s;for(size_t i=0;i<n;i++){s+=d[p[i]>>4];s+=d[p[i]&15];}return s;}
int main(int argc,char**argv){
  int fails=0;
  std::vector<uint8_t> hdr=unhex(argv[1]);
  if(hdr.size()!=140){printf("bad header\n");return 2;}
  // 1. BLAKE2b vs libsodium
  uint8_t pers[16]; memcpy(pers,"kerrigan",8); uint32_t n=192,k=7; memcpy(pers+8,&n,4); memcpy(pers+12,&k,4);
  EqMidstate ms=eq_midstate(hdr.data());
  for(uint32_t g: {0u,1u,12345u,(1u<<24)-1}){
    crypto_generichash_blake2b_state st; crypto_generichash_blake2b_init_salt_personal(&st,NULL,0,48,NULL,pers);
    crypto_generichash_blake2b_update(&st,hdr.data(),140); uint8_t le[4]={(uint8_t)g,(uint8_t)(g>>8),(uint8_t)(g>>16),(uint8_t)(g>>24)};
    crypto_generichash_blake2b_update(&st,le,4); uint8_t a[48]; crypto_generichash_blake2b_final(&st,a,48);
    uint8_t b[48]; eq_ref_hash(hdr.data(),g,b);
    uint64_t c[6]; eq_hash_index(ms,g,c); uint8_t cb[48]; for(int i=0;i<6;i++)for(int j=0;j<8;j++)cb[8*i+j]=(uint8_t)(c[i]>>(8*j));
    bool ok=!memcmp(a,b,48)&&!memcmp(a,cb,48); if(!ok)fails++;
    printf("blake2b g=%u %s\n",g,ok?"OK":"FAIL");
  }
  // 2. reference solutions
  std::set<std::string> refset;
  for(int i=2;i<argc;i++){
    std::vector<uint8_t> s=unhex(argv[i]); uint32_t idx[128]; eq_expand(s.data(),idx);
    uint8_t s2[400]; eq_compress(idx,s2); std::string why; bool v=eq_validate(hdr.data(),idx,&why);
    bool rt=!memcmp(s.data(),s2,400); if(!v||!rt)fails++;
    printf("reference solution %d: validate %s (%s), re-encode %s\n",i-1,v?"OK":"FAIL",why.c_str(),rt?"OK":"FAIL");
    refset.insert(argv[i]);
    idx[5]^=1; v=eq_validate(hdr.data(),idx,&why); printf("  corrupted: %s (%s)\n",v?"ACCEPTED (BAD)":"rejected OK",why.c_str()); if(v)fails++;
  }
  // 3. CPU emulation of the GPU solver
  auto t0=std::chrono::steady_clock::now();
  EqCpuStats st; auto cands=eq_cpu_solve(ms,&st);
  double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
  printf("emulation: %.1f s, level counts:",sec); for(int L=0;L<7;L++)printf(" %u(-%u)",st.level_counts[L],st.dropped[L]); printf(", %u candidates\n",st.ncands);
  std::set<std::string> found; int dup=0,bad=0;
  for(auto& c: cands){
    uint32_t idx[128]; memcpy(idx,c.data(),512);
    if(!eq_distinct(idx)){dup++;continue;}
    eq_canonical(idx); std::string why;
    if(!eq_validate(hdr.data(),idx,&why)){bad++;printf("  candidate invalid: %s\n",why.c_str());continue;}
    uint8_t s[400]; eq_compress(idx,s); found.insert(hex(s,400));
  }
  printf("emulation: %zu valid solutions, %d with repeated indices, %d invalid\n",found.size(),dup,bad);
  if(bad)fails++;
  int miss=0; for(auto&r:refset) if(!found.count(r)) miss++;
  int extra=0; for(auto&f:found) if(!refset.count(f)) extra++;
  printf("vs Kerrigan reference: %d missing, %d extra\n",miss,extra);
  for(auto&f:found) printf("SOL %s\n",f.c_str());
  printf("RESULT %s\n",fails?"FAIL":"OK");
  return fails?1:0;
}
