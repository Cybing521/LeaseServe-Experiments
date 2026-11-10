// ACORN baseline driver for the authorization-filtered vector search comparison.
//
// Reads the same binary inputs as the exact-scan binaries
// (base.f32, queries.f32, roles.bin, qroles.bin), builds one ACORN index over
// the whole corpus, and answers each query with the querying role's visible set
// as the predicate (filter_id_map). Reports per-query latency and recall@k
// against an exact scan of the same visible set, plus index RSS.
//
// Build (static faiss from the ACORN fork):
//   g++ -O3 -march=native -std=c++17 -I<acorn>/ -I<acorn>/build \
//       acorn_acl.cpp <acorn>/build/faiss/libfaiss.a -fopenmp -lblas -llapack -o acorn_acl
//
// Run:
//   ./acorn_acl base.f32 queries.f32 roles.bin qroles.bin n d nq k M gamma M_beta ef outdir

#include <faiss/IndexACORN.h>
#include <faiss/impl/ACORN.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

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

static double pctl(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    double i = (v.size() - 1) * q / 100.0;
    size_t lo = static_cast<size_t>(i);
    size_t hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (i - lo);
}

int main(int argc, char** argv) {
    if (argc != 14) {
        std::cerr << "usage: base.f32 queries.f32 roles.bin qroles.bin n d nq k "
                     "M gamma M_beta ef outdir\n";
        return 2;
    }
    const std::string base_path = argv[1], query_path = argv[2];
    const std::string roles_path = argv[3], qroles_path = argv[4];
    const int n = std::stoi(argv[5]), d = std::stoi(argv[6]);
    const int nq = std::stoi(argv[7]), k = std::stoi(argv[8]);
    const int M = std::stoi(argv[9]), gamma = std::stoi(argv[10]);
    const int M_beta = std::stoi(argv[11]);
    // ef may be a comma-separated list; the index is built once and searched per ef.
    std::vector<int> efs;
    {
        std::string s = argv[12];
        size_t p = 0;
        while (p <= s.size()) {
            size_t c = s.find(',', p);
            if (c == std::string::npos) c = s.size();
            if (c > p) efs.push_back(std::stoi(s.substr(p, c - p)));
            p = c + 1;
        }
    }
    const std::string out = argv[13];

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
    // CHANGES_IN applies the lease's change stream to this static ACL, so the
    // predicate and the ground truth below come from one post-change snapshot.
    if (const char* ci = std::getenv("CHANGES_IN")) {
        std::ifstream in(ci);
        if (!in) { std::cerr << "cannot open " << ci << "\n"; std::exit(1); }
        unsigned long long ep; std::string kind; int role, doc; long applied = 0;
        while (in >> ep >> kind >> role >> doc) {
            if (role < 0 || role >= n_roles) continue;
            auto& v = ids[role];
            auto it = std::find(v.begin(), v.end(), doc);
            if (kind == "grant") {
                if (it == v.end()) v.push_back(doc);
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
    std::vector<int32_t> qroles(nq);
    std::memcpy(qroles.data(), qroles_b.data() + off, nq * sizeof(int32_t));

    // Build one predicate-agnostic index over the whole corpus.
    std::vector<int> metadata(static_cast<size_t>(n), 0);
    faiss::IndexACORNFlat index(d, M, gamma, metadata, M_beta);
    auto tb0 = Clock::now();
    index.add(n, base);
    auto tb1 = Clock::now();
    const double build_s =
        std::chrono::duration<double>(tb1 - tb0).count();

    double vm = 0.0;
    {
        std::ifstream st("/proc/self/status");
        std::string line;
        while (std::getline(st, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                vm = std::atof(line.c_str() + 6) / 1024.0;  // MB
            }
        }
    }

    std::vector<float> dist(static_cast<size_t>(k));
    std::vector<faiss::idx_t> labels(static_cast<size_t>(k));
    std::vector<char> filt(static_cast<size_t>(n), 0);
    std::vector<std::pair<float, int>> brute;

    // Ground truth: exact top-k over each query's visible set, computed once.
    std::vector<std::vector<int>> truths(static_cast<size_t>(nq));
    for (int q = 0; q < nq; ++q) {
        const auto& vis = ids[qroles[q]];
        if (vis.empty()) continue;
        const float* qv = queries + static_cast<size_t>(q) * d;
        brute.clear();
        brute.reserve(vis.size());
        for (int id : vis) {
            const float* v = base + static_cast<size_t>(id) * d;
            float s = 0.0f;
            for (int j = 0; j < d; ++j) {
                const float t = v[j] - qv[j];
                s += t * t;
            }
            brute.emplace_back(s, id);
        }
        const int kk = std::min(k, static_cast<int>(brute.size()));
        std::partial_sort(brute.begin(), brute.begin() + kk, brute.end());
        for (int i = 0; i < kk; ++i) truths[q].push_back(brute[i].second);
    }

    std::ofstream sm(out + "/summary.txt");
    for (int ef : efs) {
        // Set efSearch on the index itself: passing it through
        // SearchParameters relies on a dynamic_cast that silently falls back
        // to the default (16) in this build.
        index.acorn.efSearch = ef;

        std::vector<double> ms;
        ms.reserve(nq);
        long long hits = 0, total = 0, leaked = 0;

        // Warmup pass: touch every query once so the timed pass does not pay
        // for page faults and cold graph levels.
        for (int q = 0; q < nq; ++q) {
            const auto& vis = ids[qroles[q]];
            if (vis.empty()) continue;
            std::fill(filt.begin(), filt.end(), 0);
            for (int id : vis) filt[static_cast<size_t>(id)] = 1;
            const float* qv = queries + static_cast<size_t>(q) * d;
            index.search(1, qv, k, dist.data(), labels.data(), filt.data(), nullptr);
        }

        for (int q = 0; q < nq; ++q) {
            const auto& vis = ids[qroles[q]];
            if (vis.empty()) continue;
            std::fill(filt.begin(), filt.end(), 0);
            for (int id : vis) filt[static_cast<size_t>(id)] = 1;
            const float* qv = queries + static_cast<size_t>(q) * d;

            auto t0 = Clock::now();
            index.search(1, qv, k, dist.data(), labels.data(), filt.data(), nullptr);
            auto t1 = Clock::now();
            ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());

            for (int i = 0; i < k; ++i) {
                const int got = static_cast<int>(labels[i]);
                if (got < 0) continue;
                total++;
                if (!filt[static_cast<size_t>(got)]) leaked++;
                const auto& tr = truths[q];
                if (std::find(tr.begin(), tr.end(), got) != tr.end()) hits++;
            }
        }

        const double recall = total ? static_cast<double>(hits) / total : 0.0;
        sm << "n=" << n << " d=" << d << " nq=" << nq << " k=" << k
           << " M=" << M << " gamma=" << gamma << " M_beta=" << M_beta
           << " ef=" << ef << " build_s=" << build_s
           << " p50=" << pctl(ms, 50) << " p95=" << pctl(ms, 95)
           << " recall=" << recall << " leak=" << leaked
           << " rss_mb=" << vm << " roles=" << n_roles << "\n";
        sm.flush();
        std::cout << "ef=" << ef << " p50=" << pctl(ms, 50)
                  << " p95=" << pctl(ms, 95) << " recall=" << recall
                  << " leak=" << leaked << " rss_mb=" << vm
                  << " build_s=" << build_s << "\n";
    }
    return 0;
}
