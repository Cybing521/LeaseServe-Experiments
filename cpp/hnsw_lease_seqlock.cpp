// Seqlock variant of hnsw_lease_epoch.cpp: the version counter becomes a
// sequence lock. The writer bumps to an *odd* value before touching the
// visibility bytes or the graph and bumps to an *even* value after, so a reader
// that observes the same even version at the start and the end of its query
// knows that no change committed in between and that every byte it read belongs
// to that version. Readers retry on a version change and fall back to the exact
// scan after a bounded number of attempts. The harness additionally records the
// per-document change history so the returned set can be checked, after the
// run, against V(r,v0) -- the property Lemma 1 states.
//
// Lock-free read-path variant of hnsw_lease_concurrent.cpp. The baseline takes
// a per-role std::shared_mutex around searchKnn + ACL filter (readers shared,
// writer exclusive); the writer tax on p95 is the reader waiting for the
// writer's exclusive lock. Here the reader never blocks:
//   - visibility is a byte per document (0 never, 1 live, 2 deleted) that the
//     single writer updates in place and readers load with relaxed ordering;
//   - searchKnn runs without any lock. hnswlib's query path reads link lists
//     unlocked; on x86 the writer's aligned stores make a torn read yield
//     either the old or the new value, so the worst case is a slightly stale
//     graph, never an unauthorized document (the vis filter decides safety),
//     and capacity is pre-allocated so the index never reallocates.
// The writer holds no lock either; it is the only mutator of its role state.
// A per-role version counter counts reads that straddle a change.
//
// Build:
//   g++ -O3 -march=native -std=c++17 -I/tmp hnsw_lease_epoch.cpp -o hnsw_lease_epoch
// Run:
//   hnsw_lease_epoch base queries roles qroles n dim nq k K changes threads seed outdir [mfac]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <tuple>
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

static inline float l2_avx512(const float* v, const float* q, int dim) {
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

static std::vector<int> exact_topk(const float* base, int dim,
                                   const std::vector<int>& ids,
                                   const float* qv, int k) {
    std::vector<std::pair<float, int>> buf(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        buf[i] = {l2_avx512(base + static_cast<size_t>(ids[i]) * dim, qv, dim), ids[i]};
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

// Writer-owned visibility state, readable without locks.
// vis[d]: 0 = never granted, 1 = live, 2 = revoked (tombstone).
struct RoleState {
    hnswlib::HierarchicalNSW<float>* h = nullptr;
    std::vector<std::atomic<uint8_t>> vis;  // sized n
    std::atomic<int> live{0};
    std::atomic<uint64_t> version{0};       // bumped per change
    std::unordered_set<int> cur;            // writer-only after build
    std::unordered_set<int> ever;           // writer-only after build
    // writer-only: per document, (version at which the new state applies, new state)
    std::unordered_map<int, std::vector<std::pair<uint64_t, uint8_t>>> hist;
};

static std::atomic<long> retries{0}, fallbacks{0};

int main(int argc, char** argv) {
    if (argc != 14 && argc != 15) {
        std::cerr << "usage: hnsw_lease_epoch base queries roles qroles n dim nq k K changes"
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
        st->vis = std::vector<std::atomic<uint8_t>>(n);
        for (int d : ids[r]) {
            st->h->addPoint(base + static_cast<size_t>(d) * dim, d);
            st->vis[d].store(1, std::memory_order_relaxed);
            st->cur.insert(d);
            st->ever.insert(d);
        }
        st->live.store(static_cast<int>(st->cur.size()), std::memory_order_relaxed);
        states[r] = st;
    }
    const double build_s = std::chrono::duration<double>(Clock::now() - t0).count();

    auto query_one = [&](int q, std::vector<int>& got, uint64_t& v0, uint64_t& v1) -> double {
        const int r = qroles[q];
        const float* qv = queries + static_cast<size_t>(q) * dim;
        auto t = Clock::now();
        got.clear();
        if (promoted.count(r)) {
            auto* st = states[r];
            bool accepted = false;
            for (int attempt = 0; attempt < 8 && !accepted; ++attempt) {
                const uint64_t pin = st->version.load(std::memory_order_acquire);
                if (pin & 1ull) { retries.fetch_add(1); continue; }   // writer in flight
                got.clear();
                const size_t want =
                    std::min<size_t>(static_cast<size_t>(k) * mfac,
                                     static_cast<size_t>(st->live.load(std::memory_order_relaxed)));
                auto res = st->h->searchKnn(qv, want);
                while (!res.empty()) {
                    const int lab = static_cast<int>(res.top().second);
                    res.pop();
                    if (lab >= 0 && lab < n &&
                        st->vis[lab].load(std::memory_order_relaxed) == 1)
                        got.push_back(lab);
                }
                std::reverse(got.begin(), got.end());
                if (static_cast<int>(got.size()) > k) got.resize(k);
                const uint64_t check = st->version.load(std::memory_order_acquire);
                if (check == pin && !(check & 1ull)) { v0 = pin; v1 = check; accepted = true; }
                else retries.fetch_add(1);
            }
            if (!accepted) {                       // bounded retries exhausted
                got = exact_topk(base, dim, ids[r], qv, k);
                v0 = v1 = 0;
                fallbacks.fetch_add(1);
            }
        } else {
            got = exact_topk(base, dim, ids[r], qv, k);
            v0 = v1 = 0;
        }
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };

    std::vector<uint64_t> vpins(nq, 0);                       // accepted pin per query
    std::unordered_map<int, std::unordered_set<int>> initial;  // promoted role -> built set
    for (int r : promoted) initial[r] = std::unordered_set<int>(ids[r].begin(), ids[r].end());

    auto run_phase = [&](bool with_writer, std::vector<std::vector<int>>& gots,
                         std::atomic<int>& straddled) {
        std::vector<double> lat(nq, 0.0);
        std::atomic<bool> go{false};
        std::atomic<int> leaks{0};
        std::vector<std::thread> readers;
        std::thread writer;
        if (with_writer) {
            writer = std::thread([&]() {
                while (!go.load()) {}
                std::mt19937 rng(seed + 7);
                std::uniform_int_distribution<int> opd(0, 1), docd(0, n - 1);
                std::vector<int> hot(ranked.begin(), ranked.begin() + std::min<int>(K, 10));
                std::uniform_int_distribution<int> hotd(0, static_cast<int>(hot.size()) - 1);
                for (int c = 0; c < changes; ++c) {
                    const int r = hot[hotd(rng)];
                    auto* st = states[r];
                    const bool grant = opd(rng);
                    const uint64_t vodd = st->version.fetch_add(1, std::memory_order_acq_rel) + 1;
                    int victim = -1, granted = -1;
                    if (!grant && !st->cur.empty()) {
                        std::uniform_int_distribution<int> pick(0, static_cast<int>(st->cur.size()) - 1);
                        auto it = st->cur.begin();
                        std::advance(it, pick(rng));
                        victim = *it;
                        st->cur.erase(it);
                        st->vis[victim].store(2, std::memory_order_relaxed);
                        st->hist[victim].emplace_back(vodd, 2);
                        st->live.fetch_sub(1, std::memory_order_relaxed);
                    } else if (grant) {
                        for (int tries = 0; tries < 100; ++tries) {
                            const int d = docd(rng);
                            if (st->ever.count(d)) continue;
                            granted = d;
                            st->cur.insert(d);
                            st->ever.insert(d);
                            st->vis[d].store(1, std::memory_order_relaxed);
                            st->hist[d].emplace_back(vodd, 1);
                            st->live.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                    }
                    // Graph work stays inside the odd window: a reader must not observe
                    // an even version whose link lists are half updated. The byte is what
                    // decides safety, so a torn graph costs completeness, not leakage.
                    if (victim >= 0) st->h->markDelete(victim);
                    if (granted >= 0) st->h->addPoint(base + static_cast<size_t>(granted) * dim, granted);
                    st->version.fetch_add(1, std::memory_order_release);   // back to even
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                }
            });
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
                    const int r = qroles[q];
                    if (promoted.count(r)) {
                        auto* st = states[r];
                        for (int d : gots[q])
                            if (st->vis[d].load(std::memory_order_relaxed) != 1) leaks.fetch_add(1);
                    }
                }
            });
        }
        go.store(true);
        auto tq0 = Clock::now();
        for (auto& th : readers) th.join();
        const double phase_s = std::chrono::duration<double>(Clock::now() - tq0).count();
        if (with_writer) writer.join();
        return std::make_tuple(lat, leaks.load(), phase_s);
    };

    std::vector<std::vector<int>> gots_static(nq), gots_inc(nq);
    std::atomic<int> straddled_static{0}, straddled_inc{0};
    auto [lat_static, leaks_static, phase_static_s] = run_phase(false, gots_static, straddled_static);
    auto [lat_inc, leaks_inc, phase_inc_s] = run_phase(true, gots_inc, straddled_inc);

    // Writer has joined; cur is quiescent. Same ground truth as the baseline.
    int missing = 0;
    for (int q = 0; q < nq; ++q) {
        const int r = qroles[q];
        if (!promoted.count(r)) continue;
        auto* st = states[r];
        std::vector<int> cur_ids(st->cur.begin(), st->cur.end());
        auto truth = exact_topk(base, dim, cur_ids, queries + static_cast<size_t>(q) * dim, k);
        std::unordered_set<int> gs(gots_inc[q].begin(), gots_inc[q].end());
        for (int d : truth) if (!gs.count(d)) missing++;
    }

    // Post-run ground truth: was every returned document visible at its read's pin?
    long violations = 0, returned = 0;
    for (int q = 0; q < nq; ++q) {
        const int r = qroles[q];
        if (!promoted.count(r) || vpins[q] == 0) continue;
        auto* st = states[r];
        for (int d : gots_inc[q]) {
            ++returned;
            uint8_t state = initial[r].count(d) ? 1 : 0;
            auto it = st->hist.find(d);
            if (it != st->hist.end())
                for (auto& pr : it->second)
                    if (pr.first <= vpins[q]) state = pr.second;
            if (state != 1) ++violations;
        }
    }

    std::ofstream sm(outdir + "/summary.txt");
    sm << "mode=seqlock"
       << " n=" << n << " dim=" << dim << " nq=" << nq << " k=" << k << " K=" << K
       << " mfac=" << mfac
       << " changes=" << changes << " threads=" << threads
       << " build_s=" << build_s
       << " qps_static=" << (phase_static_s > 0 ? nq / phase_static_s : 0)
       << " qps_inc=" << (phase_inc_s > 0 ? nq / phase_inc_s : 0)
       << " static_p50=" << pctl(lat_static, 0.50) << " static_p95=" << pctl(lat_static, 0.95)
       << " incr_p50=" << pctl(lat_inc, 0.50) << " incr_p95=" << pctl(lat_inc, 0.95)
       << " static_leaks=" << leaks_static << " inc_leaks=" << leaks_inc
       << " straddled=" << straddled_inc.load()
       << " retries=" << retries.load() << " fallbacks=" << fallbacks.load()
       << " returned=" << returned << " pin_violations=" << violations
       << " inc_missing=" << missing << "\n";
    std::cout << "build_s=" << build_s
              << " incr_p50=" << pctl(lat_inc, 0.50)
              << " incr_p95=" << pctl(lat_inc, 0.95)
              << " inc_leaks=" << leaks_inc << " straddled=" << straddled_inc.load()
              << " retries=" << retries.load() << " fallbacks=" << fallbacks.load()
              << " pin_violations=" << violations
              << " inc_missing=" << missing << "\n";
    return 0;
}
