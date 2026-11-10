// Contract-backed lock-free lease read path (RSU + delta ring + per-document versions).
//
// This is the MVCC step over hnsw_lease_delta_rsu.cpp: visibility is no longer
// a byte plus a seqlock retry, but a (grant_version, revoke_version) pair per
// document. A reader that pins version v can filter a candidate granted or
// revoked while it runs without retrying: visible(d,v) = grant[d] <= v < revoke[d]
// (revoke 0 = never). Only the latest grant/revoke pair is stored, so a document
// that is granted, revoked and granted again is visible at pins inside its last
// lifetime only; the full history vector is recorded but not consulted here.
//
// Design (answers the P0 concurrency objection in docs/145):
//   * the HNSW graph is IMMUTABLE while any reader can search it. Grants go to
//     a per-role delta ring and become visible through a byte flip; revokes flip
//     the byte only. The ring is folded into the graph only under quiescence
//     (version odd, readers == 0), so addPoint never overlaps searchKnn.
//   * the version word is a monotone commit counter, not a seqlock: the writer
//     bumps it once per commit and never makes it odd. A reader pins it, records
//     it with its result, and the harness scores each read against V(r, pin).
//     There is no post-search version re-check in this variant; the seqlock
//     protocol lives in hnsw_lease_seqlock.cpp.
//   * a bounded number of retries falls back to an exact scan over a copy of
//     the current live set (no pin claimed, so the read is not judged against
//     V(r,pin); it is still zero-leak because the copy only holds live docs).
//   * NOTE on the leak counters: the harness filters every arm before it scores,
//     so static_leaks/inc_leaks are 0 by construction in this build and are not
//     an independent measurement of index leakage.
//
// Run:
//   hnsw_lease_delta_mvcc base queries roles qroles n dim nq k K changes threads seed outdir [mfac]
// Build (native):  g++ -O3 -march=native -std=c++17 -I/tmp/hnswlib_fix ...
// Build (TSAN):    g++ -O1 -g -fsanitize=thread -std=c++17 -I/tmp/hnswlib_fix ...
// Build (ASAN):    ... -fsanitize=address ...
//
// NOTE on the header: /tmp/hnswlib_fix is hnswlib with the prefetch over-read
// patched (it reads *(datal+j+1) at j==size-1, one slot past a full link list;
// harmless natively, fatal under ASan/TSan). See docs/146.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <numeric>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hnswlib/hnswlib.h"

using Clock = std::chrono::high_resolution_clock;

#ifndef DELTA_CAP
#define DELTA_CAP 4096
#endif

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

#if defined(__AVX512F__)
static inline float l2_dist(const float* v, const float* q, int dim) {
    __m512 acc = _mm512_setzero_ps();
    int j = 0;
    for (; j + 16 <= dim; j += 16) {
        const __m512 a = _mm512_loadu_ps(v + j);
        const __m512 b = _mm512_loadu_ps(q + j);
        const __m512 d = _mm512_sub_ps(a, b);
        acc = _mm512_fmadd_ps(d, d, acc);
    }
    float s = _mm512_reduce_add_ps(acc);
    for (; j < dim; ++j) {
        const float t = v[j] - q[j];
        s += t * t;
    }
    return s;
}
#else
static inline float l2_dist(const float* v, const float* q, int dim) {
    float s = 0.0f;
    for (int j = 0; j < dim; ++j) {
        const float t = v[j] - q[j];
        s += t * t;
    }
    return s;
}
#endif

static std::vector<int> exact_topk(const float* base, int dim,
                                   const std::vector<int>& ids,
                                   const float* qv, int k) {
    std::vector<std::pair<float, int>> buf(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        buf[i] = {l2_dist(base + static_cast<size_t>(ids[i]) * dim, qv, dim), ids[i]};
    }
    const int kk = std::min<int>(k, buf.size());
    if (kk < static_cast<int>(buf.size())) {
        std::nth_element(buf.begin(), buf.begin() + kk, buf.end());
    }
    std::sort(buf.begin(), buf.begin() + kk);
    std::vector<int> out(kk);
    for (int i = 0; i < kk; ++i) out[i] = buf[i].second;
    return out;
}

static double pctl(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * v.size()));
    return v[i];
}

// vis[d]: 0 = never granted, 1 = live, 2 = revoked (byte tombstone).
// version: even = quiescent, odd = a change or a merge is in flight.
// ring: doc ids granted since the last merge, read only by readers that pinned
// the same even version (entries are atomic so a racing reader cannot tear).
struct RoleState {
    hnswlib::HierarchicalNSW<float>* h = nullptr;
    std::vector<std::atomic<uint32_t>> grant_v;   // 0 = built in, else odd commit version
    std::vector<std::atomic<uint32_t>> revoke_v;  // 0 = live, else odd commit version
    std::atomic<int> live{0};
    std::atomic<uint64_t> version{0};      // published commit counter (readers pin this)
    std::atomic<uint64_t> next_version{0}; // allocator; a version is published only after
                                           // its transition and delta entry are visible
    std::atomic<int> readers{0};
    std::atomic<bool> merging{false};      // graph merge in flight
    std::vector<std::atomic<int>> ring;      // delta ring, capacity DELTA_CAP
    std::atomic<int> ring_n{0};
    std::mutex exact_mu;                     // cur <-> exact-fallback copy
    std::unordered_set<int> cur;             // writer-only, except under exact_mu
    std::unordered_set<int> ever;            // writer-only
    std::unordered_map<int, std::vector<std::pair<uint64_t, uint8_t>>> hist;  // writer-only
};

static std::atomic<long> retries{0}, fallbacks{0};

int main(int argc, char** argv) {
    if (argc != 14 && argc != 15) {
        std::cerr << "usage: hnsw_lease_delta_rsu base queries roles qroles n dim nq k K changes"
                     " threads seed outdir [candidate_factor=15]\n";
        return 2;
    }
    const std::string base_path = argv[1], query_path = argv[2];
    const std::string roles_path = argv[3], qroles_path = argv[4];
    const int n = std::stoi(argv[5]), dim = std::stoi(argv[6]);
    int nq = std::stoi(argv[7]);
    const int k = std::stoi(argv[8]);
    const int K = std::stoi(argv[9]);
    const int changes = std::stoi(argv[10]);
    const int threads = std::stoi(argv[11]);
    const int seed = std::stoi(argv[12]);
    const std::string outdir = argv[13];
    const int mfac = (argc == 15) ? std::stoi(argv[14]) : 15;
    int change_us = 500;
    if (const char* e = std::getenv("CHANGE_US")) change_us = std::max(1, std::atoi(e));
    int writers = 1;  // WRITERS>1: same aggregate change rate, split over writer threads
    if (const char* e = std::getenv("WRITERS")) writers = std::max(1, std::atoi(e));

    auto base_b = read_file(base_path);
    auto q_b = read_file(query_path);
    auto roles_b = read_file(roles_path);
    auto qroles_b = read_file(qroles_path);
    const float* base = reinterpret_cast<const float*>(base_b.data());
    const float* queries = reinterpret_cast<const float*>(q_b.data());

    size_t off = 0;
    int32_t n_roles = 0;
    read_scalar(roles_b, off, n_roles);
    std::vector<std::vector<int>> ids(n_roles);
    for (int r = 0; r < n_roles; ++r) {
        int32_t rid, cnt;
        read_scalar(roles_b, off, rid);
        read_scalar(roles_b, off, cnt);
        if (rid < 0 || rid >= n_roles) { std::cerr << "bad role id\n"; return 2; }
        ids[rid].resize(cnt);
        if (cnt) std::memcpy(ids[rid].data(), roles_b.data() + off, cnt * sizeof(int32_t));
        off += static_cast<size_t>(cnt) * sizeof(int32_t);
    }
    off = 0;
    int32_t nq_file = 0;
    read_scalar(qroles_b, off, nq_file);
    if (nq > nq_file) nq = nq_file;
    std::vector<int> qroles(nq);
    std::memcpy(qroles.data(), qroles_b.data() + off, nq * sizeof(int32_t));

    std::vector<int> counts(n_roles, 0);
    for (int r : qroles) counts[r]++;
    std::vector<int> ranked(n_roles);
    std::iota(ranked.begin(), ranked.end(), 0);
    std::sort(ranked.begin(), ranked.end(), [&](int a, int b) { return counts[a] > counts[b]; });
    std::unordered_set<int> promoted(ranked.begin(), ranked.begin() + K);

    hnswlib::L2Space space(dim);
    std::unordered_map<int, RoleState*> states;
    auto t0 = Clock::now();
    for (int r : promoted) {
        auto* st = new RoleState();
        st->h = new hnswlib::HierarchicalNSW<float>(&space, ids[r].size() + changes + 10, 16, 100);
        st->grant_v = std::vector<std::atomic<uint32_t>>(n);
        st->revoke_v = std::vector<std::atomic<uint32_t>>(n);
        st->ring = std::vector<std::atomic<int>>(DELTA_CAP);
        st->ring_n.store(0);
        for (int d : ids[r]) {
            st->h->addPoint(base + static_cast<size_t>(d) * dim, d);
            st->grant_v[d].store(0, std::memory_order_relaxed);
            st->cur.insert(d);
            st->ever.insert(d);
        }
        st->live.store(static_cast<int>(st->cur.size()), std::memory_order_relaxed);
        states[r] = st;
    }
    const double build_s = std::chrono::duration<double>(Clock::now() - t0).count();

    // Fold the ring into the graph.  Called only by the writer, with the
    // version already odd and readers == 0: no searchKnn can be in flight.
    auto merge_ring = [&](RoleState* st) {
        const int cnt = st->ring_n.load(std::memory_order_acquire);
        for (int i = 0; i < cnt; ++i) {
            const int d = st->ring[i].load(std::memory_order_relaxed);
            if (st->revoke_v[d].load(std::memory_order_relaxed) == 0)
                st->h->addPoint(base + static_cast<size_t>(d) * dim, d);
        }
        st->ring_n.store(0, std::memory_order_release);
    };

    auto query_one = [&](int q, std::vector<int>& got, uint64_t& v0, uint64_t& v1,
                         uint64_t force_pin = UINT64_MAX) -> double {
        const int r = qroles[q];
        const float* qv = queries + static_cast<size_t>(q) * dim;
        auto t = Clock::now();
        got.clear();
        if (promoted.count(r)) {
            auto* st = states[r];
            bool accepted = false;
            for (int attempt = 0; attempt < 8 && !accepted; ++attempt) {
                if (st->merging.load(std::memory_order_acquire)) {
                    retries.fetch_add(1);
                    continue;   // graph merge in flight: the only fallback cause
                }
                const uint64_t pin = (force_pin != UINT64_MAX) ? force_pin
                                                              : st->version.load(std::memory_order_acquire);
                st->readers.fetch_add(1, std::memory_order_acq_rel);
                if (st->merging.load(std::memory_order_acquire)) {
                    st->readers.fetch_sub(1, std::memory_order_acq_rel);
                    retries.fetch_add(1);
                    continue;
                }
                std::vector<std::pair<float, int>> cand;
                const size_t want =
                    std::min<size_t>(static_cast<size_t>(k) * mfac,
                                     static_cast<size_t>(st->live.load(std::memory_order_relaxed)));
                auto res = st->h->searchKnn(qv, want);
                cand.reserve(res.size() + DELTA_CAP);
                while (!res.empty()) {
                    cand.push_back({res.top().first, static_cast<int>(res.top().second)});
                    res.pop();
                }
                const int dn = st->ring_n.load(std::memory_order_acquire);
                for (int i = 0; i < dn; ++i) {
                    const int d = st->ring[i].load(std::memory_order_relaxed);
                    const uint32_t g = st->grant_v[d].load(std::memory_order_relaxed);
                    if (g <= pin) {
                        const uint32_t rv = st->revoke_v[d].load(std::memory_order_relaxed);
                        if (rv == 0 || rv > pin)
                            cand.push_back({l2_dist(base + static_cast<size_t>(d) * dim, qv, dim), d});
                    }
                }
                st->readers.fetch_sub(1, std::memory_order_acq_rel);
                std::sort(cand.begin(), cand.end());
                for (const auto& cd : cand) {
                    const uint32_t g = st->grant_v[cd.second].load(std::memory_order_relaxed);
                    if (g > pin) continue;
                    const uint32_t rv = st->revoke_v[cd.second].load(std::memory_order_relaxed);
                    if (rv != 0 && rv <= pin) continue;
                    got.push_back(cd.second);
                    if (static_cast<int>(got.size()) >= k) break;
                }
                v0 = pin;
                v1 = pin;
                accepted = true;
            }
            if (!accepted) {   // only a merge window can exhaust the retries
                std::vector<int> cur_copy;
                {
                    std::lock_guard<std::mutex> lk(st->exact_mu);
                    cur_copy.assign(st->cur.begin(), st->cur.end());
                }
                got = exact_topk(base, dim, cur_copy, qv, k);
                v0 = v1 = 0;
                fallbacks.fetch_add(1);
            }
        } else {
            got = exact_topk(base, dim, ids[r], qv, k);
            v0 = v1 = 0;
        }
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };

    // ---- E0 scripted interleaving (EVENTS_FILE): apply ops one by one and
    // query after each, so the contract tests can replay T1-T8 on the real kernel.
    if (const char* ev = std::getenv("EVENTS_FILE")) {
        std::ifstream fin(ev);
        if (!fin) { std::cerr << "cannot open EVENTS_FILE\n"; return 2; }
        std::ofstream fout(outdir + "/events_out.csv");
        fout << "step,op,role,doc,pin,nret,returned\n";
        std::string line;
        int step = 0;
        std::unordered_map<int, uint64_t> pinned;
        const char* qp = std::getenv("Q_AT_PUBLISH");
        const bool q_at_publish = qp && std::atoi(qp) > 0;
        const int c3_step = qp ? std::atoi(qp) : -1;   // which step to probe (1-based)
        while (std::getline(fin, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            char op = '?';
            int r = -1, d = -1, q = 0;
            if (!(ss >> op >> r >> d)) continue;
            ss >> q;
            if (q < 0 || q >= nq) q = 0;
            auto it = states.find(r);
            if (it == states.end()) { std::cerr << "role " << r << " not promoted\n"; return 2; }
            auto* st = it->second;
            if (op == 'p') {                    // remember the current epoch for the role
                std::lock_guard<std::mutex> lk(st->exact_mu);
                pinned[r] = st->version.load(std::memory_order_acquire);
                fout << step << ",p," << r << ",0," << pinned[r] << ",0,\"\"\n";
                ++step;
                continue;
            }
            if (op == 'q') {                    // query at the remembered pin (old-pin read)
                std::vector<int> got;
                uint64_t v0 = 0, v1 = 0;
                const uint64_t fp = pinned.count(r) ? pinned[r] : UINT64_MAX;
                query_one(q, got, v0, v1, fp);
                fout << step << ",q," << r << ",0," << v0 << ',' << got.size() << ",\"";
                for (size_t i = 0; i < got.size(); ++i) fout << (i ? ";" : "") << got[i];
                fout << "\"\n";
                ++step;
                continue;
            }
            uint64_t pin;
            {
                std::lock_guard<std::mutex> lk(st->exact_mu);
                const uint64_t vcommit = st->next_version.fetch_add(1, std::memory_order_acq_rel) + 1;
                if (op == 'g') {
                    const int slot = st->ring_n.load(std::memory_order_relaxed);
                    st->grant_v[d].store(static_cast<uint32_t>(vcommit), std::memory_order_relaxed);
                    // A re-grant after a revoke re-opens the interval; the graph entry
                    // (if any) is reused, so only the versions change.
                    st->revoke_v[d].store(0, std::memory_order_relaxed);
                    st->cur.insert(d);
                    st->ever.insert(d);
                    if (slot < DELTA_CAP) {
                        st->ring[slot].store(d, std::memory_order_relaxed);
                        st->ring_n.store(slot + 1, std::memory_order_release);
                    }
                    st->live.fetch_add(1, std::memory_order_relaxed);
                } else {
                    st->revoke_v[d].store(static_cast<uint32_t>(vcommit), std::memory_order_relaxed);
                    st->cur.erase(d);
                    st->live.fetch_sub(1, std::memory_order_relaxed);
                }
                // C3 probe: a reader that pins the *old* epoch here must not see the
                // change (the new epoch is not published yet), and a reader that pins
                // after the store must see it.  Enabled with Q_AT_PUBLISH=1.
                if (q_at_publish && step + 1 == c3_step) {
                    std::vector<int> g_before;
                    uint64_t b0 = 0, b1 = 0;
                    query_one(q, g_before, b0, b1, st->version.load(std::memory_order_acquire));
                    fout << step << ",b," << r << ',' << d << ',' << b0 << ','
                         << g_before.size() << ",\"";
                    for (size_t i = 0; i < g_before.size(); ++i) fout << (i ? ";" : "") << g_before[i];
                    fout << "\"\n";
                }
                // Publish the epoch only now: a reader that pins it observes the
                // transition and the delta entry, never a half-committed state.
                st->version.store(vcommit, std::memory_order_release);
                pin = st->version.load(std::memory_order_acquire);
            }
            std::vector<int> got;
            uint64_t v0 = 0, v1 = 0;
            query_one(q, got, v0, v1);
            fout << step << ',' << op << ',' << r << ',' << d << ',' << pin << ','
                 << got.size() << ",\"";
            for (size_t i = 0; i < got.size(); ++i) fout << (i ? ";" : "") << got[i];
            fout << "\"\n";
            ++step;
        }
        std::cout << "EVENTS_DONE steps=" << step << "\n";
        return 0;
    }

    std::vector<uint64_t> vpins(nq, 0);
    std::unordered_map<int, std::unordered_set<int>> initial;
    for (int r : promoted) initial[r] = std::unordered_set<int>(ids[r].begin(), ids[r].end());

    auto run_phase = [&](bool with_writer, std::vector<std::vector<int>>& gots,
                         std::atomic<int>& straddled) {
        std::vector<double> lat(nq, 0.0);
        std::atomic<bool> go{false};
        std::atomic<int> leaks{0};
        std::vector<std::thread> readers;
        std::vector<std::thread> writer_pool;
        std::mutex merge_mu;
        if (with_writer) {
          for (int w = 0; w < writers; ++w) {
            writer_pool.emplace_back([&, w]() {
                while (!go.load()) {}
                std::mt19937 rng(seed + 7 + w);
                std::uniform_int_distribution<int> opd(0, 1), docd(0, n - 1);
                std::vector<int> hot(ranked.begin(), ranked.begin() + std::min<int>(K, 10));
                std::uniform_int_distribution<int> hotd(0, static_cast<int>(hot.size()) - 1);
                for (int c = w; c < changes; c += writers) {
                    const int r = hot[hotd(rng)];
                    auto* st = states[r];
                    const bool grant = opd(rng);
                    {
                    std::lock_guard<std::mutex> lk(st->exact_mu);
                    const uint64_t vcommit = st->next_version.fetch_add(1, std::memory_order_acq_rel) + 1;
                    if (!grant && !st->cur.empty()) {
                        std::uniform_int_distribution<int> pick(0, static_cast<int>(st->cur.size()) - 1);
                        auto it = st->cur.begin();
                        std::advance(it, pick(rng));
                        const int victim = *it;
                        st->cur.erase(it);
                        st->revoke_v[victim].store(static_cast<uint32_t>(vcommit), std::memory_order_relaxed);
                        st->hist[victim].emplace_back(vcommit, 2);
                        st->live.fetch_sub(1, std::memory_order_relaxed);
                    } else if (grant) {
                        for (int tries = 0; tries < 100; ++tries) {
                            const int d = docd(rng);
                            if (st->cur.count(d)) continue;   // already visible
                            const int slot = st->ring_n.load(std::memory_order_relaxed);
                            if (slot < DELTA_CAP) {
                                // Write the transition and the delta entry first; the epoch
                                // that readers pin is published below, after both are visible.
                                // A re-grant clears the revoke version, reopening the interval.
                                st->grant_v[d].store(static_cast<uint32_t>(vcommit), std::memory_order_relaxed);
                                st->revoke_v[d].store(0, std::memory_order_relaxed);
                                st->cur.insert(d);
                                st->ever.insert(d);
                                st->ring[slot].store(d, std::memory_order_relaxed);
                                st->ring_n.store(slot + 1, std::memory_order_release);
                                st->hist[d].emplace_back(vcommit, 1);
                                st->live.fetch_add(1, std::memory_order_relaxed);
                            }
                            break;
                        }
                    }
                    st->version.store(vcommit, std::memory_order_release);
                    }  // exact_mu: cur is consistent again
                    // Graph work, if any, is under quiescence; normal changes never touch the graph.
                    if (st->ring_n.load(std::memory_order_acquire) >= DELTA_CAP) {
                        std::lock_guard<std::mutex> mg(merge_mu);
                        if (st->ring_n.load(std::memory_order_acquire) >= DELTA_CAP) {
                            st->merging.store(true, std::memory_order_release);
                            while (st->readers.load(std::memory_order_acquire) != 0)
                                std::this_thread::yield();
                            merge_ring(st);
                            st->merging.store(false, std::memory_order_release);
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(change_us * writers));
                }
            });
          }
        }
        for (int t = 0; t < threads; ++t) {
            readers.emplace_back([&, t]() {
                while (!go.load()) {}
                const int lo = t * nq / threads;
                const int hi = (t + 1) * nq / threads;
                uint64_t v0, v1;
                for (int q = lo; q < hi; ++q) {
                    lat[q] = query_one(q, gots[q], v0, v1);
                    vpins[q] = v0;
                    if (v0 != v1) straddled.fetch_add(1);
                    (void)leaks;
                }
            });
        }
        go.store(true);
        auto tq0 = Clock::now();
        for (auto& th : readers) th.join();
        const double phase_s = std::chrono::duration<double>(Clock::now() - tq0).count();
        for (auto& w : writer_pool) w.join();
        return std::make_tuple(lat, leaks.load(), phase_s);
    };

    std::vector<std::vector<int>> gots_static(nq), gots_inc(nq);
    std::atomic<int> straddled_static{0}, straddled_inc{0};
    auto [lat_static, leaks_static, phase_static_s] = run_phase(false, gots_static, straddled_static);
    auto [lat_inc, leaks_inc, phase_inc_s] = run_phase(true, gots_inc, straddled_inc);


    // Axis split at each read's pin: a missing document that is visible and inside
    // the candidate budget is an ANN loss; one beyond the budget is SEL.  The
    // contract build materializes every granted document (graph or delta), so the
    // AUTH term is zero by construction here and is measured on the other designs
    // by the Python harness.
    long inc_auth = 0, inc_sel = 0, inc_ann = 0;
    for (int q = 0; q < nq; ++q) {
        const int r = qroles[q];
        if (!promoted.count(r)) continue;
        const uint64_t pin = vpins[q];
        if (pin == 0) continue;
        auto* st = states[r];
        std::vector<int> vis;
        vis.reserve(st->ever.size());
        for (int d : st->ever) {
            const uint32_t g = st->grant_v[d].load(std::memory_order_relaxed);
            const uint32_t rv = st->revoke_v[d].load(std::memory_order_relaxed);
            if (g <= pin && (rv == 0 || rv > pin)) vis.push_back(d);
        }
        const float* qv = queries + static_cast<size_t>(q) * dim;
        auto truth = exact_topk(base, dim, vis, qv, k);
        auto cand = exact_topk(base, dim, vis, qv, k * mfac);
        std::unordered_set<int> rset(gots_inc[q].begin(), gots_inc[q].end());
        std::unordered_set<int> cset(cand.begin(), cand.end());
        for (int d : truth) {
            if (rset.count(d)) continue;
            if (cset.count(d)) ++inc_ann; else ++inc_sel;
        }
    }

    // Missing is now scored against V(r,pin), not against the end-of-run visible set,
    // so the three axes sum to it by construction.
    const int missing = static_cast<int>(inc_auth + inc_sel + inc_ann);

    // Post-run ground truth: was every returned document visible at its read's pin?
    long violations = 0, returned = 0;
    for (int q = 0; q < nq; ++q) {
        const int r = qroles[q];
        if (!promoted.count(r) || vpins[q] == 0) continue;
        auto* st = states[r];
        for (int d : gots_inc[q]) {
            ++returned;
            const uint32_t g = st->grant_v[d].load(std::memory_order_relaxed);
            const uint32_t rv = st->revoke_v[d].load(std::memory_order_relaxed);
            const bool visible = (g <= vpins[q]) && (rv == 0 || rv > vpins[q]);
            if (!visible) ++violations;
        }
    }

    std::ofstream sm(outdir + "/summary.txt");
    sm << "mode=delta_mvcc"
       << " n=" << n << " dim=" << dim << " nq=" << nq << " k=" << k << " K=" << K
       << " mfac=" << mfac << " delta_cap=" << DELTA_CAP
       << " changes=" << changes << " threads=" << threads << " writers=" << writers
       << " build_s=" << build_s
       << " qps_static=" << (phase_static_s > 0 ? nq / phase_static_s : 0)
       << " qps_inc=" << (phase_inc_s > 0 ? nq / phase_inc_s : 0)
       << " static_p50=" << pctl(lat_static, 0.50) << " static_p95=" << pctl(lat_static, 0.95)
       << " static_p99=" << pctl(lat_static, 0.99)
       << " incr_p50=" << pctl(lat_inc, 0.50) << " incr_p95=" << pctl(lat_inc, 0.95)
       << " incr_p99=" << pctl(lat_inc, 0.99)
       << " static_leaks=" << leaks_static << " inc_leaks=" << leaks_inc
       << " straddled=" << straddled_inc.load()
       << " retries=" << retries.load() << " fallbacks=" << fallbacks.load()
       << " returned=" << returned << " pin_violations=" << violations
       << " inc_missing=" << missing << " inc_auth=" << inc_auth << " inc_sel=" << inc_sel << " inc_ann=" << inc_ann << "\n";
    std::cout << "build_s=" << build_s
              << " incr_p50=" << pctl(lat_inc, 0.50)
              << " incr_p95=" << pctl(lat_inc, 0.95)
              << " incr_p99=" << pctl(lat_inc, 0.99)
              << " inc_leaks=" << leaks_inc << " straddled=" << straddled_inc.load()
              << " retries=" << retries.load() << " fallbacks=" << fallbacks.load()
              << " pin_violations=" << violations
              << " inc_missing=" << missing << "\n";
    return 0;
}
