// E1 -- the minimal authorization counterexample, on purpose-built queries.
//
// For each query we look up the global nearest neighbour of the query vector.
// If that document is *outside* the querying role, we grant it and read at the
// pin immediately after the grant. That document is now the exact top-1 of the
// authorized set, and it has no candidate path in the graph built before the
// grant -- the one situation where "the filter makes stale indexes safe" is not
// enough.
//
// Four predicates read the same state:
//   no-cert      graph only, no certificate, no log      (the natural baseline)
//   cert-no-log  interval [a,omega] holds but there is no log
//   cert-log     interval plus the grant log             (the mechanism)
//   version-eq   pin == a only, otherwise exact fallback.  The fixture reads at pin 2 with a = 1,
//                so this arm falls back on every constructed query and the pin == a branch is
//                never exercised here.
//
// Reported per predicate: auth miss (the granted doc is the exact top-1 and is
// not returned), fallback share, p50, and leaks.
//
// Build: g++ -O3 -march=native -std=c++17 -I<hnswlib> e1_auth_miss.cpp -o e1 -pthread
// Run:   e1 base.f32 queries.f32 roles.bin qroles.bin n dim nq k K outdir [mfac]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hnswlib/hnswlib.h"

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

static float l2(const float* a, const float* b, int d) {
    float s = 0.f;
    for (int j = 0; j < d; ++j) { const float t = a[j] - b[j]; s += t * t; }
    return s;
}

int main(int argc, char** argv) {
    if (argc < 12) {
        std::cerr << "usage: e1 base queries roles qroles n dim nq k K outdir [mfac]\n";
        return 2;
    }
    const std::string base_path = argv[1], query_path = argv[2];
    const std::string roles_path = argv[3], qroles_path = argv[4];
    const int n = std::stoi(argv[5]), dim = std::stoi(argv[6]);
    int nq = std::stoi(argv[7]);
    const int k = std::stoi(argv[8]), K = std::stoi(argv[9]);
    const std::string outdir = argv[10];
    const int mfac = (argc > 11) ? std::stoi(argv[11]) : 15;

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
    std::unordered_set<int> promoted(ranked.begin(), ranked.begin() + std::min<int>(K, n_roles));

    // one global graph, used only to pick the global nearest neighbour per query
    hnswlib::L2Space space(dim);
    auto* g = new hnswlib::HierarchicalNSW<float>(&space, n, 16, 100);
    for (int d = 0; d < n; ++d) g->addPoint(base + static_cast<size_t>(d) * dim, d);
    g->setEf(64);

    struct Role {
        std::vector<int> docs;
        std::unordered_set<int> vis;      // visible at the pre-grant snapshot
        hnswlib::HierarchicalNSW<float>* h = nullptr;
    };
    std::unordered_map<int, Role> roles;
    for (int r : promoted) {
        Role R;
        R.docs = ids[r];
        for (int d : R.docs) R.vis.insert(d);
        R.h = new hnswlib::HierarchicalNSW<float>(&space, R.docs.size() + 8, 16, 100);
        for (int d : R.docs) R.h->addPoint(base + static_cast<size_t>(d) * dim, d);
        roles[r] = std::move(*std::make_unique<Role>(std::move(R)));
    }

    // The fifth entry is not a predicate but a control: the same version-equality
    // rule read at pin == a, where it is allowed to use the graph.  It shows that
    // the predicate has a passing branch; the other four read at pin = 1 > a.
    const char* names[5] = {"no-cert", "cert-no-log", "cert+log", "version-eq",
                            "version-eq@pin=a"};
    long auth_miss[5] = {0, 0, 0, 0, 0}, falls[5] = {0, 0, 0, 0, 0}, leaks[5] = {0, 0, 0, 0, 0};
    long short_count[5] = {0, 0, 0, 0, 0}, returned_sum[5] = {0, 0, 0, 0, 0};
    long min_len[5] = {1000000, 1000000, 1000000, 1000000, 1000000}, max_len[5] = {0, 0, 0, 0, 0};
    long pin_a_reads = 0, pin_a_exact = 0, pin_a_short = 0;
    long constructed = 0, top1_valid = 0;

    std::vector<double> lat[5];

    for (int q = 0; q < nq; ++q) {
        const int r = qroles[q];
        auto it = roles.find(r);
        if (it == roles.end()) continue;
        Role& R = it->second;
        const float* qv = queries + static_cast<size_t>(q) * dim;

        // global nearest neighbour: the document we will grant
        auto res = g->searchKnn(qv, 1);
        const int target = static_cast<int>(res.top().second);
        if (R.vis.count(target)) continue;              // already visible: not a granting test

        const uint64_t a = 0, omega = 1;                 // one grant at epoch 1
        std::vector<int> truth;                          // exact top-k of V(r, 1)
        {
            std::vector<std::pair<float, int>> d;
            d.reserve(R.docs.size() + 1);
            for (int doc : R.docs) d.emplace_back(l2(base + static_cast<size_t>(doc) * dim, qv, dim), doc);
            d.emplace_back(l2(base + static_cast<size_t>(target) * dim, qv, dim), target);
            const size_t kk = std::min<size_t>(k, d.size());
            std::partial_sort(d.begin(), d.begin() + kk, d.end());
            for (size_t i = 0; i < kk; ++i) truth.push_back(d[i].second);
        }
        const bool target_is_top1 = !truth.empty() && truth[0] == target;
        ++constructed;
        if (target_is_top1) ++top1_valid;

        // Rotate the arm order by query: with a fixed order the first arm paid the
        // cold cache and read 2-3x slower than arms doing strictly more work.
        for (int slot = 0; slot < 4; ++slot) {
            const int arm = (q + slot) % 4;
            const auto t = Clock::now();
            std::vector<int> got;
            bool fallback = false;
            if (arm == 0) {                              // no certificate, no log
                auto rs = R.h->searchKnn(qv, static_cast<size_t>(k) * mfac);
                std::vector<std::pair<float, int>> c;
                while (!rs.empty()) { c.emplace_back(rs.top().first, (int)rs.top().second); rs.pop(); }
                std::sort(c.begin(), c.end());
                for (auto& p : c) {
                    if (static_cast<int>(got.size()) >= k) break;
                    if (R.vis.count(p.second) || p.second == target) got.push_back(p.second);
                }
            } else if (arm == 1) {                       // interval holds, no log
                auto rs = R.h->searchKnn(qv, static_cast<size_t>(k) * mfac);
                std::vector<std::pair<float, int>> c;
                while (!rs.empty()) { c.emplace_back(rs.top().first, (int)rs.top().second); rs.pop(); }
                std::sort(c.begin(), c.end());
                for (auto& p : c) {
                    if (static_cast<int>(got.size()) >= k) break;
                    if (R.vis.count(p.second)) got.push_back(p.second);   // target not searchable
                }
            } else if (arm == 2) {                       // interval plus the grant log
                std::vector<std::pair<float, int>> c;
                auto rs = R.h->searchKnn(qv, static_cast<size_t>(k) * mfac);
                while (!rs.empty()) { c.emplace_back(rs.top().first, (int)rs.top().second); rs.pop(); }
                c.emplace_back(l2(base + static_cast<size_t>(target) * dim, qv, dim), target);
                std::sort(c.begin(), c.end());
                for (auto& p : c) {
                    if (static_cast<int>(got.size()) >= k) break;
                    if (R.vis.count(p.second) || p.second == target) got.push_back(p.second);
                }
            } else {                                     // version equality: pin != a -> exact
                fallback = true;
                std::vector<std::pair<float, int>> d;
                d.reserve(R.docs.size() + 1);
                for (int doc : R.docs)
                    d.emplace_back(l2(base + static_cast<size_t>(doc) * dim, qv, dim), doc);
                d.emplace_back(l2(base + static_cast<size_t>(target) * dim, qv, dim), target);
                const size_t kk = std::min<size_t>(k, d.size());
                std::partial_sort(d.begin(), d.begin() + kk, d.end());
                for (size_t i = 0; i < kk; ++i) got.push_back(d[i].second);
            }
            lat[arm].push_back(std::chrono::duration<double, std::milli>(Clock::now() - t).count());
            const int len = static_cast<int>(got.size());
            short_count[arm] += (len < k);
            returned_sum[arm] += len;
            if (len < min_len[arm]) min_len[arm] = len;
            if (len > max_len[arm]) max_len[arm] = len;
            std::unordered_set<int> gs(got.begin(), got.end());
            if (target_is_top1 && !gs.count(target)) auth_miss[arm]++;
            if (fallback) falls[arm]++;
            for (int d : got) if (!R.vis.count(d) && d != target) leaks[arm]++;
        }

        // Control: version equality at pin == a.  V(r,0) = R.docs, the graph holds
        // exactly that set, so the graph branch is legal and must be exact.
        {
            const auto t = Clock::now();
            std::vector<int> got;
            auto rs = R.h->searchKnn(qv, static_cast<size_t>(k) * mfac);
            std::vector<std::pair<float, int>> c;
            while (!rs.empty()) { c.emplace_back(rs.top().first, (int)rs.top().second); rs.pop(); }
            std::sort(c.begin(), c.end());
            for (auto& p : c) {
                if (static_cast<int>(got.size()) >= k) break;
                if (R.vis.count(p.second)) got.push_back(p.second);
            }
            lat[4].push_back(std::chrono::duration<double, std::milli>(Clock::now() - t).count());
            ++pin_a_reads;
            if (static_cast<int>(got.size()) < k) ++pin_a_short;
            std::vector<std::pair<float, int>> d0;
            d0.reserve(R.docs.size());
            for (int doc : R.docs) d0.emplace_back(l2(base + static_cast<size_t>(doc) * dim, qv, dim), doc);
            const size_t kk0 = std::min<size_t>(k, d0.size());
            std::partial_sort(d0.begin(), d0.begin() + kk0, d0.end());
            std::unordered_set<int> gs0(got.begin(), got.end());
            bool same = static_cast<size_t>(got.size()) == kk0;
            for (size_t i = 0; same && i < kk0; ++i) same = gs0.count(d0[i].second) > 0;
            if (same) ++pin_a_exact;
            const int len = static_cast<int>(got.size());
            short_count[4] += (len < k);
            returned_sum[4] += len;
            if (len < min_len[4]) min_len[4] = len;
            if (len > max_len[4]) max_len[4] = len;
            for (int d : got) if (!R.vis.count(d)) leaks[4]++;
        }
    }

    std::ofstream sm(outdir + "/summary.txt");
    sm << "e1 n=" << n << " nq=" << nq << " constructed=" << constructed << " top1_valid=" << top1_valid << " k=" << k << " K=" << K << " mfac=" << mfac << "\n";
    sm << "pin_eq_a reads=" << pin_a_reads << " exact_topk=" << pin_a_exact
       << " short=" << pin_a_short << "\n";
    for (int arm = 0; arm < 5; ++arm) {
        sm << names[arm] << " auth_miss=" << auth_miss[arm]
           << " short=" << short_count[arm] << " min_len=" << min_len[arm]
           << " max_len=" << max_len[arm] << " returned=" << returned_sum[arm]
           << " fallback=" << falls[arm] << " leaks=" << leaks[arm]
           << " p50=" << pctl(lat[arm], 0.50) << " p95=" << pctl(lat[arm], 0.95) << "\n";
        std::cout << names[arm] << " auth_miss=" << auth_miss[arm]
                  << " fallback=" << falls[arm] << " leaks=" << leaks[arm]
                  << " p50=" << pctl(lat[arm], 0.50) << "\n";
    }
    return 0;
}
