// Demote-always baseline (S2): no graph index.  It does keep a contiguous copy
// of each promoted role's visible vectors, and build_s/buffered_mb time and size
// that copy.
//
// Every read scans the role's *current* visible set exactly; a single writer
// applies grant/revoke changes to the same roles under a per-role lock.  This is
// the only other design that gives zero leakage AND zero missing results, so it
// is the fair denominator for the lease's completeness/latency trade-off.
//
// The protocol is deliberately identical to hnsw_lease_concurrent.cpp: same base,
// queries, role files and query-role file; same 4 readers + 1 writer; the writer
// issues `changes` updates 500 us apart, 50/50 grant/revoke, drawn from the same
// hot roles and with the same RNG stream (seed + 7).  Only the read path differs.
//
// Build:
//   g++ -O3 -march=native -std=c++17 exact_churn.cpp -o exact_churn
// Run:
//   exact_churn base queries roles qroles n dim nq k K changes threads seed outdir

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <cstdlib>
#include <mutex>
#include <random>
#include <shared_mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <immintrin.h>

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

// Same kernel as the lease harness, so the two arms price the same primitive.
static std::vector<int> exact_topk(const float* base, int dim,
                                   const std::vector<int>& ids,
                                   const float* qv, int k) {
    std::vector<std::pair<float, int>> buf(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        buf[i] = {l2_avx512(base + static_cast<size_t>(ids[i]) * dim, qv, dim), ids[i]};
    }
    const int kk = std::min<int>(k, static_cast<int>(buf.size()));
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

struct RoleState {
    std::vector<int> cur;          // doc ids, row i of buf
    std::vector<float> buf;        // contiguous copy of the visible vectors
    std::unordered_set<int> ever;  // never re-grant a revoked id
    std::shared_mutex mu;
};

int main(int argc, char** argv) {
    if (argc != 14) {
        std::cerr << "usage: exact_churn base queries roles qroles n dim nq k K changes"
                     " threads seed outdir\n";
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
    // WSLEEP: microseconds between changes (default 500). Used to match the
    // churn window to the phase length when comparing arms of different speed.
    const int wsleep = std::getenv("WSLEEP") ? std::atoi(std::getenv("WSLEEP")) : 500;

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

    // No index: the visible sets are the only state.  Build cost is zero.
    std::vector<RoleState> states(n_roles);
    const double b0 = 0.0;
    auto tg0 = Clock::now();
    for (int r = 0; r < n_roles; ++r) {
        states[r].cur = ids[r];
        states[r].ever.insert(ids[r].begin(), ids[r].end());
        if (counts[r] == 0) continue;  // only the queried roles carry a buffer
        states[r].buf.resize(ids[r].size() * static_cast<size_t>(dim));
        for (size_t i = 0; i < ids[r].size(); ++i) {
            std::memcpy(states[r].buf.data() + i * dim,
                        base + static_cast<size_t>(ids[r][i]) * dim, dim * sizeof(float));
        }
    }
    const double build_s = std::chrono::duration<double>(Clock::now() - tg0).count();
    (void)b0;
    size_t buffered_bytes = 0;
    for (int r = 0; r < n_roles; ++r) buffered_bytes += states[r].buf.size() * sizeof(float);

    auto query_one = [&](int q, std::vector<int>& got, std::vector<std::pair<float,int>>& scratch)
        -> double {
        const int r = qroles[q];
        const float* qv = queries + static_cast<size_t>(q) * dim;
        auto t = Clock::now();
        auto& st = states[r];
        std::shared_lock<std::shared_mutex> lk(st.mu);
        const int m = static_cast<int>(st.cur.size());
        scratch.resize(m);
        const float* buf = st.buf.data();
        for (int i = 0; i < m; ++i) {
            scratch[i] = {l2_avx512(buf + static_cast<size_t>(i) * dim, qv, dim), st.cur[i]};
        }
        const int kk = std::min(k, m);
        if (kk < m) std::nth_element(scratch.begin(), scratch.begin() + kk, scratch.end());
        std::sort(scratch.begin(), scratch.begin() + kk);
        got.resize(kk);
        for (int i = 0; i < kk; ++i) got[i] = scratch[i].second;
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };

    auto run_phase = [&](bool with_writer, std::vector<std::vector<int>>& gots) {
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
                    auto& st = states[r];
                    const bool grant = opd(rng);
                    std::unique_lock<std::shared_mutex> lk(st.mu);
                    if (!grant && !st.cur.empty()) {
                        std::uniform_int_distribution<int> pick(0, static_cast<int>(st.cur.size()) - 1);
                        const size_t pos = static_cast<size_t>(pick(rng));
                        const size_t last = st.cur.size() - 1;
                        if (pos != last) {
                            st.cur[pos] = st.cur[last];
                            std::memcpy(st.buf.data() + pos * dim, st.buf.data() + last * dim,
                                        dim * sizeof(float));
                        }
                        st.cur.pop_back();
                        st.buf.resize(st.cur.size() * static_cast<size_t>(dim));
                    } else if (grant) {
                        for (int tries = 0; tries < 100; ++tries) {
                            const int d = docd(rng);
                            if (st.ever.count(d)) continue;
                            st.cur.push_back(d);
                            st.buf.resize(st.cur.size() * static_cast<size_t>(dim));
                            std::memcpy(st.buf.data() + (st.cur.size() - 1) * dim,
                                        base + static_cast<size_t>(d) * dim, dim * sizeof(float));
                            st.ever.insert(d);
                            break;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(wsleep));
                }
            });
        }
        for (int t = 0; t < threads; ++t) {
            readers.emplace_back([&, t]() {
                while (!go.load()) {}
                std::vector<std::pair<float,int>> scratch;
                const int lo = t * nq / threads;
                const int hi = (t + 1) * nq / threads;
                for (int q = lo; q < hi; ++q) {
                    lat[q] = query_one(q, gots[q], scratch);
                    const int r = qroles[q];
                    auto& st = states[r];
                    std::shared_lock<std::shared_mutex> lk(st.mu);
                    for (int d : gots[q]) {
                        if (std::find(st.cur.begin(), st.cur.end(), d) == st.cur.end())
                            leaks.fetch_add(1);
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
    auto [lat_static, leaks_static, phase_static_s] = run_phase(false, gots_static);
    auto [lat_inc, leaks_inc, phase_inc_s] = run_phase(true, gots_inc);

    // Completeness is exact by construction; we still recompute it so the arm
    // carries the same field as the lease rows.
    int missing = 0;
    for (int q = 0; q < nq; ++q) {
        const int r = qroles[q];
        auto& st = states[r];
        std::shared_lock<std::shared_mutex> lk(st.mu);
        std::vector<std::pair<float,int>> scratch(st.cur.size());
        const int m = static_cast<int>(st.cur.size());
        const float* qv = queries + static_cast<size_t>(q) * dim;
        for (int i = 0; i < m; ++i)
            scratch[i] = {l2_avx512(st.buf.data() + static_cast<size_t>(i) * dim, qv, dim), st.cur[i]};
        const int kk = std::min(k, m);
        if (kk < m) std::nth_element(scratch.begin(), scratch.begin() + kk, scratch.end());
        std::sort(scratch.begin(), scratch.begin() + kk);
        std::vector<int> truth;
        for (int i = 0; i < kk; ++i) truth.push_back(scratch[i].second);
        std::unordered_set<int> gs(gots_inc[q].begin(), gots_inc[q].end());
        for (int d : truth) if (!gs.count(d)) missing++;
    }

    std::ofstream sm(outdir + "/summary.txt");
    sm << "n=" << n << " dim=" << dim << " nq=" << nq << " k=" << k << " K=" << K
       << " arm=exact_churn"
       << " changes=" << changes << " threads=" << threads
       << " build_s=" << build_s << " buffered_mb=" << (buffered_bytes >> 20)
       << " qps_static=" << (phase_static_s > 0 ? nq / phase_static_s : 0)
       << " qps_inc=" << (phase_inc_s > 0 ? nq / phase_inc_s : 0)
       << " static_p50=" << pctl(lat_static, 0.50) << " static_p95=" << pctl(lat_static, 0.95)
       << " incr_p50=" << pctl(lat_inc, 0.50) << " incr_p95=" << pctl(lat_inc, 0.95)
       << " static_leaks=" << leaks_static << " inc_leaks=" << leaks_inc
       << " inc_missing=" << missing << "\n";
    std::cout << "arm=exact_churn build_s=" << build_s
              << " static_p50=" << pctl(lat_static, 0.50)
              << " incr_p50=" << pctl(lat_inc, 0.50) << " incr_p95=" << pctl(lat_inc, 0.95)
              << " inc_leaks=" << leaks_inc << " inc_missing=" << missing << "\n";
    return 0;
}
