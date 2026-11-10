#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <shared_mutex>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "faiss/MultiTenantIndexIVFHierarchical.h"
using Clock=std::chrono::high_resolution_clock;
static std::vector<char> rf(const std::string&p){std::ifstream f(p,std::ios::binary);if(!f){std::cerr<<"open "<<p<<"\n";std::exit(1);}f.seekg(0,std::ios::end);size_t s=f.tellg();f.seekg(0);std::vector<char>b(s);f.read(b.data(),s);return b;}
template<class T> static void rs(const std::vector<char>&b,size_t&o,T&v){std::memcpy(&v,b.data()+o,sizeof(T));o+=sizeof(T);}
static double pct(std::vector<double>&v,double p){if(v.empty())return 0;std::sort(v.begin(),v.end());return v[std::min(v.size()-1,(size_t)(p*v.size()))];}
struct QRes{double p50,p95,recall;};
int main(int argc,char**argv){
  if(argc<10){std::cerr<<"usage: base queries roles qroles n d nq k nlist nprobe [niter]\n";return 2;}
  int n=std::stoi(argv[5]),d=std::stoi(argv[6]),nq=std::stoi(argv[7]),k=std::stoi(argv[8]);
  size_t nlist=std::stoul(argv[9]),nprobe=std::stoul(argv[10]);
  int niter=(argc>11)?std::stoi(argv[11]):20;
  auto bb=rf(argv[1]),qb=rf(argv[2]),rb=rf(argv[3]),qr=rf(argv[4]);
  const float*base=(const float*)bb.data();const float*queries=(const float*)qb.data();
  size_t o=0;int32_t nr=0;rs(rb,o,nr);std::vector<std::vector<int32_t>>ids(nr);
  for(int r=0;r<nr;r++){int32_t rid,cnt;rs(rb,o,rid);rs(rb,o,cnt);ids[rid].resize(cnt);if(cnt)std::memcpy(ids[rid].data(),rb.data()+o,cnt*4);o+=(size_t)cnt*4;}
  o=0;int32_t nqf=0;rs(qr,o,nqf);if(nq>nqf)nq=nqf;std::vector<int32_t>qroles(nq);std::memcpy(qroles.data(),qr.data()+o,nq*4);
  faiss::MultiTenantIndexIVFHierarchical index(d,nlist,faiss::METRIC_L2,1000,0.01,128,niter,128,nprobe,1.6f,0.4f,800,2,false);
  auto tb=Clock::now();index.train(n,base,0);std::vector<int64_t>labels(n);std::iota(labels.begin(),labels.end(),0);index.add_vector_with_ids(n,base,labels.data());
  for(int r=0;r<nr;r++)for(int32_t doc:ids[r])index.grant_access(doc,r);
  double build=std::chrono::duration<double>(Clock::now()-tb).count();
  std::cout<<"build_s="<<build<<"\n";
  int role=0;std::vector<int32_t>pool(ids[role].begin(),ids[role].begin()+std::min<size_t>(500,ids[role].size()));std::vector<char>on(pool.size(),1);
  auto eval=[&](int se,int nq2){index.search_ef=(size_t)se;std::vector<double>ms;long hits=0,total=0;
    for(int q=0;q<nq2;q++){const float*qv=queries+(size_t)q*d;const auto&vis=ids[qroles[q]];if(vis.empty())continue;
      std::vector<std::pair<float,int32_t>>truth;truth.reserve(vis.size());for(int32_t doc:vis){float s=0;const float*dv=base+(size_t)doc*d;for(int j=0;j<d;j++){float t=qv[j]-dv[j];s+=t*t;}truth.emplace_back(s,doc);}
      size_t kk=std::min<size_t>(k,truth.size());std::partial_sort(truth.begin(),truth.begin()+kk,truth.end());
      std::vector<float>dist(k,0.f);std::vector<faiss::idx_t>got(k,-1);auto a=Clock::now();index.search(1,qv,k,qroles[q],dist.data(),got.data());auto b=Clock::now();ms.push_back(std::chrono::duration<double,std::milli>(b-a).count());
      for(size_t i=0;i<kk;i++){total++;for(int j=0;j<k;j++)if(got[j]==truth[i].second){hits++;break;}}}
    return QRes{pct(ms,0.5),pct(ms,0.95),total?(double)hits/total:0};};
  int Ns[4]={0,100,1000,10000};int done=0;
  for(int ni=0;ni<4;ni++){int target=Ns[ni];std::vector<double>upd;int err=0;
    while(done<target){int idx=done%pool.size();int32_t doc=pool[idx];auto a=Clock::now();
      try{if(on[idx])index.revoke_access(doc,role);else index.grant_access(doc,role);}catch(...){err++;}
      upd.push_back(std::chrono::duration<double,std::milli>(Clock::now()-a).count());
      auto&v=ids[role];auto it=std::find(v.begin(),v.end(),doc);if(on[idx]){if(it!=v.end())v.erase(it);on[idx]=0;}else{if(it==v.end())v.push_back(doc);on[idx]=1;}
      done++;}
    auto r8=eval(800,nq),r32=eval(3200,nq);
    std::cout<<"N="<<target<<" upd_p50="<<pct(upd,0.5)<<" upd_p95="<<pct(upd,0.95)<<" err="<<err
             <<" se800="<<r8.p50<<"/"<<r8.p95<<"/"<<r8.recall<<" se3200="<<r32.p50<<"/"<<r32.p95<<"/"<<r32.recall<<"\n";}
  // concurrent: batch 10 under one lock, 100 changes
  std::shared_mutex mu;std::atomic<int>ready{0};std::atomic<bool>go{false};std::vector<double>qms,wms;std::mutex qmu;int werr=0;
  auto reader=[&](int tid){ready++;while(!go){};std::vector<double>local;for(int q=tid;q<nq;q+=4){const float*qv=queries+(size_t)q*d;std::vector<float>dist(k);std::vector<faiss::idx_t>got(k,-1);std::shared_lock<std::shared_mutex>lk(mu);auto a=Clock::now();index.search(1,qv,k,0,dist.data(),got.data());auto b=Clock::now();local.push_back(std::chrono::duration<double,std::milli>(b-a).count());}std::lock_guard<std::mutex>g(qmu);qms.insert(qms.end(),local.begin(),local.end());};
  auto writer=[&]{ready++;while(!go){};for(int b=0;b<10;b++){auto a=Clock::now();try{std::unique_lock<std::shared_mutex>lk(mu);for(int j=0;j<10;j++){int idx=(b*10+j)%pool.size();int32_t doc=pool[idx];if(on[idx])index.revoke_access(doc,role);else index.grant_access(doc,role);on[idx]=!on[idx];}}catch(...){werr++;}wms.push_back(std::chrono::duration<double,std::milli>(Clock::now()-a).count());std::this_thread::sleep_for(std::chrono::microseconds(500));}};
  std::vector<std::thread>th;for(int i=0;i<4;i++)th.emplace_back(reader,i);th.emplace_back(writer);while(ready<5){}go=true;for(auto&t:th)t.join();
  std::cout<<"concurrent q_p50="<<pct(qms,0.5)<<" q_p95="<<pct(qms,0.95)<<" q_p99="<<pct(qms,0.99)<<" updates="<<wms.size()<<" u_p50="<<pct(wms,0.5)<<" u_p95="<<pct(wms,0.95)<<" u_p99="<<pct(wms,0.99)<<" err="<<werr<<"\n";
  return 0;
}
