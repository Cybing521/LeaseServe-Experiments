#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <shared_mutex>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include "faiss/MultiTenantIndexIVFHierarchical.h"
using Clock=std::chrono::steady_clock;
static std::vector<char> rf(const std::string&p){std::ifstream f(p,std::ios::binary);if(!f){std::cerr<<"open "<<p<<"\n";std::exit(1);}f.seekg(0,std::ios::end);size_t s=f.tellg();f.seekg(0);std::vector<char>b(s);f.read(b.data(),s);return b;}
template<class T> static void rd(const std::vector<char>&b,size_t&o,T&v){std::memcpy(&v,b.data()+o,sizeof(T));o+=sizeof(T);}
static double pct(std::vector<double>&v,double p){if(v.empty())return 0;std::sort(v.begin(),v.end());return v[std::min(v.size()-1,(size_t)(p*v.size()))];}
int main(int argc,char**argv){
  if(argc<13){std::cerr<<"usage: base queries roles qroles n d nq k nlist nprobe se seed\n";return 2;}
  int n=std::stoi(argv[5]),d=std::stoi(argv[6]),nq=std::stoi(argv[7]),k=std::stoi(argv[8]);
  size_t nlist=std::stoul(argv[9]),nprobe=std::stoul(argv[10]),se=std::stoul(argv[11]); int seed=std::stoi(argv[12]);
  auto bb=rf(argv[1]),qb=rf(argv[2]),rb=rf(argv[3]),qr=rf(argv[4]);
  const float*base=(const float*)bb.data(); const float*queries=(const float*)qb.data();
  size_t o=0;int32_t nr=0;rd(rb,o,nr);std::vector<std::vector<int32_t>>ids(nr);
  for(int r=0;r<nr;r++){int32_t rid,c;rd(rb,o,rid);rd(rb,o,c);ids[rid].resize(c);if(c)std::memcpy(ids[rid].data(),rb.data()+o,c*4);o+=(size_t)c*4;}
  o=0;int32_t nqf=0;rd(qr,o,nqf);if(nq>nqf)nq=nqf;std::vector<int>qroles(nq);std::memcpy(qroles.data(),qr.data()+o,nq*4);
  faiss::MultiTenantIndexIVFHierarchical index(d,nlist,faiss::METRIC_L2,1000,0.01,128,20,128,nprobe,1.6f,0.4f,se,2,false);
  auto tb=Clock::now(); index.train(n,base,0); std::vector<int64_t>lab(n);std::iota(lab.begin(),lab.end(),0);index.add_vector_with_ids(n,base,lab.data());
  for(int r=0;r<nr;r++)for(int32_t doc:ids[r])index.grant_access(doc,r);
  std::cerr<<"build_s="<<std::chrono::duration<double>(Clock::now()-tb).count()<<"\n";
  int role=qroles[0]; std::vector<int> members=ids[role]; std::vector<char> live(n,0); for(int x:ids[role])live[x]=1;
  std::shared_mutex mu; std::vector<std::vector<int32_t>> pins(nq);
  // warmup
  for(int q=0;q<nq;q++){std::shared_lock<std::shared_mutex>lk(mu);std::vector<float>dist(k);std::vector<faiss::idx_t>got(k,-1);index.search(1,queries+(size_t)q*d,k,qroles[q],dist.data(),got.data());}
  std::mt19937 srng(seed+12345); std::unordered_set<int> ts; while((int)ts.size()<std::min(500,nq))ts.insert((int)(srng()%nq));
  std::atomic<int> ready{0}; std::atomic<bool>go{false}; Clock::time_point t_origin;
  std::vector<double> qms,e2e,ums; std::mutex mtx; std::atomic<long> miss{0},leak{0},chk{0}; long granted=0,revoked=0;
  std::vector<std::pair<double,double>> read_iv,upd_iv;
  std::thread writer([&]{ready++;while(!go){};std::mt19937 wr(seed);
    for(int c=0;c<50;c++){auto u0=Clock::now();bool g=(wr()%2)==0;int doc;
      {std::unique_lock<std::shared_mutex>lk(mu);
       if(g){doc=(int)(wr()%n); if(!live[doc]){live[doc]=1;members.push_back(doc);ids[role].push_back(doc);granted++;index.grant_access(doc,role);}}
       else{ if(members.empty()){continue;} size_t i=wr()%members.size();doc=members[i]; if(live[doc]){live[doc]=0;members[i]=members.back();members.pop_back();auto it=std::find(ids[role].begin(),ids[role].end(),(int32_t)doc);if(it!=ids[role].end())ids[role].erase(it);revoked++;index.revoke_access(doc,role);}}
      }
      auto u1=Clock::now();{std::lock_guard<std::mutex>g2(mtx);ums.push_back(std::chrono::duration<double,std::milli>(u1-u0).count());upd_iv.emplace_back(std::chrono::duration<double>(u0-t_origin).count(),std::chrono::duration<double>(u1-t_origin).count());}
      std::this_thread::sleep_for(std::chrono::milliseconds(30));}});
  std::thread reader([&]{ready++;while(!go){};
    for(int q=0;q<nq;q++){std::vector<int32_t> vis;std::vector<float> dist(k);std::vector<faiss::idx_t> got(k,-1);
      auto t0=Clock::now();{std::shared_lock<std::shared_mutex>lk(mu);vis=ids[qroles[q]];index.search(1,queries+(size_t)q*d,k,qroles[q],dist.data(),got.data());}auto t2=Clock::now();
      std::lock_guard<std::mutex>g2(mtx);qms.push_back(std::chrono::duration<double,std::milli>(t2-t0).count());read_iv.emplace_back(std::chrono::duration<double>(t0-t_origin).count(),std::chrono::duration<double>(t2-t_origin).count());
      if(ts.count(q)){std::vector<std::pair<float,int>> all;all.reserve(vis.size());for(int doc:vis){float s=0;const float*dv=base+(size_t)doc*d;for(int j=0;j<d;j++){float t=queries[(size_t)q*d+j]-dv[j];s+=t*t;}all.emplace_back(s,doc);}size_t kk=std::min<size_t>(k,all.size());std::partial_sort(all.begin(),all.begin()+kk,all.end());std::unordered_set<int>gs;for(int i=0;i<k;i++)if(got[i]>=0)gs.insert((int)got[i]);for(size_t i=0;i<kk;i++)if(!gs.count(all[i].second))miss++;chk+=kk;}}
    });
  while(ready.load()<2){} t_origin=Clock::now();go.store(true);reader.join();writer.join();
  long orr=0,oru=0;for(auto&r:read_iv)for(auto&u:upd_iv)if(r.first<=u.second&&u.first<=r.second){orr++;break;}
  for(auto&u:upd_iv)for(auto&r:read_iv)if(u.first<=r.second&&r.first<=u.second){oru++;break;}
  std::cout<<"curator_online n="<<n<<" nq="<<nq<<" k="<<k<<" ef="<<se<<" changes=50 threads=1 p50="<<pct(qms,0.5)<<" p95="<<pct(qms,0.95)<<" p99="<<pct(qms,0.99)<<" updates="<<upd_iv.size()<<" u_p99="<<pct(ums,0.99)<<" overlap_reads="<<orr<<" overlapping_updates="<<oru<<" granted="<<granted<<" revoked="<<revoked<<" missing="<<miss<<" truth_checked="<<chk<<" recall="<<(chk?1.0-(double)miss/chk:0)<<"\n";
  return 0;
}
