// Veda-style access-aware lattice baseline (our reimplementation, not the
// authors' code). The published design materializes index nodes over role
// combinations and answers a query from the smallest covering node plus an
// exact scan of the leftover; a greedy pass chooses nodes under a storage
// budget.
//
// This file implements the part that matters for a fair comparison:
//   - atoms: documents grouped by their exact role-combination signature,
//     so blocks are exclusive and carry no impurity;
//   - candidate nodes: the frequent signatures plus the head singletons;
//   - greedy selection by value/cost under a memory budget (fraction of the
//     corpus), value = queries served x coverage of the node for that role;
//   - per-node HNSW; a role queries the covering node with the largest doc
//     set and exact-scans the leftover V(r) \ docs(node);
//   - truth is exact top-k over V(r), so missing/leakage are directly
//     comparable to the lease arms of churn_arms.cpp.
//
// Build: g++ -O3 -march=native -std=c++17 -I/tmp veda_lattice.cpp -o veda_lattice -pthread
// Run:   veda_lattice base.f32 queries.f32 roles.bin qroles.bin n dim nq k budget seed outdir [mfac=15]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <queue>
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
    const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * v.size()));
    return v[i];
}

static std::vector<std::pair<float, int>> exact_topk(const float* base, int dim,
                                   const std::vector<int>& docs,
                                   const float* q, int k) {
    std::vector<std::pair<float, int>> d;
    d.reserve(docs.size());
    for (int id : docs) {
        const float* v = base + static_cast<size_t>(id) * dim;
        float s = 0.f;
        for (int j = 0; j < dim; ++j) { const float t = v[j] - q[j]; s += t * t; }
        d.emplace_back(s, id);
    }
    const size_t kk = std::min<size_t>(k, d.size());
    std::partial_sort(d.begin(), d.begin() + kk, d.end());
    return std::vector<std::pair<float, int>>(d.begin(), d.begin() + kk);
}

int main(int argc, char** argv) {
    if (argc < 12) {
        std::cerr << "usage: veda_lattice base queries roles qroles n dim nq k budget seed outdir [mfac]\n";
        return 2;
    }
    const std::string base_path = argv[1], query_path = argv[2];
    const std::string roles_path = argv[3], qroles_path = argv[4];
    const int n = std::stoi(argv[5]), dim = std::stoi(argv[6]);
    int nq = std::stoi(argv[7]);
    const int k = std::stoi(argv[8]);
    const double budget = std::stod(argv[9]);      // fraction of the corpus
    const int seed = std::stoi(argv[10]);
    const std::string outdir = argv[11];
    const int mfac = (argc > 12) ? std::stoi(argv[12]) : 15;
    (void)seed;

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

    std::vector<int> qcount(n_roles, 0);
    for (int r : qroles) qcount[r]++;

    // ---- documents -> role signature (sorted role list)
    std::vector<std::vector<int>> doc_roles(n);
    for (int r = 0; r < n_roles; ++r)
        for (int d : ids[r]) doc_roles[d].push_back(r);

    // ---- atoms: identical signatures grouped
    std::unordered_map<std::string, std::vector<int>> atoms;
    for (int d = 0; d < n; ++d) {
        if (doc_roles[d].empty()) continue;
        std::string key(reinterpret_cast<const char*>(doc_roles[d].data()),
                        doc_roles[d].size() * sizeof(int));
        atoms[key].push_back(d);
    }

    // ---- candidate nodes: frequent non-singleton signatures + head singletons
    struct Node { std::vector<int> roles; std::vector<int> docs; double value; };
    std::vector<Node> cand;
    std::vector<std::pair<size_t, std::string>> by_size;
    for (auto& kv : atoms) by_size.emplace_back(kv.second.size(), kv.first);
    std::sort(by_size.rbegin(), by_size.rend());

    std::vector<std::string> keys;
    for (size_t i = 0; i < by_size.size() && keys.size() < 60; ++i) {
        std::vector<int> rs(by_size[i].second.size() / sizeof(int));
        std::memcpy(rs.data(), by_size[i].second.data(), by_size[i].second.size());
        std::sort(rs.begin(), rs.end());
        if (rs.size() < 2 || rs.size() > 4) continue;
        bool any_query = false;
        for (int r : rs) if (qcount[r] > 0) any_query = true;
        if (!any_query) continue;
        keys.push_back(by_size[i].second);
    }
    std::vector<int> head;
    {
        std::vector<int> order(n_roles);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return qcount[a] > qcount[b]; });
        for (int i = 0; i < 10 && i < n_roles; ++i) if (qcount[order[i]] > 0) head.push_back(order[i]);
    }

    auto docs_of = [&](const std::vector<int>& rs) {
        std::vector<int> out;
        for (auto& kv : atoms) {
            std::vector<int> sig(kv.first.size() / sizeof(int));
            std::memcpy(sig.data(), kv.first.data(), kv.first.size());
            bool sup = true;
            for (int r : rs) if (!std::binary_search(sig.begin(), sig.end(), r)) { sup = false; break; }
            if (sup) out.insert(out.end(), kv.second.begin(), kv.second.end());
        }
        return out;
    };

    for (const auto& key : keys) {
        std::vector<int> rs(key.size() / sizeof(int));
        std::memcpy(rs.data(), key.data(), key.size());
        std::sort(rs.begin(), rs.end());
        Node nd; nd.roles = rs; nd.docs = docs_of(rs);
        if (nd.docs.empty()) continue;
        double v = 0;
        for (int r : rs) {
            const double cov = static_cast<double>(nd.docs.size()) / std::max<size_t>(1, ids[r].size());
            v += qcount[r] * std::min(1.0, cov);
        }
        nd.value = v;
        cand.push_back(std::move(nd));
    }
    for (int r : head) {
        Node nd; nd.roles = {r}; nd.docs = ids[r];
        nd.value = qcount[r];
        cand.push_back(std::move(nd));
    }

    // ---- greedy under the memory budget
    const size_t cap = static_cast<size_t>(budget * n);
    std::vector<char> used(cand.size(), 0);
    std::vector<int> chosen;
    size_t mem = 0;
    while (true) {
        int best = -1; double best_ratio = 0;
        for (size_t i = 0; i < cand.size(); ++i) {
            if (used[i]) continue;
            if (mem + cand[i].docs.size() > cap) continue;
            const double ratio = cand[i].value / static_cast<double>(cand[i].docs.size());
            if (ratio > best_ratio) { best_ratio = ratio; best = static_cast<int>(i); }
        }
        if (best < 0) break;
        used[best] = 1; chosen.push_back(best); mem += cand[best].docs.size();
    }

    // ---- build one HNSW per chosen node
    hnswlib::L2Space space(dim);
    std::vector<hnswlib::HierarchicalNSW<float>*> idx;
    const auto t0 = Clock::now();
    for (int ci : chosen) {
        auto* h = new hnswlib::HierarchicalNSW<float>(&space, cand[ci].docs.size() + 10, 16, 100);
        for (int d : cand[ci].docs) h->addPoint(base + static_cast<size_t>(d) * dim, d);
        idx.push_back(h);
    }
    const double build_s = std::chrono::duration<double>(Clock::now() - t0).count();

    // ---- role -> best covering node (largest doc set among chosen nodes)
    std::vector<int> best_node(n_roles, -1);
    for (size_t j = 0; j < chosen.size(); ++j) {
        for (int r : cand[chosen[j]].roles)
            if (best_node[r] < 0 || cand[chosen[j]].docs.size() > cand[chosen[best_node[r]]].docs.size())
                best_node[r] = static_cast<int>(j);
    }

    // ---- queries
    std::vector<double> lat(nq, 0.0);
    long leaks = 0, missing = 0;
    for (int q = 0; q < nq; ++q) {
        const int r = qroles[q];
        const float* qv = queries + static_cast<size_t>(q) * dim;
        const auto t = Clock::now();
        std::vector<std::pair<float, int>> got;   // (distance, id), merged by distance
        const int j = best_node[r];
        std::vector<char> in_node(n, 0);
        if (j >= 0) {
            auto* h = idx[j];
            const size_t want = std::min<size_t>(static_cast<size_t>(k) * mfac, cand[chosen[j]].docs.size());
            auto res = h->searchKnn(qv, want);
            std::unordered_set<int> vis(ids[r].begin(), ids[r].end());
            while (!res.empty()) {
                const auto pr = res.top();
                res.pop();
                if (vis.count(static_cast<int>(pr.second)))
                    got.emplace_back(pr.first, static_cast<int>(pr.second));
            }
            for (int d : cand[chosen[j]].docs) in_node[d] = 1;
        }
        std::vector<int> leftover;
        leftover.reserve(ids[r].size());
        for (int d : ids[r]) if (!in_node[d]) leftover.push_back(d);
        auto tail = exact_topk(base, dim, leftover, qv, k);
        got.insert(got.end(), tail.begin(), tail.end());
        std::sort(got.begin(), got.end());                  // by distance
        std::vector<int> top;
        std::unordered_set<int> seen;
        for (const auto& pr : got) {
            if (seen.insert(pr.second).second) top.push_back(pr.second);
            if (static_cast<int>(top.size()) == k) break;
        }
        got.clear();
        const std::vector<int>& got_ids = top;
        lat[q] = std::chrono::duration<double, std::milli>(Clock::now() - t).count();

        std::vector<int> truth;
        for (const auto& pr : exact_topk(base, dim, ids[r], qv, k)) truth.push_back(pr.second);
        std::unordered_set<int> gs(got_ids.begin(), got_ids.end()), vs(ids[r].begin(), ids[r].end());
        for (int d : got_ids) if (!vs.count(d)) ++leaks;
        for (int d : truth) if (!gs.count(d)) ++missing;
    }

    std::ofstream sm(outdir + "/summary.txt");
    sm << "mode=veda_lattice n=" << n << " dim=" << dim << " nq=" << nq << " k=" << k
       << " budget=" << budget << " mfac=" << mfac
       << " nodes=" << chosen.size()
       << " build_s=" << build_s
       << " p50=" << pctl(lat, 0.50) << " p95=" << pctl(lat, 0.95)
       << " leaks=" << leaks << " missing=" << missing
       << " memory=" << static_cast<double>(mem) / n << "\n";
    std::cout << "nodes=" << chosen.size() << " build_s=" << build_s
              << " p50=" << pctl(lat, 0.50) << " p95=" << pctl(lat, 0.95)
              << " leaks=" << leaks << " missing=" << missing
              << " memory=" << static_cast<double>(mem) / n << "\n";
    return 0;
}
