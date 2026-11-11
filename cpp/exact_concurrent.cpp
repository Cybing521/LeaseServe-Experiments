// Multi-threaded exact scan (gather base[ids] per query), scalar kernel.
//
// This is the C++ counterpart of D30: it answers whether the exact path is
// CPU-bound or Python-bound when 4 readers run concurrently.  Threads split
// queries, not vectors, so the reader count changes throughput and not
// per-query latency; the scalar/AVX-512 gap is the instruction set.
//
// Build:
//   g++ -O3 -march=native -fopenmp -std=c++17 exact_concurrent.cpp -o exact_concurrent
// Run:
//   OMP_NUM_THREADS=4 exact_concurrent base.f32 queries.f32 roles.bin qroles.bin \
//     n dim nq k outdir
// Output: exact_ms.bin (nq double), summary.txt (mean_exact_ms)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

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

int main(int argc, char** argv) {
    if (argc != 10) {
        std::cerr << "usage: exact_concurrent base.f32 queries.f32 roles.bin qroles.bin "
                     "n dim nq k outdir\n";
        return 2;
    }
    const std::string base_path = argv[1], query_path = argv[2];
    const std::string roles_path = argv[3], qroles_path = argv[4];
    const int n = std::stoi(argv[5]), dim = std::stoi(argv[6]);
    const int nq = std::stoi(argv[7]), k = std::stoi(argv[8]);
    const std::string outdir = argv[9];

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
    std::vector<int32_t> qroles(nq);
    std::memcpy(qroles.data(), qroles_b.data() + off, nq * sizeof(int32_t));

    std::vector<double> ms(nq, 0.0);
    volatile long long sink = 0;

#ifdef _OPENMP
#pragma omp parallel
#endif
    {
        std::vector<float> gmat;
        std::vector<std::pair<float, int>> buf;
        std::vector<int> out;
        long long local_sink = 0;
#ifdef _OPENMP
#pragma omp for schedule(dynamic, 1)
#endif
        for (int q = 0; q < nq; ++q) {
            const int r = qroles[q];
            const int m = static_cast<int>(ids[r].size());
            if (m == 0) continue;
            const float* qv = queries + static_cast<size_t>(q) * dim;
            gmat.resize(static_cast<size_t>(m) * dim);
            for (int i = 0; i < m; ++i) {
                std::memcpy(gmat.data() + static_cast<size_t>(i) * dim,
                            base + static_cast<size_t>(ids[r][i]) * dim,
                            dim * sizeof(float));
            }
            auto t0 = Clock::now();
            buf.resize(m);
            for (int i = 0; i < m; ++i) {
                const float* v = gmat.data() + static_cast<size_t>(i) * dim;
                float s = 0.f;
                for (int j = 0; j < dim; ++j) {
                    const float t = v[j] - qv[j];
                    s += t * t;
                }
                buf[i] = {s, ids[r][i]};
            }
            const int kk = std::min(k, m);
            if (kk < m) std::nth_element(buf.begin(), buf.begin() + kk, buf.end());
            std::sort(buf.begin(), buf.begin() + kk);
            out.resize(kk);
            for (int i = 0; i < kk; ++i) { out[i] = buf[i].second; local_sink += out[i]; }
            auto t1 = Clock::now();
            ms[q] = std::chrono::duration<double, std::milli>(t1 - t0).count();
        }
#ifdef _OPENMP
#pragma omp atomic
#endif
        sink += local_sink;
    }

    std::ofstream f(outdir + "/exact_ms.bin", std::ios::binary);
    f.write(reinterpret_cast<const char*>(ms.data()), ms.size() * sizeof(double));
    double mean = 0;
    for (double x : ms) mean += x;
    mean /= nq;
    std::ofstream sm(outdir + "/summary.txt");
    sm << "n=" << n << " dim=" << dim << " nq=" << nq << " k=" << k
       << " threads=" << omp_get_max_threads() << " mean_exact_ms=" << mean
       << " sink=" << sink << "\n";
    std::cout << "mean_exact_ms=" << mean << " threads=" << omp_get_max_threads()
              << " nq=" << nq << "\n";
    return 0;
}
