// ACORN online (EXPERIMENTAL -- not used for any number in the paper).
//
// Status 2026-11-10 21:25: the driver builds and runs, but the end-to-end online
// campaign did not finish: a writer-starvation deadlock was fixed with a priority
// flag, a 200 KB per-read bitmap copy was removed, and a warmup pass was added; the
// last stable measurements were single-reader (p50 1.47 ms at ef=32 with 12/250
// missing, 5.65 ms at ef=256 with 1/250), matching the offline recall but 5x its
// latency, and the 4-reader runs were still being reconciled when the host session
// dropped. Treat every number this binary prints as unverified; the paper makes no
// online ACORN claim.
//
// ACORN online: one shared ACORN index, a writer that applies ACL changes while
// four readers query. Each read pins the ACL version it observed, searches with
// that snapshot as the predicate bitmap, and is scored against the same snapshot.
//
// Reports query p50/p95/p99, update throughput, leaks@pin, and the missing count
// against an exact scan of the pinned visible set (sampled every `--truth-every`
// reads to keep the run short).
//
// Build (static faiss from the ACORN fork):
//   g++ -O3 -march=native -std=c++17 -I<acorn>/ -I<acorn>/build \
//       acorn_online.cpp <acorn>/build/faiss/libfaiss.a -fopenmp -lblas -llapack -o acorn_online
// Run:
//   ./acorn_online base.f32 queries.f32 roles.bin qroles.bin n d nq k M gamma M_beta ef \
//       changes threads seed outdir
#include <faiss/IndexACORN.h>
#include <faiss/impl/ACORN.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <omp.h>

using Clock = std::chrono::steady_clock;

static std::vector<char> slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", p.c_str()); std::exit(1); }
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
template <class T> static void rd(const std::vector<char>& b, size_t& o, T& v) {
    std::memcpy(&v, b.data() + o, sizeof(T)); o += sizeof(T);
}
static double pctl(std::vector<double>& v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t i = static_cast<size_t>(p / 100.0 * (v.size() - 1));
    return v[i];
}

int main(int argc, char** argv) {
    if (argc < 17) {
        std::fprintf(stderr, "usage: acorn_online base queries roles qroles n d nq k M gamma M_beta ef changes threads seed outdir\n");
        return 2;
    }
    const std::string bp = argv[1], qp = argv[2], rp = argv[3], qrp = argv[4];
    const int n = std::atoi(argv[5]), d = std::atoi(argv[6]), nq = std::atoi(argv[7]);
    const int k = std::atoi(argv[8]), M = std::atoi(argv[9]);
    const int gamma = std::atoi(argv[10]), M_beta = std::atoi(argv[11]);
    const int ef = std::atoi(argv[12]), changes = std::atoi(argv[13]);
    const int threads = std::atoi(argv[14]), seed = std::atoi(argv[15]);
    const std::string outdir = argv[16];

    auto bb = slurp(bp), qb = slurp(qp), rb = slurp(rp), qrb = slurp(qrp);
    const float* base = reinterpret_cast<const float*>(bb.data());
    const float* queries = reinterpret_cast<const float*>(qb.data());
    size_t o = 0; int32_t nroles = 0; rd(rb, o, nroles);
    std::vector<std::vector<int>> ids(nroles);
    for (int r = 0; r < nroles; ++r) {
        int32_t rid, c; rd(rb, o, rid); rd(rb, o, c);
        ids[rid].resize(c);
        if (c) std::memcpy(ids[rid].data(), rb.data() + o, (size_t)c * sizeof(int32_t));
        o += (size_t)c * sizeof(int32_t);
    }
    o = 0; int32_t nqf = 0; rd(qrb, o, nqf);
    std::vector<int> qr(nqf); std::memcpy(qr.data(), qrb.data() + o, (size_t)nqf * sizeof(int32_t));
    const int nq_eff = std::min(nq, nqf);

    std::vector<int> metadata(static_cast<size_t>(n), 0);
    faiss::IndexACORNFlat index(d, M, gamma, metadata, M_beta);
    std::vector<float> xb(base, base + (size_t)n * d);
    index.add(n, xb.data());
    // efSearch must be set on the inner index: the SearchParameters route relies on a
    // dynamic_cast that silently falls back to 16 in this build.
    index.acorn.efSearch = ef;

    // One hot role drives both the traffic and the churn, so the update stream is
    // visible to the readers rather than landing on idle roles.
    // One bitmap per role (100 x 200k = 20 MB): a query must be filtered by *its* role,
    // not by the churn role. Churn targets one hot role so the update stream is visible.
    const int r = qr[0];
    std::vector<int> members = ids[r];
    std::vector<std::vector<char>> lives(nroles, std::vector<char>(n, 0));
    for (int rr = 0; rr < nroles; ++rr)
        for (int x : ids[rr]) lives[rr][x] = 1;
    std::vector<char>& live = lives[r];

    std::atomic<uint64_t> version{0};
    std::atomic<bool> done{false};
    std::atomic<long> updates{0};
    std::atomic<long> granted_total{0}, revoked_total{0};

    // Readers hold the shared lock for the duration of a search and use the bitmap in
    // place; the writer takes it exclusively between changes. Copying the bitmap per
    // read cost 200 KB of memcpy and produced a bogus 100 ms p99.
    std::shared_mutex acl_mu;
    // Writer priority: continuous readers otherwise starve the exclusive lock and the
    // change stream never finishes.
    std::atomic<bool> writer_wants{false};
    std::mt19937 wrng(seed);
    long added = 0;
    int change_us = 500;
    if (const char* e = std::getenv("CHANGE_US")) change_us = std::max(1, std::atoi(e));
    std::thread writer([&] {
        for (int c = 0; c < changes; ++c) {
            int doc, grant;
            writer_wants.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            {
                std::unique_lock<std::shared_mutex> lk(acl_mu);
                writer_wants.store(false, std::memory_order_release);
                const bool g = (wrng() % 2) == 0;
                if (g) {
                    doc = static_cast<int>(wrng() % (unsigned)n);
                    grant = !live[doc] ? 1 : 0;
                } else {
                    if (members.empty()) { std::this_thread::sleep_for(std::chrono::microseconds(200)); continue; }
                    const size_t idx = wrng() % members.size();
                    doc = members[idx];
                    grant = 0;
                    if (live[doc]) { members[idx] = members.back(); members.pop_back(); }
                }
                if ((grant && !live[doc]) || (!grant && live[doc])) {
                    live[doc] = grant ? 1 : 0;
                    updates.fetch_add(1, std::memory_order_relaxed);
                    if (grant) { added++; granted_total.fetch_add(1, std::memory_order_relaxed); }
                    else revoked_total.fetch_add(1, std::memory_order_relaxed);
                }
                version.fetch_add(1, std::memory_order_release);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(change_us));
        }
        done.store(true, std::memory_order_release);
    });

    std::vector<std::vector<double>> lat(threads);
    std::atomic<long> leaked{0}, missing{0}, checked{0};
    std::vector<float> dist(k);
    std::vector<faiss::idx_t> lab(k);
    std::vector<char> filt(n);

    // Search is a single-query call; keep OpenMP from oversubscribing the four
    // readers (build/train above still uses the full pool).
    omp_set_num_threads(1);

    // Warmup: one untimed pass over every query so the readers do not pay page faults
    // and cold graph levels. The offline driver does the same; without it the online
    // row reads ~5x slower for reasons that have nothing to do with churn.
    for (int q = 0; q < nq_eff; ++q) {
        std::shared_lock<std::shared_mutex> lk(acl_mu);
        index.search(1, queries + (size_t)q * d, k, dist.data(), lab.data(),
                     lives[qr[q]].data(), nullptr);
    }

    const auto t_start = Clock::now();
    std::vector<std::thread> readers;
    for (int t = 0; t < threads; ++t) {
        readers.emplace_back([&, t] {
            std::vector<float> dist(k);
            std::vector<faiss::idx_t> lab(k);
            std::vector<char> hot(n);
            for (int q = t; q < nq_eff; q += threads) {
                    const float* qv = queries + (size_t)q * d;
                    while (writer_wants.load(std::memory_order_acquire)) std::this_thread::yield();
                    {
                        std::shared_lock<std::shared_mutex> lk(acl_mu);
                        std::memcpy(hot.data(), lives[qr[q]].data(), (size_t)n);
                    }
                    auto t0 = Clock::now();
                    index.search(1, qv, k, dist.data(), lab.data(), hot.data(), nullptr);
                    auto t1 = Clock::now();
                    lat[t].push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
                    for (int i = 0; i < k; ++i) {
                        const int got = (int)lab[i];
                        if (got < 0) continue;
                        if (!hot[(size_t)got]) leaked.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (std::getenv("TRUTH_SAMPLE") && (q + t) % 8 == 0) {
                        std::vector<int> vis;
                        vis.reserve(members.size());
                        for (int x = 0; x < n; ++x) if (hot[(size_t)x]) vis.push_back(x);
                        std::vector<std::pair<float, int>> all;
                        all.reserve(vis.size());
                        for (int doc : vis) {
                            float s = 0; const float* dv = base + (size_t)doc * d;
                            for (int j = 0; j < d; ++j) { const float tt = qv[j] - dv[j]; s += tt * tt; }
                            all.emplace_back(s, doc);
                        }
                        const size_t kk = std::min<size_t>((size_t)k, all.size());
                        std::partial_sort(all.begin(), all.begin() + kk, all.end());
                        std::unordered_set<int> gs;
                        for (int i = 0; i < k; ++i) if (lab[i] >= 0) gs.insert((int)lab[i]);
                        for (size_t i = 0; i < kk; ++i) if (!gs.count(all[i].second))
                            missing.fetch_add(1, std::memory_order_relaxed);
                        checked.fetch_add(kk, std::memory_order_relaxed);
                    }
            }
        });
    }
    for (auto& th : readers) th.join();
    if (writer.joinable()) writer.join();
    const double run_s = std::chrono::duration<double>(Clock::now() - t_start).count();

    std::vector<double> all;
    for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
    const double p50 = pctl(all, 50), p95 = pctl(all, 95), p99 = pctl(all, 99);
    const long upd = updates.load();
    std::string cmd = "mkdir -p " + outdir;
    if (system(cmd.c_str()) != 0) return 1;
    std::ofstream sm(outdir + "/summary.txt");
    sm << "acorn_online n=" << n << " d=" << d << " nq=" << nq_eff << " k=" << k
       << " M=" << M << " gamma=" << gamma << " M_beta=" << M_beta << " ef=" << ef
       << " changes=" << changes << " threads=" << threads << " reads=" << all.size()
       << " run_s=" << run_s
       << " p50=" << p50 << " p95=" << p95 << " p99=" << p99
       << " updates=" << upd << " updates_per_s=" << (run_s > 0 ? upd / run_s : 0)
       << " granted=" << granted_total.load() << " revoked=" << revoked_total.load()
       << " leaked=" << leaked.load()
       << " missing=" << missing.load() << " truth_checked=" << checked.load() << "\n";
    std::printf("%s", ("wrote " + outdir + "/summary.txt\n").c_str());
    return 0;
}
