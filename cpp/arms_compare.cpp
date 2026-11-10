// Five-arm, same-runtime comparison (round-2 checklist, E1 / R09).
//
// One binary, one corpus, one ACL, one query set, one change stream, five arms
// measured on the same query with the same pin, in an order rotated by query so
// that no arm always runs first:
//   exact  : scan V(r,pin)  (the exact top-k; missing = 0 by construction)
//   static : HNSW built at t0, never repaired, filtered by V(r,pin)
//   unv    : HNSW updated in place under a shared_mutex, filtered by V(r,pin)
//   lease  : graph + delta ring, both filtered by V(r,pin) (no certificate here)
//   pin    : version-pin predicate, no delta log (falls back to exact once stale)
//
// Every arm except `exact` is filtered against the same pinned visible set, so
// leakage is comparable; missing is decomposed into auth (the returned set
// cannot contain the document because it was never materialized for this arm)
// and ann/sel (it was materialized but not returned).
//
// usage: arms_compare base queries roles qroles n dim nq k K changes threads seed outdir [mfac]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hnswlib/hnswlib.h"

using Clock = std::chrono::high_resolution_clock;

static std::vector<char> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "cannot open " << path << "\n"; std::exit(1); }
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<char> buf(static_cast<size_t>(n));
    f.read(buf.data(), n);
    if (!f) { std::cerr << "short read " << path << "\n"; std::exit(1); }
    return buf;
}

template <class T>
static void read_scalar(const std::vector<char>& b, size_t& off, T& v) {
    std::memcpy(&v, b.data() + off, sizeof(T));
    off += sizeof(T);
}

static inline float l2_dist(const float* v, const float* q, int dim) {
    float s = 0.0f;
    for (int j = 0; j < dim; ++j) { const float t = v[j] - q[j]; s += t * t; }
    return s;
}

static std::vector<int> exact_topk(const float* base, int dim,
                                   const std::vector<int>& ids,
                                   const float* qv, int k) {
    std::vector<std::pair<float, int>> buf(ids.size());
    for (size_t i = 0; i < ids.size(); ++i)
        buf[i] = {l2_dist(base + static_cast<size_t>(ids[i]) * dim, qv, dim), ids[i]};
    const int kk = std::min<int>(k, buf.size());
    if (kk < static_cast<int>(buf.size())) std::nth_element(buf.begin(), buf.begin() + kk, buf.end());
    std::sort(buf.begin(), buf.begin() + kk);
    std::vector<int> out(kk);
    for (int i = 0; i < kk; ++i) out[i] = buf[i].second;
    return out;
}

static double pctl(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * v.size()))];
}

struct Role {
    std::vector<int> init;                      // documents present at t0
    hnswlib::HierarchicalNSW<float>* g = nullptr;   // immutable graph (static + lease)
    hnswlib::HierarchicalNSW<float>* u = nullptr;   // in-place index (unv)
    std::shared_mutex u_mu;                      // readers shared, writer exclusive
    // ACL state, shared by all arms for filtering
    std::vector<std::atomic<uint32_t>> grant_v, revoke_v;
    std::unordered_set<int> ever;               // writer-only: materialized doc ids
    std::atomic<uint64_t> version{0}, next_version{0};
    std::vector<std::atomic<int>> ring;
    std::atomic<int> ring_n{0};
    std::unordered_set<int> init_set;
};

int main(int argc, char** argv) {
    if (argc < 15) {
        std::cerr << "usage: arms_compare base queries roles qroles n dim nq k K changes threads seed outdir [mfac]\n";
        return 2;
    }
    const std::string base_path = argv[1], query_path = argv[2], roles_path = argv[3], qroles_path = argv[4];
    const int n = std::atoi(argv[5]), dim = std::atoi(argv[6]), nq = std::atoi(argv[7]);
    const int k = std::atoi(argv[8]), K = std::atoi(argv[9]), changes = std::atoi(argv[10]);
    const int threads = std::atoi(argv[11]), seed = std::atoi(argv[12]);
    const std::string outdir = argv[13];
    const char* env = std::getenv("CHANGE_US");
    const int change_us = env ? std::atoi(env) : 500;
    // stream shape: AGGR = grant share (%), BURST = changes per burst (0 = steady)
    const char* aggr_env = std::getenv("AGGR");
    const int aggr = aggr_env ? std::atoi(aggr_env) : 50;
    const char* burst_env = std::getenv("BURST");
    const int burst = burst_env ? std::atoi(burst_env) : 0;
    const int mfac = argc > 14 ? std::atoi(argv[14]) : 15;
    (void)argc;

    auto qb = read_file(query_path);
    auto qroles_b = read_file(qroles_path);
    size_t off = 0;
    int nq_file = 0;
    read_scalar(qroles_b, off, nq_file);
    std::vector<int> qroles(nq_file);
    std::memcpy(qroles.data(), qroles_b.data() + off, sizeof(int) * nq_file);
    const int nq_eff = std::min(nq, nq_file);

    std::vector<char> base_buf;
    {
        std::ifstream f(base_path, std::ios::binary);
        if (!f) { std::cerr << "cannot open " << base_path << "\n"; return 1; }
        base_buf.resize(static_cast<size_t>(n) * dim * sizeof(float));
        f.read(base_buf.data(), base_buf.size());
        if (!f) { std::cerr << "short read " << base_path << "\n"; return 1; }
    }
    const float* base = reinterpret_cast<const float*>(base_buf.data());

    // roles: <nrole> then (rid, cnt, ids)
    std::vector<std::vector<int>> roles;
    {
        auto rb = read_file(roles_path);
        size_t o = 0;
        int nr = 0;
        read_scalar(rb, o, nr);
        roles.resize(nr);
        for (int i = 0; i < nr; ++i) {
            int rid = 0, cnt = 0;
            read_scalar(rb, o, rid); read_scalar(rb, o, cnt);
            roles[rid].resize(cnt);
            std::memcpy(roles[rid].data(), rb.data() + o, sizeof(int) * cnt);
            o += sizeof(int) * cnt;
        }
    }

    std::vector<int> counts(roles.size(), 0);
    for (int q = 0; q < nq_eff; ++q) counts[qroles[q]]++;
    std::vector<int> ranked(roles.size());
    for (size_t r = 0; r < roles.size(); ++r) ranked[r] = static_cast<int>(r);
    std::sort(ranked.begin(), ranked.end(), [&](int a, int b) { return counts[a] > counts[b]; });
    std::unordered_set<int> promoted;
    for (int i = 0; i < K && i < static_cast<int>(ranked.size()); ++i) promoted.insert(ranked[i]);

    hnswlib::L2Space space(dim);
    std::unordered_map<int, Role> st;
    std::vector<int> docs;
    for (int r : promoted) {
        Role& R = st[r];
        R.init = roles[r];
        R.grant_v = std::vector<std::atomic<uint32_t>>(n);
        R.revoke_v = std::vector<std::atomic<uint32_t>>(n);
        R.ring = std::vector<std::atomic<int>>(4096);
        for (int d : R.init) { R.grant_v[d].store(0); R.revoke_v[d].store(0); R.ever.insert(d); R.init_set.insert(d); }
        R.g = new hnswlib::HierarchicalNSW<float>(&space, R.init.size() + changes + 16, 16, 100);
        R.u = new hnswlib::HierarchicalNSW<float>(&space, R.init.size() + changes + 16, 16, 100);
        for (int d : R.init) { R.g->addPoint(base + (size_t)d * dim, d); R.u->addPoint(base + (size_t)d * dim, d); }
    }

    std::atomic<bool> go{false}, done{false};
    std::atomic<long> leaks[5], pre_leaks[5], missing[5], auth[5], returned[5], fallbacks[5];
    for (int a = 0; a < 5; ++a) { leaks[a] = 0; pre_leaks[a] = 0; missing[a] = 0; auth[a] = 0; returned[a] = 0; fallbacks[a] = 0; }
    std::vector<std::vector<double>> lat(5);
    std::mutex lat_mu;

    auto visible = [&](Role& R, int d, uint64_t pin) {
        const uint32_t g = R.grant_v[d].load(std::memory_order_relaxed);
        const uint32_t rv = R.revoke_v[d].load(std::memory_order_relaxed);
        // grant_v == 0 means "present at build time" (visible from version 0)
        return g <= pin && (rv == 0 || rv > pin);
    };

    // writer: 200 changes over the promoted roles, publishing after the data
    std::thread writer([&]() {
        while (!go.load()) std::this_thread::yield();
        std::vector<int> pr(promoted.begin(), promoted.end());
        std::sort(pr.begin(), pr.end());
        unsigned s = seed * 2654435761u + 1;
        auto rnd = [&]() { s = s * 1664525u + 1013904223u; return s; };
        std::vector<int> cur_docs;
        for (int c = 0; c < changes; ++c) {
            const int r = pr[rnd() % pr.size()];
            Role& R = st[r];
            const bool grant = (int)(rnd() % 100) < aggr;
            {
                std::unique_lock<std::shared_mutex> lk(R.u_mu);
                const uint64_t vcommit = R.next_version.fetch_add(1) + 1;
                if (grant) {
                    int d = 0;
                    for (int t = 0; t < 200; ++t) { d = (int)(rnd() % n); if (!R.ever.count(d)) break; }
                    if (R.ever.count(d)) { std::this_thread::sleep_for(std::chrono::microseconds(change_us)); continue; }
                    R.grant_v[d].store((uint32_t)vcommit);
                    R.revoke_v[d].store(0);
                    R.ever.insert(d);
                    const int slot = R.ring_n.load();
                    if (slot < 4096) { R.ring[slot].store(d); R.ring_n.store(slot + 1, std::memory_order_release); }
                    R.u->addPoint(base + (size_t)d * dim, d);
                } else {
                    if (R.ever.empty()) { std::this_thread::sleep_for(std::chrono::microseconds(change_us)); continue; }
                    auto it = R.ever.begin();
                    std::advance(it, rnd() % R.ever.size());
                    const int d = *it;
                    if (R.revoke_v[d].load() != 0) { std::this_thread::sleep_for(std::chrono::microseconds(change_us)); continue; }
                    R.revoke_v[d].store((uint32_t)vcommit);
                    R.u->markDelete(d);
                }
                R.version.store(vcommit, std::memory_order_release);
            }
            if (burst > 0) {
                if ((c + 1) % burst == 0) std::this_thread::sleep_for(std::chrono::microseconds(change_us * burst));
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(change_us));
            }
        }
        done.store(true, std::memory_order_release);
    });

    auto run = [&](int tid, std::vector<double>* tl) {
        while (!go.load()) std::this_thread::yield();
        // keep querying for the whole change window, so reads overlap the stream
        do {
        for (int q = tid; q < nq_eff; q += threads) {
            const int r = qroles[q];
            if (!promoted.count(r)) continue;
            Role& R = st[r];
            const float* qv = nullptr;
            if ((size_t)(q + 1) * dim * sizeof(float) <= qb.size()) qv = reinterpret_cast<const float*>(qb.data()) + (size_t)q * dim;
            if (!qv) continue;
            const uint64_t pin = R.version.load(std::memory_order_acquire);
            std::vector<int> S;
            {
                std::shared_lock<std::shared_mutex> lk(R.u_mu);
                S.reserve(R.ever.size());
                for (int d : R.ever) if (visible(R, d, pin)) S.push_back(d);
            }

            // Rotated arm order: the arm that runs first inside a query changes
            // with q, so no arm always pays the cold-cache penalty.  Each arm is
            // timed on its own; scoring runs after every arm has produced a
            // result, so the order cannot leak into the counters either.
            std::vector<int> got[5], raw[5];
            auto to_ms = [](Clock::time_point a) {
                return std::chrono::duration<double, std::milli>(Clock::now() - a).count();
            };
            auto accept = [&](int arm, int d) {
                if (!visible(R, d, pin)) return false;
                if (arm == 1 && !R.init_set.count(d)) return false;   // static: grants not materialized
                return true;
            };
            // raw = the arm's own top-k before filtering; got = what it returns.
            auto take = [&](int arm, std::vector<std::pair<float, int>>& cand) {
                std::sort(cand.begin(), cand.end());
                std::unordered_set<int> seen_raw, seen_got;
                for (auto& c : cand) {
                    if (static_cast<int>(raw[arm].size()) >= k) break;
                    if (seen_raw.insert(c.second).second) raw[arm].push_back(c.second);
                }
                for (auto& c : cand) {
                    if (static_cast<int>(got[arm].size()) >= k) break;
                    if (!seen_got.insert(c.second).second) continue;
                    if (accept(arm, c.second)) got[arm].push_back(c.second);
                }
            };
            auto graph_cand = [&](hnswlib::HierarchicalNSW<float>* g) {
                std::vector<std::pair<float, int>> cand;
                auto pq = g->searchKnn(qv, k * mfac);
                cand.reserve(pq.size());
                while (!pq.empty()) { cand.push_back({pq.top().first, (int)pq.top().second}); pq.pop(); }
                return cand;
            };

            for (int slot = 0; slot < 5; ++slot) {
                const int arm = (q + slot) % 5;
                if (arm == 0) {                                    // exact over the pinned set
                    auto t0 = Clock::now();
                    raw[0] = exact_topk(base, dim, S, qv, k);
                    got[0] = raw[0];
                    tl[0].push_back(to_ms(t0));
                } else if (arm == 1 || arm == 3) {                 // static graph / lease graph+ring
                    auto t1 = Clock::now();
                    auto cand = graph_cand(R.g);
                    if (arm == 3) {
                        const int rn = R.ring_n.load(std::memory_order_acquire);
                        for (int i = 0; i < rn; ++i) {
                            int d = R.ring[i].load();
                            if (visible(R, d, pin))
                                cand.push_back({l2_dist(base + (size_t)d * dim, qv, dim), d});
                        }
                    }
                    take(arm, cand);
                    tl[arm].push_back(to_ms(t1));
                } else if (arm == 2) {                             // unversioned, in place
                    std::shared_lock<std::shared_mutex> lk(R.u_mu);
                    auto t2 = Clock::now();
                    auto cand = graph_cand(R.u);
                    take(2, cand);
                    tl[2].push_back(to_ms(t2));
                } else {                                           // arm 4: version pin, no delta
                    auto t4 = Clock::now();
                    if (R.version.load(std::memory_order_acquire) == 0) {
                        auto cand = graph_cand(R.g);
                        take(4, cand);
                    } else {
                        fallbacks[4]++;
                        got[4] = exact_topk(base, dim, S, qv, k);
                        raw[4] = got[4];
                    }
                    tl[4].push_back(to_ms(t4));
                }
            }

            // truth is the exact arm; score every arm against it.
            const std::vector<int>& T = raw[0];
            for (int a = 0; a < 5; ++a) {
                returned[a] += got[a].size();
                // leaks: unauthorized documents the arm would return unfiltered.
                for (int d : raw[a]) if (!visible(R, d, pin)) pre_leaks[a]++;
                // leaks@response: must stay 0 because every arm filters.
                std::unordered_set<int> gs(got[a].begin(), got[a].end());
                for (int d : got[a]) if (!visible(R, d, pin)) leaks[a]++;
                for (int d : T) {
                    if (gs.count(d)) continue;
                    missing[a]++;
                    const bool is_mat = (a == 1) ? (bool)R.init_set.count(d) : true;
                    if (!is_mat) auth[a]++;
                }
            }
        }
        } while (!done.load(std::memory_order_acquire));
    };

    std::vector<std::thread> readers;
    for (int t = 0; t < threads; ++t) readers.emplace_back([&, t]() {
        std::vector<double> tl[5];
        run(t, tl);
        std::lock_guard<std::mutex> lk(lat_mu);
        for (int a = 0; a < 5; ++a) lat[a].insert(lat[a].end(), tl[a].begin(), tl[a].end());
    });
    go.store(true);
    for (auto& t : readers) t.join();
    writer.join();

    std::ofstream sm(outdir + "/summary.txt");
    const char* names[5] = {"exact", "static", "unversioned", "lease", "pin_no_delta"};
    sm << "mode=arms_compare n=" << n << " dim=" << dim << " nq=" << nq_eff << " k=" << k
       << " K=" << K << " changes=" << changes << " threads=" << threads << "\n";
    for (int a = 0; a < 5; ++a) {
        sm << names[a] << " reads=" << lat[a].size()
           << " p50=" << pctl(lat[a], 0.50) << " p95=" << pctl(lat[a], 0.95)
           << " p99=" << pctl(lat[a], 0.99)
           << " leaks=" << leaks[a] << " pre_leaks=" << pre_leaks[a] << " missing=" << missing[a] << " auth=" << auth[a]
           << " returned=" << returned[a] << " fallbacks=" << fallbacks[a] << "\n";
    }
    std::cout << "wrote " << outdir << "/summary.txt\n";
    return 0;
}
