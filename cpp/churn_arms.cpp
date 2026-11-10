// Churn arms for the lease paper, one harness, four designs.
//
//   shared     one full-corpus HNSW + fresh ACL filter; zero maintenance
//   static     per-role head indexes, never repaired; filter at read time
//   lease      per-role head indexes, repaired on every change (addPoint/markDelete)
//   partition  co-access role groups, one index per group, repaired on change
//
// All arms see the same ACL, the same 200-change schedule (50/50 grant/revoke),
// the same queries and the same exact truth computed from the *current* role
// set. Churn is applied between the query passes (sequential), so the numbers
// are comparable across arms but are not the 4R+1W concurrent table.
//
// Build (hnswlib headers in /tmp):
//   g++ -O3 -march=native -std=c++17 -I/tmp churn_arms.cpp -o churn_arms -pthread
// Run:
//   churn_arms base.f32 queries.f32 roles.bin qroles.bin n dim nq k K changes seed outdir \
//              mode [mfac=15] [groups=8]
//
// Output: summary.txt with build_s, update_s, p50/p95, leaks, missing,
// memory multiplier and (partition) the group count.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <queue>
#include <random>
#include <string>
#include <thread>
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

static void build_parallel(hnswlib::HierarchicalNSW<float>* h,
                           const std::vector<int>& docs, const float* base,
                           int dim, int nthreads) {
    std::atomic<size_t> next{0};
    auto work = [&]() {
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= docs.size()) return;
            const int d = docs[i];
            h->addPoint(base + static_cast<size_t>(d) * dim, d);
        }
    };
    std::vector<std::thread> ts;
    for (int t = 0; t < nthreads; ++t) ts.emplace_back(work);
    for (auto& t : ts) t.join();
}

int main(int argc, char** argv) {
    if (argc < 14 || argc > 18) {
        std::cerr << "usage: churn_arms base queries roles qroles n dim nq k K changes seed "
                     "outdir mode [mfac=15] [groups=8] [budget] [repair_every]\n";
        return 2;
    }
    const std::string base_path = argv[1], query_path = argv[2];
    const std::string roles_path = argv[3], qroles_path = argv[4];
    const int n = std::stoi(argv[5]), dim = std::stoi(argv[6]);
    const int nq = std::stoi(argv[7]), k = std::stoi(argv[8]);
    const int K = std::stoi(argv[9]);
    const int changes = std::stoi(argv[10]);
    const int seed = std::stoi(argv[11]);
    const std::string outdir = argv[12];
    const std::string mode = argv[13];
    const int mfac = (argc >= 15) ? std::stoi(argv[14]) : 15;
    const int groups = (argc >= 16) ? std::stoi(argv[15]) : 8;
    const int budget_arg = (argc >= 17) ? std::stoi(argv[16]) : -1;
    const bool is_part = (mode == "partition" || mode == "partbudget_role" ||
                          mode == "partbudget_doc");
    const bool is_lease = (mode == "lease" || mode == "leasebudget_role" ||
                           mode == "leasebudget_doc");
    const bool budgeted = (mode == "partbudget_role" || mode == "partbudget_doc" ||
                           mode == "leasebudget_role" || mode == "leasebudget_doc");

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
    std::vector<int> qroles(nq);
    std::memcpy(qroles.data(), qroles_b.data() + off, nq * sizeof(int32_t));

    std::vector<int> counts(n_roles, 0);
    for (int r : qroles) counts[r]++;
    std::vector<int> ranked(n_roles);
    std::iota(ranked.begin(), ranked.end(), 0);
    std::sort(ranked.begin(), ranked.end(), [&](int a, int b) { return counts[a] > counts[b]; });
    std::unordered_set<int> promoted(ranked.begin(), ranked.begin() + K);
    const std::vector<int> promoted_list(promoted.begin(), promoted.end());

    // Live ACL state, mutated by the churn schedule.
    std::vector<std::unordered_set<int>> cur(n_roles);
    for (int r = 0; r < n_roles; ++r) cur[r] = std::unordered_set<int>(ids[r].begin(), ids[r].end());
    std::unordered_set<int> ever;
    for (int r : promoted) ever.insert(ids[r].begin(), ids[r].end());
    std::vector<std::unordered_set<int>> pending(n_roles);   // grants awaiting repair

    hnswlib::L2Space space(dim);
    std::unordered_map<int, hnswlib::HierarchicalNSW<float>*> per_role;  // static / lease
    std::unordered_map<int, hnswlib::HierarchicalNSW<float>*> per_group; // partition
    std::unordered_map<int, int> role_group;                             // role -> group
    hnswlib::HierarchicalNSW<float>* global = nullptr;
    std::vector<std::vector<int>> group_docs;
    std::vector<std::unordered_set<int>> gindex, gdead;                  // partition bookkeeping

    const int nthreads = 8;
    auto t0 = Clock::now();

    if (mode == "shared") {
        std::vector<int> all(n);
        std::iota(all.begin(), all.end(), 0);
        global = new hnswlib::HierarchicalNSW<float>(&space, n, 16, 100);
        build_parallel(global, all, base, dim, nthreads);
    } else if (mode == "static" || is_lease) {
        for (int r : promoted) {
            std::vector<int> docs(ids[r].begin(), ids[r].end());
            auto* h = new hnswlib::HierarchicalNSW<float>(&space, docs.size() + changes + 10, 16, 100);
            build_parallel(h, docs, base, dim, nthreads);
            per_role[r] = h;
        }
    } else if (is_part) {
        // Co-access score between role pairs from the inverted lists.
        std::vector<std::vector<int>> doc_roles(n);
        for (int r = 0; r < n_roles; ++r)
            for (int d : ids[r]) doc_roles[d].push_back(r);
        std::unordered_map<uint64_t, int> pair_score;
        for (int d = 0; d < n; ++d) {
            const auto& rs = doc_roles[d];
            for (size_t i = 0; i < rs.size(); ++i)
                for (size_t j = i + 1; j < rs.size(); ++j) {
                    const uint64_t key = (static_cast<uint64_t>(std::min(rs[i], rs[j])) << 32) |
                                         static_cast<uint32_t>(std::max(rs[i], rs[j]));
                    ++pair_score[key];
                }
        }
        const size_t cap = static_cast<size_t>(n) * 13 / (10 * groups);
        std::vector<std::unordered_set<int>> gset;
        std::vector<std::vector<int>> gmembers;
        std::vector<int> order(n_roles);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return ids[a].size() > ids[b].size(); });
        for (int r : order) {
            if (ids[r].empty()) continue;
            int best = -1;
            double best_score = -1.0;
            for (size_t g = 0; g < gset.size(); ++g) {
                double s = 0.0;
                for (int m : gmembers[g]) {
                    const uint64_t key = (static_cast<uint64_t>(std::min(r, m)) << 32) |
                                         static_cast<uint32_t>(std::max(r, m));
                    auto it = pair_score.find(key);
                    if (it != pair_score.end()) s += it->second;
                }
                size_t grow = 0;
                for (int d : ids[r]) if (!gset[g].count(d)) ++grow;
                if (gset[g].size() + grow > cap) continue;
                if (s > best_score) { best_score = s; best = static_cast<int>(g); }
            }
            if (best < 0) {
                gset.emplace_back();
                gmembers.emplace_back();
                best = static_cast<int>(gset.size()) - 1;
            }
            gset[best].insert(ids[r].begin(), ids[r].end());
            gmembers[best].push_back(r);
            role_group[r] = best;
        }
        group_docs.resize(gset.size());
        for (size_t g = 0; g < gset.size(); ++g)
            group_docs[g].assign(gset[g].begin(), gset[g].end());
        gindex = gset;
        gdead.resize(gset.size());
        for (size_t g = 0; g < group_docs.size(); ++g) {
            auto* h = new hnswlib::HierarchicalNSW<float>(&space, group_docs[g].size() + changes + 10, 16, 100);
            build_parallel(h, group_docs[g], base, dim, nthreads);
            per_group[static_cast<int>(g)] = h;
        }
    } else {
        std::cerr << "unknown mode " << mode << "\n";
        return 2;
    }
    const double build_s = std::chrono::duration<double>(Clock::now() - t0).count();

    // Churn: 50/50 grant/revoke on the hot roles, applied to the ACL and, for
    // the maintained arms, to the index.
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> opd(0, 1);
    std::uniform_int_distribution<int> docd(0, n - 1);
    std::vector<int> hot(ranked.begin(), ranked.begin() + std::min<int>(K, 10));
    std::uniform_int_distribution<int> hotd(0, static_cast<int>(hot.size()) - 1);

    long g_seq = 0;                                                   // current event index
    std::vector<std::unordered_map<int, long>> pending_seq(n_roles);  // (role,doc) -> grant seq
    std::vector<double> lags;                                         // grant -> searchable, in events

    auto apply_index = [&](int r, int d, bool grant) {
        if (budgeted && grant) { pending[r].insert(d); pending_seq[r][d] = g_seq; return; }
        if (is_lease && per_role.count(r)) {
            if (grant) per_role[r]->addPoint(base + static_cast<size_t>(d) * dim, d);
            else per_role[r]->markDelete(d);
        } else if (is_part && role_group.count(r)) {
            const int g = role_group[r];
            auto* h = per_group[g];
            if (grant) {
                if (gdead[g].count(d)) { h->unmarkDelete(d); gdead[g].erase(d); }
                else if (!gindex[g].count(d)) {
                    h->addPoint(base + static_cast<size_t>(d) * dim, d);
                    gindex[g].insert(d);
                }
            } else if (gindex[g].count(d)) {
                h->markDelete(d);
                gdead[g].insert(d);
            }
        }
    };

    // ---------------------------------------------------------------- repair
    // Budgeted modes defer grants; the repair sells a fixed number of insertions,
    // either split across roles by query share (role) or spent on the documents
    // the most query weight can see, one insertion per index that still lacks
    // them (doc). With repair_every > 0 the same total budget is spent in ticks
    // during the stream; the lag of each grant is recorded either way.
    long insertions = 0, budget_used = 0;
    int budget = (budget_arg >= 0) ? budget_arg : changes / 2;
    const int repair_every = (argc >= 18) ? std::stoi(argv[17]) : 0;
    long ticks = 0;
    long carry = 0;
    double repair_s = 0.0;

    auto do_repair = [&](long tick_budget, long now_seq) {
        if (!budgeted || tick_budget <= 0) return;
        const auto t_r0 = Clock::now();
        ++ticks;
        long spent_here = 0;
        std::unordered_map<int, std::vector<int>> doc_roles;
        for (int r : promoted) {
            const bool has = is_lease ? per_role.count(r) : role_group.count(r);
            if (!has) continue;
            for (int d : pending[r]) if (cur[r].count(d)) doc_roles[d].push_back(r);
        }
        auto record = [&](int r, int d) {
            auto it = pending_seq[r].find(d);
            if (it != pending_seq[r].end()) {
                lags.push_back(static_cast<double>(now_seq - it->second));
                pending_seq[r].erase(it);
            }
            pending[r].erase(d);
        };
        auto put = [&](int r, int d) {   // one insertion into role r's index
            if (is_lease) {
                per_role[r]->addPoint(base + static_cast<size_t>(d) * dim, d);
            } else {
                const int g = role_group[r];
                auto* h = per_group[g];
                if (gdead[g].count(d)) { h->unmarkDelete(d); gdead[g].erase(d); }
                else if (!gindex[g].count(d)) {
                    h->addPoint(base + static_cast<size_t>(d) * dim, d);
                    gindex[g].insert(d);
                }
            }
            ++insertions; ++spent_here; ++budget_used;
        };
        if (mode == "partbudget_doc" || mode == "leasebudget_doc") {
            std::vector<std::pair<double, int>> order;
            order.reserve(doc_roles.size());
            for (auto& kv : doc_roles) {
                double w = 0.0;
                for (int r : kv.second) w += counts[r];
                order.emplace_back(w, kv.first);
            }
            std::sort(order.begin(), order.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
            for (auto& pr : order) {
                if (spent_here >= tick_budget) break;
                std::unordered_set<int> done;   // one insertion per index
                for (int r : doc_roles[pr.second]) {
                    if (spent_here >= tick_budget) break;
                    const int key = is_lease ? r : role_group[r];
                    if (done.count(key)) continue;
                    put(r, pr.second);
                    done.insert(key);
                    record(r, pr.second);
                }
            }
        } else {
            std::vector<int> by_share = promoted_list;
            std::sort(by_share.begin(), by_share.end(),
                      [&](int a, int b) { return counts[a] > counts[b]; });
            const long total_q = std::max<long>(1, std::accumulate(
                by_share.begin(), by_share.end(), 0L,
                [&](long s, int r) { return s + counts[r]; }));
            for (size_t i = 0; i < by_share.size(); ++i) {
                if (spent_here >= tick_budget) break;
                const int r = by_share[i];
                const bool has = is_lease ? per_role.count(r) : role_group.count(r);
                if (!has) continue;
                long share = (i + 1 == by_share.size())
                                 ? (tick_budget - spent_here)
                                 : static_cast<long>(tick_budget) * counts[r] / total_q;
                long used = 0;
                std::vector<int> cand;   // copy first: record() erases from pending[r]
                for (int d : pending[r]) if (cur[r].count(d)) cand.push_back(d);
                for (int d : cand) {
                    if (used >= share || spent_here >= tick_budget) break;
                    put(r, d);
                    ++used;
                    record(r, d);
                }
            }
        }
        repair_s += std::chrono::duration<double>(Clock::now() - t_r0).count();
    };

    auto tc0 = Clock::now();
    for (int c = 0; c < changes; ++c) {
        g_seq = c;
        const int r = hot[hotd(rng)];
        const bool grant = opd(rng);
        if (!grant && !cur[r].empty()) {
            std::uniform_int_distribution<int> pick(0, static_cast<int>(cur[r].size()) - 1);
            auto it = cur[r].begin();
            std::advance(it, pick(rng));
            const int d = *it;
            apply_index(r, d, false);
            cur[r].erase(it);
        } else if (grant) {
            for (int tries = 0; tries < 100; ++tries) {
                const int d = docd(rng);
                if (cur[r].count(d) || ever.count(d)) continue;
                ever.insert(d);
                apply_index(r, d, true);
                cur[r].insert(d);
                break;
            }
        }
        if (repair_every > 0 && (c + 1) % repair_every == 0) {
            // per-tick allowance plus whatever the previous tick could not spend,
            // so the schedule never starves the budget it was given
            const long n_ticks = std::max<long>(1, (changes + repair_every - 1) / repair_every);
            const long per_tick = (budget + n_ticks - 1) / n_ticks;
            const long want = std::min<long>(budget - budget_used, per_tick + carry);
            const long before = budget_used;
            do_repair(want, c + 1);
            carry = std::max<long>(0, want - (budget_used - before));
        }
    }
    const double update_s = std::chrono::duration<double>(Clock::now() - tc0).count();

    // whatever the schedule left behind is repaired at the end of the stream
    do_repair(budget - budget_used, changes);

    long never_repaired = 0;
    for (int r : promoted) for (int d : pending[r]) if (cur[r].count(d)) ++never_repaired;


    std::vector<double> lat(nq);
    int leaks = 0, missing = 0;
    for (int q = 0; q < nq; ++q) {
        const int r = qroles[q];
        const float* qv = queries + static_cast<size_t>(q) * dim;
        std::vector<int> got;
        auto t = Clock::now();
        hnswlib::HierarchicalNSW<float>* h = nullptr;
        if (mode == "shared") h = global;
        else if (mode == "static" || is_lease) {
            auto it = per_role.find(r);
            if (it != per_role.end()) h = it->second;
        } else if (is_part) {
            auto it = role_group.find(r);
            if (it != role_group.end()) h = per_group[it->second];
        }
        if (h) {
            auto res = h->searchKnn(qv, static_cast<size_t>(k) * mfac);
            while (!res.empty()) {
                const int lab = static_cast<int>(res.top().second);
                res.pop();
                if (cur[r].count(lab)) got.push_back(lab);
            }
            std::reverse(got.begin(), got.end());
            if (static_cast<int>(got.size()) > k) got.resize(k);
        } else {
            std::vector<int> docs(cur[r].begin(), cur[r].end());
            got = exact_topk(base, dim, docs, qv, k);
        }
        lat[q] = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
        std::vector<int> docs(cur[r].begin(), cur[r].end());
        auto truth = exact_topk(base, dim, docs, qv, k);
        std::unordered_set<int> gs(got.begin(), got.end());
        for (int d : got) if (!cur[r].count(d)) ++leaks;
        for (int d : truth) if (!gs.count(d)) ++missing;
    }

    size_t indexed = 0;
    if (mode == "shared") indexed = n;
    else if (is_part) {
        for (auto& gd : group_docs) indexed += gd.size();
    } else {
        for (auto& kv : per_role) indexed += kv.second->cur_element_count;
    }
    const double mem = static_cast<double>(indexed) / static_cast<double>(n);

    std::ostringstream ss;
    ss << "mode=" << mode << " n=" << n << " dim=" << dim << " nq=" << nq << " k=" << k
       << " K=" << K << " mfac=" << mfac << " changes=" << changes
       << " build_s=" << build_s << " update_s=" << update_s
       << " p50=" << pctl(lat, 0.50) << " p95=" << pctl(lat, 0.95)
       << " leaks=" << leaks << " missing=" << missing
       << " memory=" << mem;
    if (is_part) ss << " groups=" << group_docs.size();
    if (budgeted) {
        ss << " budget=" << budget << " insertions=" << insertions
           << " repair_s=" << repair_s << " repair_every=" << repair_every
           << " ticks=" << ticks << " never=" << never_repaired;
        if (!lags.empty()) {
            ss << " lag_p50=" << pctl(lags, 0.50) << " lag_p95=" << pctl(lags, 0.95);
        }
    }
    ss << "\n";
    std::ofstream sm(outdir + "/summary.txt");
    sm << ss.str();
    std::cout << ss.str();
    return 0;
}
