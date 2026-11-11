// Single-kernel mechanism ablation and deployment-boundary harness.
//
// One binary, one corpus, one query set, one deterministic change stream.  The
// arm is selected at run time and changes ONLY the validity predicate / repair
// policy; the graph, the visibility filter, the ring layout, the query loop and
// the scoring code are shared.
//
//   A  version equality: a lease is usable only while pin == build epoch; any
//      committed change (even a revocation) forces an exact scan.
//   B  coverage predicate, no grant log: usable while no grant has committed
//      for the role; a revocation needs no repair, a grant does.
//   C  coverage + log, no certificate gate: always answers from graph + ring,
//      even when the log has a hole (DROP_EVERY).  Unsafe only under a fault.
//   D  full lease: certificate gate pin <= covered_until, exact fallback
//      otherwise.
//   E  D + periodic merge: rebuild the graph over the visible set every
//      MERGE_EVERY changes per role and publish a new lease.
//
// Metrics: p50/p95/p99 and mean latency (overall and split by lease / exact
// fallback), fallback rate, leased-read share, missing / auth-missing / leaks,
// update cost (per change, including merge), merge count and time, peak RSS,
// materialization factor, ring rows, live documents.
//
// usage:
//   arms_boundary base queries roles qroles n dim nq k K changes threads seed outdir mfac
// env:
//   ARM=A|B|C|D|E  AGGR=0..100  CHANGE_US=200  DROP_EVERY=0
//   MERGE_EVERY=0  TRACE=path.csv  CSV=path.csv  TAG=name

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define protected public
#include "hnswlib/hnswlib.h"
#undef protected

using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

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
    if (kk < static_cast<int>(buf.size()))
        std::nth_element(buf.begin(), buf.begin() + kk, buf.end());
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

static double mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double s = 0;
    for (double x : v) s += x;
    return s / v.size();
}

static double current_rss_mb() {
    std::ifstream f("/proc/self/status");
    std::string key;
    while (f >> key) {
        if (key == "VmRSS:") { long kb = 0; f >> kb; return kb / 1024.0; }
        std::string rest; std::getline(f, rest);
    }
    return 0.0;
}

static double peak_rss_mb() {
    std::ifstream f("/proc/self/status");
    std::string key;
    while (f >> key) {
        if (key == "VmHWM:") {
            long kb = 0;
            f >> kb;
            return kb / 1024.0;
        }
        std::string rest;
        std::getline(f, rest);
    }
    return 0.0;
}

struct Lease {
    hnswlib::HierarchicalNSW<float>* g = nullptr;
    int ring_base = 0;
    uint64_t epoch = 0;
};

struct Role {
    int rid = 0;
    std::vector<int> init;
    std::vector<std::atomic<uint32_t>> grant_v, revoke_v, covered_v;
    std::vector<int> ever;                 // append-only, readers scan [0, ever_n)
    std::atomic<int> ever_n{0};
    std::vector<std::atomic<int>> ring;    // append-only grant ids
    std::atomic<int> ring_n{0};
    std::atomic<uint64_t> version{0};      // last committed epoch
    std::atomic<uint64_t> covered_until{0};
    std::atomic<bool> frozen{false};
    std::atomic<Lease*> lease{nullptr};
    std::atomic<int> grants_since_build{0};
    std::atomic<int> changes_since_merge{0};
    std::atomic<long> merges{0};
    // writer-only
    std::unordered_set<int> ever_set;
};

static inline bool visible(const Role& R, int d, uint64_t pin) {
    const uint32_t g = R.grant_v[d].load(std::memory_order_relaxed);
    const uint32_t rv = R.revoke_v[d].load(std::memory_order_relaxed);
    return g <= pin && (rv == 0 || rv > pin);
}

static inline bool covered(const Role& R, int d, uint64_t pin) {
    const uint32_t g = R.grant_v[d].load(std::memory_order_relaxed);
    if (g == 0) return true;                       // present at build time
    const uint32_t c = R.covered_v[d].load(std::memory_order_relaxed);
    return c != 0 && c <= pin;
}

static Lease* build_lease(const float* base, int dim, Role& R, uint64_t epoch,
                          int ring_base, int M, int efc) {
    static hnswlib::L2Space* space = nullptr;      // one space for the process
    if (!space) space = new hnswlib::L2Space(dim);
    std::vector<int> ids;
    const int en = R.ever_n.load(std::memory_order_acquire);
    ids.reserve(en);
    for (int i = 0; i < en; ++i) {
        const int d = R.ever[i];
        if (visible(R, d, epoch)) ids.push_back(d);
    }
    auto* g = new hnswlib::HierarchicalNSW<float>(space, ids.size() + 16, M, efc, 100, false, true);
    for (int d : ids) g->addPoint(base + static_cast<size_t>(d) * dim, d);
    if (std::getenv("DBG"))
        std::cerr << "build ids=" << ids.size() << " n=" << g->getCurrentElementCount()
                  << " M=" << M << " efc=" << efc << " ring_base=" << ring_base
                  << " epoch=" << epoch << " ef=" << g->ef_ << " maxM0=" << g->maxM0_
                  << " maxlevel=" << g->maxlevel_ << "\n";
    auto* L = new Lease();
    L->g = g;
    L->ring_base = ring_base;
    L->epoch = epoch;
    return L;
}

int main(int argc, char** argv) {
    if (argc < 15) {
        std::cerr << "usage: arms_boundary base queries roles qroles n dim nq k K changes "
                     "threads seed outdir mfac\n";
        return 2;
    }
    const std::string base_path = argv[1], query_path = argv[2], roles_path = argv[3],
                      qroles_path = argv[4];
    const int n = std::atoi(argv[5]), dim = std::atoi(argv[6]), nq = std::atoi(argv[7]);
    const int k = std::atoi(argv[8]), K = std::atoi(argv[9]), changes = std::atoi(argv[10]);
    const int threads = std::atoi(argv[11]), seed = std::atoi(argv[12]);
    const std::string outdir = argv[13];
    const int mfac = argc > 14 ? std::atoi(argv[14]) : 15;

    const char* arm_env = std::getenv("ARM");
    const char arm = arm_env ? arm_env[0] : 'D';
    const char* aggr_env = std::getenv("AGGR");
    const int aggr = aggr_env ? std::atoi(aggr_env) : 50;
    const char* cu = std::getenv("CHANGE_US");
    const int change_us = cu ? std::atoi(cu) : 200;
    const char* de = std::getenv("DROP_EVERY");
    const int drop_every = de ? std::atoi(de) : 0;
    const char* me = std::getenv("MERGE_EVERY");
    const int merge_every = me ? std::atoi(me) : 0;
    const char* trace_env = std::getenv("TRACE");
    const char* rss_trace_env = std::getenv("RSS_TRACE");
    const char* csv_env = std::getenv("CSV");
    const char* tag_env = std::getenv("TAG");

    auto qb = read_file(query_path);
    auto qroles_b = read_file(qroles_path);
    size_t qoff = 0;
    int nq_file = 0;
    read_scalar(qroles_b, qoff, nq_file);
    std::vector<int> qroles(nq_file);
    std::memcpy(qroles.data(), qroles_b.data() + qoff, sizeof(int) * nq_file);
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
    std::vector<char> qbuf;
    {
        std::ifstream f(query_path, std::ios::binary);
        if (!f) { std::cerr << "cannot open " << query_path << "\n"; return 1; }
        qbuf.resize(static_cast<size_t>(nq_eff) * dim * sizeof(float));
        f.read(qbuf.data(), qbuf.size());
        if (!f) { std::cerr << "short read query file\n"; return 1; }
    }
    const float* queries = reinterpret_cast<const float*>(qbuf.data());

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

    std::vector<Role*> by_role(roles.size(), nullptr);
    std::unordered_map<int, Role*> owned;
    const int M = 16, efc = 100;

    const bool exact_only = (arm == 'X');
    auto t_build0 = Clock::now();
    for (int r : promoted) {
        if (exact_only) break;
        auto* R = new Role();
        R->rid = r;
        R->init = roles[r];
        R->grant_v = std::vector<std::atomic<uint32_t>>(n);
        R->revoke_v = std::vector<std::atomic<uint32_t>>(n);
        R->covered_v = std::vector<std::atomic<uint32_t>>(n);
        for (int i = 0; i < n; ++i) {
            R->grant_v[i].store(0, std::memory_order_relaxed);
            R->revoke_v[i].store(0, std::memory_order_relaxed);
            R->covered_v[i].store(0, std::memory_order_relaxed);
        }
        R->ever.reserve(std::min<size_t>(static_cast<size_t>(n),
                                         R->init.size() + changes + 16));
        R->ring = std::vector<std::atomic<int>>(changes + 64);
        for (int i = 0; i < changes + 64; ++i) R->ring[i].store(0);
        for (int d : R->init) {
            R->ever.push_back(d);
            R->ever_set.insert(d);
        }
        R->ever_n.store(static_cast<int>(R->ever.size()));
        R->lease.store(build_lease(base, dim, *R, 0, 0, M, efc), std::memory_order_release);
        by_role[r] = R;
        owned[r] = R;
    }
    const double build_ms = ms_since(t_build0);

    long mat_docs = 0;
    for (int r : promoted) mat_docs += static_cast<long>(roles[r].size());
    const double mat_factor = static_cast<double>(mat_docs) / n;

    std::atomic<bool> go{false}, done{false};
    std::atomic<long> leaks{0}, missing{0}, auth_missing{0}, returned{0};
    std::atomic<long> leased_reads{0}, fallback_reads{0}, unpromoted_reads{0};
    std::atomic<long> prof_search_ns{0}, prof_post_ns{0};
    std::vector<std::vector<double>> lat(threads);
    std::vector<std::vector<double>> lat_lease(threads), lat_exact(threads);
    std::vector<std::vector<double>> trace_t(threads), trace_lat(threads);
    std::vector<std::vector<char>> trace_path(threads);
    const auto t_start = Clock::now();
    std::vector<double> update_ms;
    std::atomic<long> merge_count{0};
    std::vector<double> merge_ms_vec;

    auto do_merge = [&](Role* R) {
        const uint64_t ver = R->version.load(std::memory_order_acquire);
        const int base_i = R->ring_n.load(std::memory_order_acquire);
        auto t0 = Clock::now();
        Lease* L = build_lease(base, dim, *R, ver, base_i, M, efc);
        merge_ms_vec.push_back(ms_since(t0));
        R->lease.store(L, std::memory_order_release);
        R->covered_until.store(ver, std::memory_order_release);
        R->frozen.store(false, std::memory_order_relaxed);
        R->grants_since_build.store(0, std::memory_order_relaxed);
        R->changes_since_merge.store(0, std::memory_order_relaxed);
        R->merges.fetch_add(1, std::memory_order_relaxed);
        merge_count.fetch_add(1, std::memory_order_relaxed);
    };

    std::thread writer([&]() {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        if (exact_only) { done.store(true, std::memory_order_release); return; }
        std::vector<int> pr(promoted.begin(), promoted.end());
        std::sort(pr.begin(), pr.end());
        unsigned s = static_cast<unsigned>(seed) * 2654435761u + 1;
        auto rnd = [&]() { s = s * 1664525u + 1013904223u; return s; };
        for (int c = 0; c < changes; ++c) {
            const int r = pr[rnd() % pr.size()];
            Role& R = *by_role[r];
            const bool grant = static_cast<int>(rnd() % 100) < aggr;
            auto t0 = Clock::now();
            const uint64_t vcommit = R.version.load(std::memory_order_relaxed) + 1;
            if (grant) {
                int d = -1;
                for (int t = 0; t < 200; ++t) {
                    const int cand = static_cast<int>(rnd() % n);
                    if (!R.ever_set.count(cand)) { d = cand; break; }
                }
                if (d < 0) { std::this_thread::sleep_for(std::chrono::microseconds(change_us)); continue; }
                R.grant_v[d].store(static_cast<uint32_t>(vcommit), std::memory_order_relaxed);
                R.revoke_v[d].store(0, std::memory_order_relaxed);
                R.ever_set.insert(d);
                R.ever.push_back(d);
                R.ever_n.store(static_cast<int>(R.ever.size()), std::memory_order_release);
                const bool drop = drop_every > 0 && ((c + 1) % drop_every == 0);
                if (!drop) {
                    const int slot = R.ring_n.load(std::memory_order_relaxed);
                    if (slot < changes + 64) {
                        R.ring[slot].store(d, std::memory_order_relaxed);
                        R.ring_n.store(slot + 1, std::memory_order_release);
                    }
                    R.covered_v[d].store(static_cast<uint32_t>(vcommit), std::memory_order_relaxed);
                    if (!R.frozen.load(std::memory_order_relaxed))
                        R.covered_until.store(vcommit, std::memory_order_release);
                } else {
                    R.frozen.store(true, std::memory_order_release);
                }
                R.grants_since_build.fetch_add(1, std::memory_order_relaxed);
            } else {
                const int en = R.ever_n.load(std::memory_order_relaxed);
                int d = -1;
                for (int t = 0; t < 64 && en > 0; ++t) {
                    const int cand = R.ever[rnd() % en];
                    if (R.revoke_v[cand].load(std::memory_order_relaxed) == 0) { d = cand; break; }
                }
                if (d < 0) { std::this_thread::sleep_for(std::chrono::microseconds(change_us)); continue; }
                R.revoke_v[d].store(static_cast<uint32_t>(vcommit), std::memory_order_relaxed);
                if (!R.frozen.load(std::memory_order_relaxed))
                    R.covered_until.store(vcommit, std::memory_order_release);
            }
            R.version.store(vcommit, std::memory_order_release);
            R.changes_since_merge.fetch_add(1, std::memory_order_relaxed);
            if (arm == 'E' && merge_every > 0 &&
                R.changes_since_merge.load(std::memory_order_relaxed) >= merge_every) {
                do_merge(&R);
            }
            update_ms.push_back(ms_since(t0));
            std::this_thread::sleep_for(std::chrono::microseconds(change_us));
        }
        done.store(true, std::memory_order_release);
    });

    auto run = [&](int tid) {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        do {
            for (int q = tid; q < nq_eff; q += threads) {
                const int r = qroles[q];
                const float* qv = queries + static_cast<size_t>(q) * dim;
                const uint64_t pin = by_role[r] ? by_role[r]->version.load(std::memory_order_acquire) : 0;
                auto t0 = Clock::now();
                std::vector<int> got;
                char path = 'x';
                if (!by_role[r]) {
                    // cold role (or the exact-only reference arm): exact scan
                    got = exact_topk(base, dim, roles[r], qv, k);
                    path = exact_only ? 'f' : 'c';
                    if (exact_only) fallback_reads.fetch_add(1, std::memory_order_relaxed);
                    else unpromoted_reads.fetch_add(1, std::memory_order_relaxed);
                } else {
                    Role& R = *by_role[r];
                    Lease* L = R.lease.load(std::memory_order_acquire);
                    bool use_lease = false;
                    if (L) {
                        switch (arm) {
                            case 'A': use_lease = (pin == L->epoch); break;
                            case 'B': use_lease = (R.grants_since_build.load(std::memory_order_relaxed) == 0); break;
                            case 'C': use_lease = true; break;
                            default:  use_lease = (pin <= R.covered_until.load(std::memory_order_acquire)); break;
                        }
                    }
                    if (use_lease) {
                        path = 'l';
                        std::vector<std::pair<float, int>> cand;
                        const int m = k * mfac;
                        auto ts = Clock::now();
                        auto pq = L->g->searchKnn(qv, m);
                        prof_search_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-ts).count(), std::memory_order_relaxed);
                        while (!pq.empty()) {
                            cand.push_back({pq.top().first, static_cast<int>(pq.top().second)});
                            pq.pop();
                        }
                        const int rn = R.ring_n.load(std::memory_order_acquire);
                        for (int i = L->ring_base; i < rn; ++i) {
                            const int d = R.ring[i].load(std::memory_order_relaxed);
                            if (visible(R, d, pin))
                                cand.push_back({l2_dist(base + static_cast<size_t>(d) * dim, qv, dim), d});
                        }
                        auto tp = Clock::now();
                        std::sort(cand.begin(), cand.end());
                        std::unordered_set<int> seen;
                        for (auto& cd : cand) {
                            if (static_cast<int>(got.size()) >= k) break;
                            if (!visible(R, cd.second, pin) || seen.count(cd.second)) continue;
                            seen.insert(cd.second);
                            got.push_back(cd.second);
                        }
                        prof_post_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-tp).count(), std::memory_order_relaxed);
                        leased_reads.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        path = 'f';
                        std::vector<int> ids;
                        const int en = R.ever_n.load(std::memory_order_acquire);
                        for (int i = 0; i < en; ++i) {
                            const int d = R.ever[i];
                            if (visible(R, d, pin)) ids.push_back(d);
                        }
                        got = exact_topk(base, dim, ids, qv, k);
                        fallback_reads.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                const double dt = ms_since(t0);
                // score outside the timed region
                std::vector<int> T;
                if (!by_role[r]) {
                    T = exact_topk(base, dim, roles[r], qv, k);
                    returned.fetch_add(static_cast<long>(got.size()), std::memory_order_relaxed);
                    std::unordered_set<int> gs(got.begin(), got.end());
                    for (int d : T) if (!gs.count(d)) missing.fetch_add(1, std::memory_order_relaxed);
                } else {
                    Role& R = *by_role[r];
                    std::vector<int> ids;
                    const int en = R.ever_n.load(std::memory_order_acquire);
                    for (int i = 0; i < en; ++i) {
                        const int d = R.ever[i];
                        if (visible(R, d, pin)) ids.push_back(d);
                    }
                    T = exact_topk(base, dim, ids, qv, k);
                    returned.fetch_add(static_cast<long>(got.size()), std::memory_order_relaxed);
                    std::unordered_set<int> gs(got.begin(), got.end());
                    for (int d : got)
                        if (!visible(R, d, pin)) leaks.fetch_add(1, std::memory_order_relaxed);
                    for (int d : T) {
                        if (gs.count(d)) continue;
                        missing.fetch_add(1, std::memory_order_relaxed);
                        if (!covered(R, d, pin)) auth_missing.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                lat[tid].push_back(dt);
                if (path == 'l') lat_lease[tid].push_back(dt);
                else lat_exact[tid].push_back(dt);
                if (trace_env) {
                    trace_t[tid].push_back(ms_since(t_start));
                    trace_lat[tid].push_back(dt);
                    trace_path[tid].push_back(path);
                }
            }
        } while (!done.load(std::memory_order_acquire));
    };

    std::thread rss_thread;
    if (rss_trace_env) {
        rss_thread = std::thread([&]() {
            std::ofstream rf(rss_trace_env);
            rf << "t_ms,rss_mb\n";
            while (!done.load(std::memory_order_acquire)) {
                rf << ms_since(t_start) << "," << current_rss_mb() << "\n";
                rf.flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            rf << ms_since(t_start) << "," << current_rss_mb() << "\n";
        });
    }
    go.store(true, std::memory_order_release);
    std::vector<std::thread> readers;
    for (int t = 0; t < threads; ++t) readers.emplace_back(run, t);
    writer.join();
    for (auto& th : readers) th.join();
    if (rss_thread.joinable()) rss_thread.join();

    std::vector<double> all, lease_l, exact_l;
    for (int t = 0; t < threads; ++t) {
        all.insert(all.end(), lat[t].begin(), lat[t].end());
        lease_l.insert(lease_l.end(), lat_lease[t].begin(), lat_lease[t].end());
        exact_l.insert(exact_l.end(), lat_exact[t].begin(), lat_exact[t].end());
    }
    const long reads = static_cast<long>(all.size());
    const double rss = peak_rss_mb();
    const double upd_total = [&] { double s = 0; for (double x : update_ms) s += x; return s; }();
    double merge_total = 0;
    for (double x : merge_ms_vec) merge_total += x;
    const long ring_rows = [&] { long s = 0; for (auto& kv : owned) s += kv.second->ring_n.load(); return s; }();
    const long ever_rows = [&] { long s = 0; for (auto& kv : owned) s += kv.second->ever_n.load(); return s; }();

    std::ostringstream os;
    os << std::fixed << std::setprecision(4);
    os << "tag=" << (tag_env ? tag_env : "") << " arm=" << arm << " aggr=" << aggr
       << " drop_every=" << drop_every << " merge_every=" << merge_every
       << " seed=" << seed << " n=" << n << " dim=" << dim << " nq=" << nq_eff
       << " k=" << k << " K=" << K << " changes=" << changes << " threads=" << threads
       << " mfac=" << mfac << "\n";
    os << "reads=" << reads << " leased=" << leased_reads.load()
       << " fallbacks=" << fallback_reads.load() << " unpromoted=" << unpromoted_reads.load()
       << " fallback_rate=" << (reads ? double(fallback_reads.load()) / reads : 0.0)
       << " leased_rate=" << (reads ? double(leased_reads.load()) / reads : 0.0) << "\n";
    os << "p50=" << pctl(all, 0.50) << " p95=" << pctl(all, 0.95)
       << " p99=" << pctl(all, 0.99) << " mean=" << mean(all) << "\n";
    os << "lease_p50=" << pctl(lease_l, 0.50) << " lease_p95=" << pctl(lease_l, 0.95)
       << " lease_mean=" << mean(lease_l) << " lease_reads=" << lease_l.size() << "\n";
    os << "exact_p50=" << pctl(exact_l, 0.50) << " exact_p95=" << pctl(exact_l, 0.95)
       << " exact_mean=" << mean(exact_l) << " exact_reads=" << exact_l.size() << "\n";
    os << "missing=" << missing.load() << " auth=" << auth_missing.load()
       << " leaks=" << leaks.load() << " returned=" << returned.load() << "\n";
    os << "applied=" << update_ms.size() << " update_total_ms=" << upd_total
       << " update_p50_ms=" << pctl(update_ms, 0.50)
       << " update_p95_ms=" << pctl(update_ms, 0.95)
       << " merges=" << merge_count.load() << " merge_total_ms=" << merge_total << "\n";
    long dcomps=0; for (auto& kv : owned) dcomps += kv.second->lease.load()->g->metric_distance_computations.load();
    os << "dist_comps=" << dcomps << "\n";
    os << "prof_search_ms=" << prof_search_ns.load()/1e6 << " prof_post_ms=" << prof_post_ns.load()/1e6 << "\n";
    os << "rss_mb=" << rss << " build_ms=" << build_ms << " matfac=" << mat_factor
       << " mat_docs=" << mat_docs << " ring_rows=" << ring_rows << " ever_docs=" << ever_rows << "\n";

    std::ofstream sm(outdir + "/summary.txt");
    sm << os.str();
    std::cout << os.str();

    if (csv_env) {
        const bool exists = std::ifstream(csv_env).good();
        std::ofstream cf(csv_env, std::ios::app);
        if (!exists)
            cf << "tag,arm,conc,K,aggr,drop,merge,seed,reads,leased_rate,fallback_rate,p50,p95,p99,mean,"
                  "lease_p50,exact_p50,missing,auth,leaks,update_total_ms,updates,merges,merge_total_ms,rss_mb,matfac\n";
        cf << (tag_env ? tag_env : "") << "," << arm << "," << (std::getenv("CONC") ? std::getenv("CONC") : "")
           << "," << K << "," << aggr << "," << drop_every << "," << merge_every << "," << seed << ","
           << reads << "," << (reads ? double(leased_reads.load()) / reads : 0.0) << ","
           << (reads ? double(fallback_reads.load()) / reads : 0.0) << ","
           << pctl(all, 0.50) << "," << pctl(all, 0.95) << "," << pctl(all, 0.99) << ","
           << mean(all) << "," << pctl(lease_l, 0.50) << "," << pctl(exact_l, 0.50) << ","
           << missing.load() << "," << auth_missing.load() << "," << leaks.load() << ","
           << upd_total << "," << update_ms.size() << "," << merge_count.load() << ","
           << merge_total << "," << rss << "," << mat_factor << "\n";
    }

    if (trace_env) {
        std::ofstream tf(trace_env);
        tf << "read,t_ms,lat_ms,path\n";
        long idx = 0;
        for (int t = 0; t < threads; ++t)
            for (size_t i = 0; i < trace_lat[t].size(); ++i)
                tf << idx++ << "," << trace_t[t][i] << "," << trace_lat[t][i] << ","
                   << trace_path[t][i] << "\n";
    }
    return 0;
}
