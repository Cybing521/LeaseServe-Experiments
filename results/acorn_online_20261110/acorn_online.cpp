// ACORN online driver used in supplement §ACORN's online path.
//
// Order: build index -> untimed warmup over all queries -> barrier releases
// 1 reader + 1 writer together. The writer applies 50 changes at a fixed
// cadence; each update interval and each read interval is recorded, so
// overlap_reads / overlapping_updates in summary.txt report actual overlap.
// TRUTH_SAMPLE=1 adds an online exact check against the *pinned* bitmap
// (quality run only; it pollutes latency).
//
// Source hash and run commands: experiments/results/acorn_online_20261110/MANIFEST.md
//
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

    std::atomic<long> updates{0};
    std::atomic<long> granted_total{0}, revoked_total{0};
    std::atomic<long> leaked{0}, missing{0}, checked{0};
    std::shared_mutex acl_mu;
    std::mutex tmu;
    std::vector<std::pair<double,double>> upd_iv, read_iv;

    std::mt19937 wrng(seed);
    long added = 0;
    int change_us = 0;
    if (const char* e = std::getenv("CHANGE_US")) change_us = std::max(1, std::atoi(e));

    std::vector<float> wdist(k);
    std::vector<faiss::idx_t> wlab(k);
    std::vector<char> filt(n);

    omp_set_num_threads(1);

    // Warmup FIRST: the timed readers and the writer start together afterwards.
    auto tw0 = Clock::now();
    for (int q = 0; q < nq_eff; ++q) {
        std::shared_lock<std::shared_mutex> lk(acl_mu);
        index.search(1, queries + (size_t)q * d, k, wdist.data(), wlab.data(),
                     lives[qr[q]].data(), nullptr);
    }
    const double warm_s = std::chrono::duration<double>(Clock::now() - tw0).count();
    if (change_us <= 0 && changes > 0) {
        // Spread the changes over roughly the timed reader window.
        change_us = std::max(1000, (int)(warm_s * 1e6 / changes * 1.05));
    }

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    Clock::time_point t_origin;

    std::thread writer;
    if (changes > 0) {
        writer = std::thread([&] {
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int c = 0; c < changes; ++c) {
                auto u0 = Clock::now();
                int doc = 0, grant = 0;
                {
                    std::unique_lock<std::shared_mutex> lk(acl_mu);
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
                }
                auto u1 = Clock::now();
                {
                    std::lock_guard<std::mutex> g(tmu);
                    upd_iv.emplace_back(std::chrono::duration<double>(u0 - t_origin).count(),
                                       std::chrono::duration<double>(u1 - t_origin).count());
                }
                std::this_thread::sleep_for(std::chrono::microseconds(change_us));
            }
        });
    }

    std::vector<std::vector<double>> lat(threads);
    std::vector<std::thread> readers;
    for (int t = 0; t < threads; ++t) {
        readers.emplace_back([&, t] {
            std::vector<float> dist(k);
            std::vector<faiss::idx_t> lab(k);
            std::vector<char> hot(n);
            std::vector<std::pair<double,double>> local;
            local.reserve((size_t)nq_eff / threads + 1);
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int q = t; q < nq_eff; q += threads) {
                const float* qv = queries + (size_t)q * d;
                {
                    std::shared_lock<std::shared_mutex> lk(acl_mu);
                    std::memcpy(hot.data(), lives[qr[q]].data(), (size_t)n);
                }
                auto t0 = Clock::now();
                index.search(1, qv, k, dist.data(), lab.data(), hot.data(), nullptr);
                auto t1 = Clock::now();
                lat[t].push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
                local.emplace_back(std::chrono::duration<double>(t0 - t_origin).count(),
                                   std::chrono::duration<double>(t1 - t_origin).count());
                for (int i = 0; i < k; ++i) {
                    const int got = (int)lab[i];
                    if (got < 0) continue;
                    if (!hot[(size_t)got]) leaked.fetch_add(1, std::memory_order_relaxed);
                }
                // Independent online truth: sample the pinned bitmap, not a static ACL.
                if (std::getenv("TRUTH_SAMPLE") && q % 200 == 0) {
                    std::vector<int> vis;
                    vis.reserve(ids[qr[q]].size());
                    for (int x = 0; x < n; ++x) if (hot[(size_t)x]) vis.push_back(x);
                    std::vector<std::pair<float,int>> all;
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
            std::lock_guard<std::mutex> g(tmu);
            read_iv.insert(read_iv.end(), local.begin(), local.end());
        });
    }

    while (ready.load(std::memory_order_acquire) < threads + (changes > 0 ? 1 : 0))
        std::this_thread::yield();
    t_origin = Clock::now();
    go.store(true, std::memory_order_release);
    for (auto& th : readers) th.join();
    if (writer.joinable()) writer.join();
    const double run_s = std::chrono::duration<double>(Clock::now() - t_origin).count();

    long overlap_reads = 0, overlapping_updates = 0;
    for (auto& r : read_iv) {
        for (auto& u : upd_iv)
            if (r.first <= u.second && u.first <= r.second) { overlap_reads++; break; }
    }
    for (auto& u : upd_iv) {
        for (auto& r : read_iv)
            if (u.first <= r.second && r.first <= u.second) { overlapping_updates++; break; }
    }

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
       << " run_s=" << run_s << " warm_s=" << warm_s << " change_us=" << change_us
       << " p50=" << p50 << " p95=" << p95 << " p99=" << p99
       << " updates=" << upd << " granted=" << granted_total.load() << " revoked=" << revoked_total.load()
       << " overlap_reads=" << overlap_reads << " overlapping_updates=" << overlapping_updates
       << " leaked=" << leaked.load()
       << " missing=" << missing.load() << " truth_checked=" << checked.load()
       << " recall=" << (checked.load() ? 1.0 - (double)missing.load() / checked.load() : 0.0) << "\n";
    std::printf("%s", ("wrote " + outdir + "/summary.txt\n").c_str());
    return 0;
}
