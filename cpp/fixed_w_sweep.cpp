// Fixed-W sweep: hold the materialized set W(L) fixed, vary the candidate budget m
// and the graph parameters (M, efConstruction), and report the axis split.
// Prop. 1 says auth is a function of (W, V(r,e)) alone, so it must not move here
// while ann and sel do.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>
#include "hnswlib/hnswlib.h"

static std::vector<char> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", p.c_str()); std::exit(1); }
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
template <class T> static void rs(const std::vector<char>& b, size_t& o, T& v) {
    std::memcpy(&v, b.data() + o, sizeof(T)); o += sizeof(T);
}
static float l2(const float* a, const float* b, int d) {
    float s = 0; for (int i = 0; i < d; ++i) { float t = a[i] - b[i]; s += t * t; } return s;
}

int main(int argc, char** argv) {
    if (argc < 12) {
        std::fprintf(stderr, "usage: fixed_w base queries roles qroles n dim nq k K hole changes seed outdir\n");
        return 2;
    }
    const std::string base_p = argv[1], q_p = argv[2], roles_p = argv[3], qr_p = argv[4];
    const int n = std::atoi(argv[5]), dim = std::atoi(argv[6]), nq = std::atoi(argv[7]);
    const int k = std::atoi(argv[8]), K = std::atoi(argv[9]), hole = std::atoi(argv[10]);
    const int changes = std::atoi(argv[11]), seed = std::atoi(argv[12]);
    const std::string outdir = argv[13];

    auto bb = read_file(base_p), qb = read_file(q_p);
    const float* base = reinterpret_cast<const float*>(bb.data());
    const float* Q = reinterpret_cast<const float*>(qb.data());
    auto rb = read_file(roles_p), qrb = read_file(qr_p);
    size_t o = 0; int32_t nr = 0; rs(rb, o, nr);
    std::vector<std::vector<int>> ids(nr);
    for (int r = 0; r < nr; ++r) { int32_t rid, c; rs(rb, o, rid); rs(rb, o, c); ids[rid].resize(c);
        if (c) std::memcpy(ids[rid].data(), rb.data() + o, c * sizeof(int32_t)); o += (size_t)c * sizeof(int32_t); }
    o = 0; int32_t nqf = 0; rs(qrb, o, nqf);
    std::vector<int> qr(nqf); std::memcpy(qr.data(), qrb.data() + o, nqf * sizeof(int32_t));
    const int nq_eff = std::min(nq, nqf);

    // One role, fixed snapshot. V(r,e) gains `changes` grants and loses a few revokes;
    // every `hole`-th grant is dropped from the log, so W(L) misses exactly those and
    // auth > 0. W(L) = V(r,a_L) + accepted log entries, and it never changes below.
    const int r = qr[0];
    std::vector<int> vis = ids[r];
    std::unordered_set<int> seen(vis.begin(), vis.end());
    std::vector<int> granted, dropped;
    std::mt19937 rng(seed);
    int gi = 0;
    for (int c = 0; c < changes; ++c) {
        int d = static_cast<int>(rng() % (unsigned)n);
        if (seen.count(d)) continue;
        seen.insert(d); vis.push_back(d); ++gi;
        if (hole > 0 && gi % hole == 0) dropped.push_back(d); else granted.push_back(d);
    }
    std::unordered_set<int> W(ids[r].begin(), ids[r].end());
    for (int d : granted) W.insert(d);

    // Queries: the first nq_eff real queries, then one per dropped grant, using the
    // dropped document itself as the query vector. That document is the exact top-1 of
    // V(r,e) but absent from W(L), so the hole is actually exercised (auth > 0).
    std::vector<std::vector<float>> QV;
    for (int q = 0; q < nq_eff; ++q) QV.emplace_back(Q + (size_t)q * dim, Q + (size_t)(q + 1) * dim);
    for (int d : dropped) QV.emplace_back(base + (size_t)d * dim, base + (size_t)(d + 1) * dim);
    const int nq_all = (int)QV.size();

    // Exact truth over V(r,e) for each query (fixed, independent of index).
    std::vector<std::vector<int>> truth(nq_all);
    for (int q = 0; q < nq_all; ++q) {
        std::vector<std::pair<float, int>> all; all.reserve(vis.size());
        for (int d : vis) all.emplace_back(l2(base + (size_t)d * dim, QV[q].data(), dim), d);
        std::partial_sort(all.begin(), all.begin() + std::min<size_t>(k, all.size()), all.end());
        for (size_t i = 0; i < std::min<size_t>(k, all.size()); ++i) truth[q].push_back(all[i].second);
    }

    std::printf("role=%d |V(r,e)|=%zu |W|=%zu dropped=%zu\n", r, vis.size(), W.size(), dropped.size());
    std::printf("%4s %4s %5s %7s %7s %6s %6s %6s\n", "M", "efC", "m", "missing", "auth", "ann", "sel", "p50ms");
    std::ofstream csv(outdir + "/fixed_w.csv");
    csv << "M,efC,m,missing,auth,ann,sel,p50\n";
    for (int M : {8, 16, 32}) {
        for (int efC : {50, 100, 200}) {
            hnswlib::L2Space space(dim);
            auto* g = new hnswlib::HierarchicalNSW<float>(&space, ids[r].size() + 16, M, efC);
            for (int d : ids[r]) g->addPoint(base + (size_t)d * dim, d);   // graph over V(r,a_L)
            for (int m : {50, 150, 500, 1500}) {
                long miss = 0, auth = 0, ann = 0, sel = 0;
                std::vector<double> lat;
                for (int q = 0; q < nq_all; ++q) {
                    auto t0 = std::chrono::steady_clock::now();
                    auto pq = g->searchKnn(QV[q].data(), (size_t)m);
                    std::vector<std::pair<float, int>> cand;
                    while (!pq.empty()) { cand.push_back({pq.top().first, (int)pq.top().second}); pq.pop(); }
                    std::sort(cand.begin(), cand.end());
                    // Merge graph and log candidates by distance, then filter: the log is
                    // exact and contributes candidates, it is not a fallback appendage.
                    for (int d : granted) cand.emplace_back(l2(base + (size_t)d * dim, QV[q].data(), dim), d);
                    std::sort(cand.begin(), cand.end());
                    std::unordered_set<int> gs;
                    std::unordered_set<int> vis_set(vis.begin(), vis.end());
                    for (auto& c : cand) {
                        if ((int)gs.size() >= k) break;
                        if (vis_set.count(c.second)) gs.insert(c.second);
                    }
                    auto t1 = std::chrono::steady_clock::now();
                    lat.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
                    for (int d : truth[q]) {
                        if (gs.count(d)) continue;
                        ++miss;
                        if (!W.count(d)) ++auth;          // W cannot supply it
                        else if (std::find(granted.begin(), granted.end(), d) != granted.end()) ++ann;
                        else ++sel;
                    }
                }
                std::sort(lat.begin(), lat.end());
                double p50 = lat.empty() ? 0 : lat[lat.size() / 2];
                std::printf("%4d %4d %5d %7ld %7ld %6ld %6ld %6.3f\n", M, efC, m, miss, auth, ann, sel, p50);
                csv << M << "," << efC << "," << m << "," << miss << "," << auth << ","
                    << ann << "," << sel << "," << p50 << "\n";
            }
            delete g;
        }
    }
    return 0;
}
