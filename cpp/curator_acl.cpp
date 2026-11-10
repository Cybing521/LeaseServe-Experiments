// Curator baseline driver for the authorization-filtered comparison.
//
// One shared hierarchical index over the whole corpus; each document is granted
// to the roles that can see it (Curator allows a vector to belong to several
// tenants), and each query searches inside its role's tenant id. This is the
// "shared structure with per-label sub-indexes" baseline: it answers the
// question of whether per-role materialization (the lease) is worth its memory
// over a shared tree that keeps an access map.
//
// Reports per-query latency, recall@k against an exact scan of the same visible
// set, and index RSS.
//
// Build:
//   g++ -O3 -march=native -std=c++17 -I<curator>/3rd_party/faiss \
//       curator_acl.cpp <curator>/3rd_party/faiss/build/faiss/libfaiss.a \
//       -fopenmp -lblas -llapack -o curator_acl
// Run:
//   curator_acl base.f32 queries.f32 roles.bin qroles.bin n d nq k nlist nprobe outdir

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "faiss/MultiTenantIndexIVFHierarchical.h"

using Clock = std::chrono::high_resolution_clock;

static std::vector<char> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::cerr << "cannot open " << path << "\n"; std::exit(1); }
    f.seekg(0, std::ios::end);
    const auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<char> buf(sz);
    f.read(buf.data(), static_cast<std::streamsize>(sz));
    return buf;
}

template <typename T>
static void read_scalar(const std::vector<char>& b, size_t& off, T& out) {
    std::memcpy(&out, b.data() + off, sizeof(T));
    off += sizeof(T);
}

static double pctl(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(p * v.size()))];
}

static long rss_mb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmHWM:", 0) == 0)
            return std::stol(line.substr(6)) / 1024;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 12) {
        std::cerr << "usage: curator_acl base.f32 queries.f32 roles.bin qroles.bin "
                     "n d nq k nlist nprobe outdir [niter max_leaf]\n";
        return 2;
    }
    const int niter = (argc > 12) ? std::stoi(argv[12]) : 20;
    const int max_leaf = (argc > 13) ? std::stoi(argv[13]) : 128;
    const int search_ef = (argc > 14) ? std::stoi(argv[14]) : 100;
    const int n = std::stoi(argv[5]), d = std::stoi(argv[6]);
    int nq = std::stoi(argv[7]);
    const int k = std::stoi(argv[8]);
    const size_t nlist = static_cast<size_t>(std::stoul(argv[9]));
    const size_t nprobe = static_cast<size_t>(std::stoul(argv[10]));
    const std::string outdir = argv[11];

    auto base_b = read_file(argv[1]);
    auto q_b = read_file(argv[2]);
    auto roles_b = read_file(argv[3]);
    auto qroles_b = read_file(argv[4]);
    const float* base = reinterpret_cast<const float*>(base_b.data());
    const float* queries = reinterpret_cast<const float*>(q_b.data());

    size_t off = 0;
    int32_t n_roles = 0;
    read_scalar(roles_b, off, n_roles);
    std::vector<std::vector<int32_t>> ids(n_roles);
    for (int r = 0; r < n_roles; ++r) {
        int32_t rid, cnt;
        read_scalar(roles_b, off, rid);
        read_scalar(roles_b, off, cnt);
        ids[rid].resize(cnt);
        if (cnt) std::memcpy(ids[rid].data(), roles_b.data() + off, cnt * sizeof(int32_t));
        off += static_cast<size_t>(cnt) * sizeof(int32_t);
    }
    // CHANGES_IN applies the lease's change stream to this static ACL, so the
    // predicate and the ground truth below come from one post-change snapshot.
    if (const char* ci = std::getenv("CHANGES_IN")) {
        std::ifstream in(ci);
        if (!in) { std::cerr << "cannot open " << ci << "\n"; std::exit(1); }
        unsigned long long ep; std::string kind; int role, doc; long applied = 0;
        while (in >> ep >> kind >> role >> doc) {
            if (role < 0 || role >= n_roles) continue;
            auto& v = ids[role];
            auto it = std::find(v.begin(), v.end(), static_cast<int32_t>(doc));
            if (kind == "grant") {
                if (it == v.end()) v.push_back(static_cast<int32_t>(doc));
            } else if (it != v.end()) {
                v.erase(it);
            }
            ++applied;
        }
        std::cerr << "CHANGES_IN: applied " << applied << " changes from " << ci << "\n";
    }

    off = 0;
    int32_t nq_file = 0;
    read_scalar(qroles_b, off, nq_file);
    if (nq > nq_file) nq = nq_file;
    std::vector<int32_t> qroles(nq);
    std::memcpy(qroles.data(), qroles_b.data() + off, nq * sizeof(int32_t));

    faiss::MultiTenantIndexIVFHierarchical index(
            static_cast<size_t>(d), nlist, faiss::METRIC_L2,
            /*bf_capacity=*/1000, /*bf_false_pos=*/0.01, /*max_sl_size=*/128,
            /*clus_niter=*/static_cast<size_t>(niter),
            /*max_leaf_size=*/static_cast<size_t>(max_leaf), /*nprobe=*/nprobe,
            /*prune_thres=*/1.6f, /*variance_boost=*/0.4f, /*search_ef=*/static_cast<size_t>(search_ef),
            /*beam_size=*/2, /*use_temp_index_caching=*/false);

    auto tb0 = Clock::now();
    std::cerr << "[phase] train start n=" << n << std::endl;
    index.train(n, base, 0);
    std::cerr << "[phase] train done " << std::chrono::duration<double>(Clock::now()-tb0).count() << "s" << std::endl;
    std::vector<int64_t> labels(n);
    std::iota(labels.begin(), labels.end(), 0);
    auto ta0 = Clock::now();
    index.add_vector_with_ids(n, base, labels.data());
    std::cerr << "[phase] add done " << std::chrono::duration<double>(Clock::now()-ta0).count() << "s" << std::endl;
    auto tg0 = Clock::now();
    for (int r = 0; r < n_roles; ++r)
        for (int32_t doc : ids[r]) index.grant_access(doc, r);
    std::cerr << "[phase] grants done " << std::chrono::duration<double>(Clock::now()-tg0).count() << "s" << std::endl;
    const double build_s =
            std::chrono::duration<double>(Clock::now() - tb0).count();

    std::vector<double> ms;
    std::vector<double> ratio;
    long hits = 0, total = 0;
    for (int q = 0; q < nq; ++q) {
        const int r = qroles[q];
        const auto& vis = ids[r];
        if (vis.empty()) continue;
        const float* qv = queries + static_cast<size_t>(q) * d;

        // exact top-k over the visible set, the truth for recall
        std::vector<std::pair<float, int32_t>> truth;
        truth.reserve(vis.size());
        for (int32_t doc : vis) {
            float s = 0.f;
            const float* dv = base + static_cast<size_t>(doc) * d;
            for (int j = 0; j < d; ++j) { const float t = qv[j] - dv[j]; s += t * t; }
            truth.emplace_back(s, static_cast<int32_t>(doc));
        }
        const size_t kk = std::min<size_t>(k, truth.size());
        std::partial_sort(truth.begin(), truth.begin() + kk, truth.end());

        std::vector<float> dist(k, 0.f);
        std::vector<faiss::idx_t> got(k, -1);
        auto t0 = Clock::now();
        index.search(1, qv, k, r, dist.data(), got.data());
        auto t1 = Clock::now();
        ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());

        for (size_t i = 0; i < kk; ++i) {
            ++total;
            for (int j = 0; j < k; ++j)
                if (got[j] == truth[i].second) { ++hits; break; }
        }
    }

    std::filesystem::create_directories(outdir);
    std::ofstream sm(outdir + "/summary.txt");
    sm << "curator n=" << n << " d=" << d << " nq=" << nq << " k=" << k
       << " nlist=" << nlist << " nprobe=" << nprobe
       << " p50=" << pctl(ms, 0.50) << " p95=" << pctl(ms, 0.95)
       << " recall=" << (total ? static_cast<double>(hits) / total : 0.0)
       << " rss_mb=" << rss_mb() << " build_s=" << build_s << "\n";
    std::cout << "p50=" << pctl(ms, 0.50) << " p95=" << pctl(ms, 0.95)
              << " recall=" << (total ? static_cast<double>(hits) / total : 0.0)
              << " rss_mb=" << rss_mb() << " build_s=" << build_s << "\n";
    return 0;
}
