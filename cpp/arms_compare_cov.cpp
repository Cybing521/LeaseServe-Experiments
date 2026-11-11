// Four-arm, same-runtime comparison (round-2 checklist, E1 / R09).
//
// One binary, one corpus, one ACL, one query set, one change stream, four arms
// measured on the same query with the same pin:
//   exact  : scan V(r,pin)  (the exact top-k; missing = 0 by construction)
//   static : HNSW built at t0, never repaired, filtered by V(r,pin)
//   unv    : HNSW updated in place under a shared_mutex, filtered by V(r,pin)
//   lease  : immutable graph + delta ring + per-document versions (contract build)
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
    std::atomic<int> grants_since_build{0};  // coverage witness: grants not in the graph
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
    // MOVE=1: the stream reclassifies a document from one role to another at a
    // single commit (revoke at the source, grant at the destination).  Used for
    // the multi-tenant instance, where a change touches two policies at once.
    const char* move_env = std::getenv("MOVE");
    const int move_mode = move_env ? std::atoi(move_env) : 0;
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
    std::atomic<long> leaks[6], missing[6], auth[6], returned[6], fallbacks[6];
    for (int a = 0; a < 6; ++a) { leaks[a] = 0; missing[a] = 0; auth[a] = 0; returned[a] = 0; fallbacks[a] = 0; }
    std::vector<std::vector<double>> lat(6);
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
            if (move_mode) {
                int ia = (int)(rnd() % pr.size()), ib = (int)(rnd() % pr.size());
                if (ia == ib) ib = (ib + 1) % pr.size();
                Role* A = &st[pr[ia]];
                Role* B = &st[pr[ib]];
                int ra = pr[ia], rb = pr[ib];
                if (ra > rb) { std::swap(A, B); std::swap(ra, rb); }
                std::unique_lock<std::shared_mutex> la(A->u_mu, std::defer_lock);
                std::unique_lock<std::shared_mutex> lb(B->u_mu, std::defer_lock);
                std::lock(la, lb);
                int d = -1;
                for (int t = 0; t < 64 && !A->ever.empty(); ++t) {
                    auto it = A->ever.begin();
                    std::advance(it, rnd() % A->ever.size());
                    if (A->revoke_v[*it].load() == 0 && !B->ever.count(*it)) { d = *it; break; }
                }
                if (d < 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(change_us));
                    continue;
                }
                const uint64_t va = A->next_version.fetch_add(1) + 1;
                const uint64_t vb = B->next_version.fetch_add(1) + 1;
                A->revoke_v[d].store(static_cast<uint32_t>(va));
                A->u->markDelete(d);
                B->grant_v[d].store(static_cast<uint32_t>(vb));
                B->revoke_v[d].store(0);
                B->ever.insert(d);
                const int slot = B->ring_n.load();
                if (slot < 4096) { B->ring[slot].store(d); B->ring_n.store(slot + 1, std::memory_order_release); }
                B->grants_since_build.fetch_add(1, std::memory_order_release);
                B->u->addPoint(base + static_cast<size_t>(d) * dim, d);
                A->version.store(va, std::memory_order_release);
                B->version.store(vb, std::memory_order_release);
                if (burst > 0) {
                    if ((c + 1) % burst == 0) std::this_thread::sleep_for(std::chrono::microseconds(change_us * burst));
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(change_us));
                }
                continue;
            }
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
                    R.grants_since_build.fetch_add(1, std::memory_order_release);
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

            // arm 0: exact
            auto t0 = Clock::now();
            auto T = exact_topk(base, dim, S, qv, k);
            tl[0].push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());

            auto score = [&](int arm, const std::vector<int>& got, bool materialized_all, const std::unordered_set<int>* mat) {
                returned[arm] += got.size();
                std::unordered_set<int> gs(got.begin(), got.end());
                for (int d : got) if (!visible(R, d, pin)) leaks[arm]++;
                for (int d : T) {
                    if (gs.count(d)) continue;
                    missing[arm]++;
                    const bool is_mat = materialized_all || (mat && mat->count(d));
                    if (!is_mat) auth[arm]++;
                }
            };
            score(0, T, true, nullptr);

            // arm 1: static (graph built at t0, never repaired)
            {
                auto t1 = Clock::now();
                std::vector<int> got;
                auto pq = R.g->searchKnn(qv, k * mfac);
                std::vector<std::pair<float, int>> cand;
                while (!pq.empty()) { cand.push_back({pq.top().first, (int)pq.top().second}); pq.pop(); }
                std::sort(cand.begin(), cand.end());
                for (auto& c : cand) {
                    if (got.size() >= (size_t)k) break;
                    if (visible(R, c.second, pin) && R.init_set.count(c.second)) got.push_back(c.second);
                }
                tl[1].push_back(std::chrono::duration<double, std::milli>(Clock::now() - t1).count());
                score(1, got, false, &R.init_set);   // grants are not materialized in the static arm
            }
            // arm 2: unversioned in-place
            {
                std::shared_lock<std::shared_mutex> lk(R.u_mu);
                auto t2 = Clock::now();
                std::vector<int> got;
                auto pq = R.u->searchKnn(qv, k * mfac);
                std::vector<std::pair<float, int>> cand;
                while (!pq.empty()) { cand.push_back({pq.top().first, (int)pq.top().second}); pq.pop(); }
                std::sort(cand.begin(), cand.end());
                for (auto& c : cand) {
                    if (got.size() >= (size_t)k) break;
                    if (visible(R, c.second, pin)) got.push_back(c.second);
                }
                tl[2].push_back(std::chrono::duration<double, std::milli>(Clock::now() - t2).count());
                score(2, got, true, nullptr);    // it applies grants in place, so its losses are ann/sel
            }
            // arm 3: lease (graph + ring)
            {
                auto t3 = Clock::now();
                std::vector<int> got;
                std::unordered_set<int> seen;
                // union of graph candidates and delta-ring candidates, ranked by distance
                auto pq = R.g->searchKnn(qv, k * mfac);
                std::vector<std::pair<float, int>> cand;
                while (!pq.empty()) { cand.push_back({pq.top().first, (int)pq.top().second}); pq.pop(); }
                const int rn = R.ring_n.load(std::memory_order_acquire);
                for (int i = 0; i < rn; ++i) {
                    int d = R.ring[i].load();
                    if (visible(R, d, pin)) cand.push_back({l2_dist(base + (size_t)d * dim, qv, dim), d});
                }
                std::sort(cand.begin(), cand.end());
                for (auto& c : cand) {
                    if (got.size() >= (size_t)k) break;
                    if (visible(R, c.second, pin) && !seen.count(c.second)) { got.push_back(c.second); seen.insert(c.second); }
                }
                tl[3].push_back(std::chrono::duration<double, std::milli>(Clock::now() - t3).count());
                score(3, got, true, nullptr);
            }
            // arm 4: version pin + exact fallback, NO delta.  The graph was built at
            // role version 0, so any committed change to the role makes the lease
            // stale and the read falls back to the exact scan until a rebuild.
            {
                auto t4 = Clock::now();
                std::vector<int> got;
                if (R.version.load(std::memory_order_acquire) == 0) {
                    auto pq = R.g->searchKnn(qv, k * mfac);
                    std::vector<std::pair<float, int>> cand;
                    while (!pq.empty()) { cand.push_back({pq.top().first, (int)pq.top().second}); pq.pop(); }
                    std::sort(cand.begin(), cand.end());
                    for (auto& c : cand) {
                        if (got.size() >= (size_t)k) break;
                        if (visible(R, c.second, pin)) got.push_back(c.second);
                    }
                } else {
                    fallbacks[4]++;
                    got = exact_topk(base, dim, S, qv, k);   // stale lease: exact scan
                }
                tl[4].push_back(std::chrono::duration<double, std::milli>(Clock::now() - t4).count());
                score(4, got, true, nullptr);
            }
            // arm 5: coverage gate WITHOUT the delta ring.  A revoke needs no repair
            // (the pinned filter drops it); a grant does.  So the graph stays usable
            // while no grant has committed for the role, and the read falls back only
            // on the grant side.  Under a revoke-only stream this is the same fast
            // path as the lease arm; under a grant-only stream it is exact.
            {
                auto t5 = Clock::now();
                std::vector<int> got;
                if (R.grants_since_build.load(std::memory_order_acquire) == 0) {
                    auto pq = R.g->searchKnn(qv, k * mfac);
                    std::vector<std::pair<float, int>> cand;
                    while (!pq.empty()) { cand.push_back({pq.top().first, (int)pq.top().second}); pq.pop(); }
                    std::sort(cand.begin(), cand.end());
                    for (auto& c : cand) {
                        if (got.size() >= (size_t)k) break;
                        if (visible(R, c.second, pin)) got.push_back(c.second);
                    }
                } else {
                    fallbacks[5]++;
                    got = exact_topk(base, dim, S, qv, k);   // uncovered grant: exact scan
                }
                tl[5].push_back(std::chrono::duration<double, std::milli>(Clock::now() - t5).count());
                score(5, got, true, nullptr);
            }
        }
        } while (!done.load(std::memory_order_acquire));
    };

    std::vector<std::thread> readers;
    for (int t = 0; t < threads; ++t) readers.emplace_back([&, t]() {
        std::vector<double> tl[6];
        run(t, tl);
        std::lock_guard<std::mutex> lk(lat_mu);
        for (int a = 0; a < 6; ++a) lat[a].insert(lat[a].end(), tl[a].begin(), tl[a].end());
    });
    go.store(true);
    for (auto& t : readers) t.join();
    writer.join();

    std::ofstream sm(outdir + "/summary.txt");
    const char* names[6] = {"exact", "static", "unversioned", "lease", "pin_no_delta", "cov_no_delta"};
    sm << "mode=arms_compare n=" << n << " dim=" << dim << " nq=" << nq_eff << " k=" << k
       << " K=" << K << " changes=" << changes << " threads=" << threads << "\n";
    for (int a = 0; a < 6; ++a) {
        sm << names[a] << " reads=" << lat[a].size()
           << " p50=" << pctl(lat[a], 0.50) << " p95=" << pctl(lat[a], 0.95)
           << " p99=" << pctl(lat[a], 0.99)
           << " leaks=" << leaks[a] << " missing=" << missing[a] << " auth=" << auth[a]
           << " returned=" << returned[a] << " fallbacks=" << fallbacks[a] << "\n";
    }
    std::cout << "wrote " << outdir << "/summary.txt\n";
    return 0;
}
