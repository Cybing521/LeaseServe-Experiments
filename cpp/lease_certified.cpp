// Coverage-certified materialization lease, single writer (reference driver).
//
// This is the experimental counterpart of docs/172 (coverage lease). It keeps the
// old contract driver's query stream and metrics, but the read path is now
// certified by an interval [a, omega] instead of version equality:
//
//   * a read loads the lease, registers a ref, re-checks the pointer, pins the
//     published epoch, and uses the lease only if a <= pin <= omega.  If the
//     interval does not hold it scans V(r,pin) exactly -- that is the *only*
//     fallback (plus an 8-way lease-swap retry, which is counted with it).
//   * grant/revoke publication is data -> omega -> version.  A grant is appended
//     to the per-lease ring (entry first, ring_n release), a revoke only flips the
//     byte.  DROP_GRANT=k loses every k-th grant from the ring and freezes omega
//     from then until the next merge: exactly what the certificate has to catch.
//   * a merge builds G'=G over cur in the background, then swaps the lease
//     pointer under a short lock.  Readers are never drained.  The old lease is
//     retired and freed when its refcount and the per-role load guard are clear.
//   * history rows are reclaimed below min live a (current + retired leases with
//     refs).  A retained baseline row keeps the state at that lower bound, so a
//     live pin can always reconstruct its state; multi (re-granted) docs use the
//     history, every other doc uses the per-document bytes.
//
// Deviations from the task's letter, all documented in the final report:
//   1. a per-role `loading` guard (seq_cst) closes the load-pointer / refs++
//      reclamation race; without it `refs==0 && delete` is a use-after-free.
//   2. the fallback keeps its lease ref until the exact scan is done, so
//      min-live-a cannot pass the pinned epoch in the middle of the scan.
//   3. historical state is reconstructed as "last transition <= min_live_a"
//      baseline + transitions above it (a strict prefix drop loses the state at
//      the lower bound for old pins).
//   4. |W| for the dead-fraction phi is the materialized set size
//      |V(r,a)| + |ring| (graph element count + ring_n), per docs/172.
//   5. recovery replays the log, rebuilds the lease over the replayed set at the
//      last durable epoch, and only then serves; a post-recovery change therefore
//      never advances omega over grants that are in neither graph nor ring.
//
// Run:
//   lease_certified base queries roles qroles n dim nq k K changes threads seed outdir [mfac]
// Env: CHANGE_US=500 DELTA_CAP=4096 MERGE_EVERY=0 DROP_GRANT=0 CERT=1 WRITERS=1 MMAX=5000
// Build:
// Build: g++ -O3 -march=native -std=c++17 -I experiments/cpp/third_party experiments/cpp/lease_certified.cpp -o experiments/cpp/lease_certified -pthread

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <numeric>
#include <queue>
#include <random>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "hnswlib/hnswlib.h"

#if defined(__AVX512F__)
#include <immintrin.h>
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

#if defined(__AVX512F__)
static inline float l2_dist(const float* v, const float* q, int dim) {
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
#else
static inline float l2_dist(const float* v, const float* q, int dim) {
    float s = 0.0f;
    for (int j = 0; j < dim; ++j) {
        const float t = v[j] - q[j];
        s += t * t;
    }
    return s;
}
#endif

static std::vector<int> exact_topk(const float* base, int dim,
                                   const std::vector<int>& ids,
                                   const float* qv, int k) {
    std::vector<std::pair<float, int>> buf(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        buf[i] = {l2_dist(base + static_cast<size_t>(ids[i]) * dim, qv, dim), ids[i]};
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

// ---------------------------------------------------------------------------
// Lease: immutable graph over V(r,a), per-lease grant ring, coverage epoch omega.
// ---------------------------------------------------------------------------
struct Lease {
    hnswlib::HierarchicalNSW<float>* g = nullptr;  // immutable graph over V(r,a)
    std::vector<std::atomic<int>> ring;            // grants appended since the last merge
    std::vector<std::atomic<uint64_t>> ring_ep;    // epoch of each ring entry
    std::atomic<int> ring_n{0};
    uint64_t a = 0;                                // build epoch of g
    std::atomic<uint64_t> omega{0};                // coverage epoch
    std::atomic<int> refs{0};                      // RCU reference count
};

struct RoleState {
    int rid = -1;                                  // role id (for merge tracing)
    std::atomic<Lease*> lease{nullptr};
    std::vector<Lease*> retired;                   // writer-only
    std::atomic<int> loading{0};                   // seq_cst guard for the load->refs window

    std::vector<std::atomic<uint32_t>> grant_v;    // per-document bytes (0 = built-in)
    std::vector<std::atomic<uint32_t>> revoke_v;   // 0 = not revoked
    std::vector<std::atomic<uint8_t>> multi;       // 1 = re-granted: bytes alone are ambiguous

    std::atomic<uint64_t> version{0};              // published commit epoch
    std::atomic<uint64_t> next_version{0};         // epoch allocator
    std::atomic<int> live{0};                      // |V(r,current)|

    std::mutex exact_mu;                           // cur / ever copy for snapshots
    std::unordered_set<int> cur;                   // writer-only under exact_mu
    std::unordered_set<int> ever;                  // writer-only under exact_mu

    mutable std::shared_mutex ever_mu;             // guards ever for the fallback snapshot
    mutable std::shared_mutex hist_mu;             // guards hist, base, multi updates
    std::unordered_map<int, std::vector<std::pair<uint64_t, uint8_t>>> hist;
    std::vector<std::atomic<uint64_t>> base_ep;    // state at base_ep is base_state
    std::vector<std::atomic<uint8_t>> base_state;

    uint64_t last_gc_a = 0;
    bool gc_done = false;
    int changes_since_merge = 0;
    bool hole = false;                             // DROP_GRANT fired: omega frozen

    // Caller holds hist_mu (shared or unique) when multi[d] can be set.
    bool visible_locked(int d, uint64_t pin) const {
        if (multi[d].load(std::memory_order_relaxed)) {
            auto it = hist.find(d);
            if (it != hist.end() && !it->second.empty()) {
                const auto& v = it->second;
                auto j = std::upper_bound(
                    v.begin(), v.end(), pin,
                    [](uint64_t p, const std::pair<uint64_t, uint8_t>& e) { return p < e.first; });
                if (j != v.begin()) return std::prev(j)->second == 1;
            }
            if (pin >= base_ep[d].load(std::memory_order_relaxed))
                return base_state[d].load(std::memory_order_relaxed) == 1;
            return false;
        }
        const uint32_t g = grant_v[d].load(std::memory_order_relaxed);
        if (g > pin) return false;
        const uint32_t rv = revoke_v[d].load(std::memory_order_relaxed);
        return rv == 0 || rv > pin;
    }

    bool visible(int d, uint64_t pin) const {
        if (!multi[d].load(std::memory_order_acquire)) return visible_locked(d, pin);
        std::shared_lock<std::shared_mutex> lk(hist_mu);
        return visible_locked(d, pin);
    }
};


// ---------------------------------------------------------------------------
// WAL client: the durable sequencer of experiments/service/seqd.py. A commit is
// appended to the write-ahead log and fsync'd before the version is returned,
// so a grant becomes visible only after it is durable. Replay rebuilds the
// visible sets from the log, which is the persistence half of the theorem's
// assumption ("a grant is logged before it is visible, and omega never passes
// an unlogged grant").
// ---------------------------------------------------------------------------
#ifndef WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

struct Wal {
    int fd = -1;
    bool on = false;
    bool dead = false;         // an RPC failed: the stream position is unknown, refuse further use
    int timeout_ms = 2000;     // a stalled sequencer must fail the commit, not freeze the writer

    void fail() {
        if (fd >= 0) ::close(fd);
        fd = -1;
        dead = true;           // keep `on` true: callers must refuse, not fall back to local epochs
    }

    bool connect_to(const std::string& hp) {
        const auto pos = hp.find(':');
        if (pos == std::string::npos) return false;
        const std::string host = hp.substr(0, pos);
        const int port = std::atoi(hp.substr(pos + 1).c_str());
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(port));
        if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) return false;
        if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            ::close(fd); fd = -1; return false;
        }
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        on = true;
        return true;
    }

    bool rpc(const std::string& req, std::string& resp) {
        if (!on || dead) return false;
        std::string line = req + "\n";
        size_t sent = 0;
        while (sent < line.size()) {
            const ssize_t n = ::send(fd, line.data() + sent, line.size() - sent, 0);
            if (n <= 0) { fail(); return false; }
            sent += static_cast<size_t>(n);
        }
        resp.clear();
        char c = 0;
        while (true) {
            const ssize_t n = ::recv(fd, &c, 1, 0);
            if (n <= 0) { fail(); return false; }
            if (c == '\n') break;
            resp.push_back(c);
        }
        return true;
    }

    long commit(int role, const char* kind, int doc, bool& ok) {
        std::string resp;
        char req[160];
        std::snprintf(req, sizeof(req),
                      "{\"op\":\"commit\",\"role\":%d,\"kind\":\"%s\",\"doc\":%d}", role, kind, doc);
        ok = false;
        if (!on || !rpc(req, resp)) return 0;
        const auto p = resp.find("\"version\":");
        if (p == std::string::npos) return 0;
        ok = true;
        return std::atol(resp.c_str() + p + 10);
    }

    // Current committed version of the sequencer; -1 if it cannot be asked.
    long version() {
        std::string resp;
        if (!on || !rpc("{\"op\":\"version\"}", resp)) return -1;
        const auto p = resp.find("\"version\":");
        if (p == std::string::npos) return -1;
        return std::atol(resp.c_str() + p + 10);
    }

    bool replay(long from, std::vector<std::array<long, 4>>& out) {
        std::string resp;
        char req[96];
        std::snprintf(req, sizeof(req), "{\"op\":\"replay\",\"from\":%ld}", from);
        if (!on || !rpc(req, resp)) return false;
        if (resp.find("\"ok\":false") != std::string::npos) return false;
        // Each event is the literal [version,role,kind,doc] with integer fields;
        // kind is 1 for a grant and 2 for a revoke.
        size_t i = 0;
        while ((i = resp.find('[', i)) != std::string::npos) {
            ++i;
            if (i < resp.size() && resp[i] == '[') continue;      // the outer array
            long v[4] = {0, 0, 0, 0};
            int got = 0;
            while (got < 4 && i < resp.size()) {
                while (i < resp.size() && (resp[i] == ' ' || resp[i] == ',')) ++i;
                long x = 0;
                bool neg = false;
                if (i < resp.size() && resp[i] == '-') { neg = true; ++i; }
                bool any = false;
                while (i < resp.size() && resp[i] >= '0' && resp[i] <= '9') {
                    x = x * 10 + (resp[i++] - '0');
                    any = true;
                }
                if (!any) break;
                v[got++] = neg ? -x : x;
            }
            if (got == 4) out.push_back({v[0], v[1], v[2], v[3]});
            while (i < resp.size() && resp[i] != ']') ++i;
            if (i < resp.size()) ++i;
        }
        return true;
    }
};

static std::atomic<long> g_cert_fallbacks{0}, g_merges{0}, g_retries{0}, g_straddled{0};
static std::atomic<long> g_wal_refused{0};
static std::atomic<long> g_regrant_violations{0};   // REGRANT_CHECK=1
static std::atomic<long> g_promotions{0};           // cold-role promotions
static std::atomic<long> g_promote_us{0};           // total time spent building promoted roles
static std::atomic<long> g_exact_reads{0};
static std::atomic<long> g_unpromoted_change{0};  // refused: change to a role without a state   // reads served by an exact scan (unpromoted or uncertified)
static std::atomic<long> g_leaks{0}, g_missing{0}, g_auth{0};

int main(int argc, char** argv) {
    if (argc != 14 && argc != 15) {
        std::cerr << "usage: lease_certified base queries roles qroles n dim nq k K changes"
                     " threads seed outdir [mfac=15]\n";
        return 2;
    }
    const std::string base_path = argv[1], query_path = argv[2];
    const std::string roles_path = argv[3], qroles_path = argv[4];
    const int n = std::stoi(argv[5]), dim = std::stoi(argv[6]);
    int nq = std::stoi(argv[7]);
    const int k = std::stoi(argv[8]);
    int K = std::stoi(argv[9]);
    int changes = std::stoi(argv[10]);
    int threads = std::stoi(argv[11]);
    const int seed = std::stoi(argv[12]);
    const std::string outdir = argv[13];
    const int mfac = (argc == 15) ? std::stoi(argv[14]) : 15;

    int change_us = 500;
    if (const char* e = std::getenv("CHANGE_US")) change_us = std::max(1, std::atoi(e));
    int delta_cap = 4096;
    if (const char* e = std::getenv("DELTA_CAP")) delta_cap = std::max(1, std::atoi(e));
    int merge_every = 0;
    if (const char* e = std::getenv("MERGE_EVERY")) merge_every = std::max(0, std::atoi(e));
    int drop_grant = 0;
    if (const char* e = std::getenv("DROP_GRANT")) drop_grant = std::max(0, std::atoi(e));
    int cert_on = 1;
    if (const char* e = std::getenv("CERT")) cert_on = (std::atoi(e) != 0);
    int pred_eq = 0;      // PRED=1: version equality instead of the covering interval
    if (const char* e = std::getenv("PRED")) pred_eq = (std::atoi(e) != 0);
    int nolog = 0;        // NOLOG=1: certificate over the graph only (no grant log)
    if (const char* e = std::getenv("NOLOG")) nolog = (std::atoi(e) != 0);
    int writers = 1;
    if (const char* e = std::getenv("WRITERS")) writers = std::max(1, std::atoi(e));
    int mmax = 5000;
    if (const char* e = std::getenv("MMAX")) mmax = std::max(1, std::atoi(e));
    if (threads < 1) threads = 1;
    if (K < 1) K = 1;

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
        if (rid < 0 || rid >= n_roles) { std::cerr << "bad role id\n"; return 2; }
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
    K = std::min<int>(K, n_roles);
    std::unordered_set<int> promoted;
    for (int i = 0; i < K; ++i) {
        if (!ids[ranked[i]].empty()) promoted.insert(ranked[i]);
    }

    // SCRIPT=<file>: deterministic change stream for kernel-level regressions.
    // One operation per line, '#' starts a comment:
    //   grant <role> <doc> [lost]   grant; "lost" also drops its log entry
    //   revoke <role> <doc>
    //   merge <role>
    // The stream runs single-threaded before the readers start, so every read pins
    // the final epoch and the run is reproducible.  The supplied script is also the
    // independent event stream an external oracle can fold (P0-2).
    struct ScriptOp { char kind; int role; int doc; bool lost; };
    std::vector<ScriptOp> script;
    if (const char* sp = std::getenv("SCRIPT")) {
        std::ifstream in(sp);
        if (!in) { std::cerr << "cannot open SCRIPT " << sp << "\n"; return 2; }
        std::string line;
        int lineno = 0;
        while (std::getline(in, line)) {
            ++lineno;
            const auto hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            std::istringstream iss(line);
            std::string k;
            if (!(iss >> k)) continue;
            if (k == "merge") {
                int r = 0;
                if (!(iss >> r) || r < 0 || r >= n_roles) {
                    std::cerr << "SCRIPT " << sp << ":" << lineno << ": bad merge\n";
                    return 2;
                }
                script.push_back({'m', r, -1, false});
            } else if (k == "grant" || k == "revoke") {
                int r = 0, d = 0;
                std::string flag;
                if (!(iss >> r >> d) || r < 0 || r >= n_roles || d < 0 || d >= n) {
                    std::cerr << "SCRIPT " << sp << ":" << lineno << ": bad " << k << "\n";
                    return 2;
                }
                iss >> flag;
                if (k == "revoke" && !flag.empty()) {
                    std::cerr << "SCRIPT " << sp << ":" << lineno
                              << ": revoke takes no flag\n";
                    return 2;
                }
                script.push_back({k[0], r, d, flag == "lost"});
            } else {
                std::cerr << "SCRIPT " << sp << ":" << lineno << ": unknown op " << k
                          << "\n";
                return 2;
            }
        }
        if (!script.empty()) changes = static_cast<int>(script.size());
        std::cerr << "SCRIPT: " << script.size() << " operations from " << sp << "\n";
    }

    hnswlib::L2Space space(dim);
    // The registry: one atomic slot per role. A null slot is a role the process does
    // not materialize; its reads take the exact path. Promotion stores a fully built
    // state into its slot, so a reader that observes the pointer also observes the
    // graph, the log and the lease it points at.
    std::vector<std::atomic<RoleState*>> by_role(static_cast<size_t>(n_roles));
    for (auto& slot : by_role) slot.store(nullptr, std::memory_order_relaxed);
    std::vector<RoleState*> all_states;                 // iteration order; guarded by promo_mu
    std::shared_mutex promo_mu;
    Wal wal;
    bool replay_on = false;
    std::signal(SIGPIPE, SIG_IGN);   // a dead sequencer must be a failed commit, not a crash
    if (const char* e = std::getenv("WAL")) {
        // A configured WAL is a durability contract: if it cannot be reached the run
        // must fail closed instead of silently falling back to local epochs.
        if (const char* t = std::getenv("WAL_TIMEOUT_MS"))
            wal.timeout_ms = std::max(1, std::atoi(t));
        if (!wal.connect_to(e)) {
            std::cerr << "WAL configured but unreachable (" << e
                      << "); refusing to run without durability\n";
            return 3;
        }
        std::cerr << "WAL connected: " << e << "\n";
    }
    if (const char* e = std::getenv("REPLAY")) replay_on = (std::atoi(e) != 0);
    // Review P0-2: a WAL that already holds grants must be replayed before this process
    // may serve or advance the watermark, otherwise the graph is built from the base
    // ACL and a later revoke can publish an interval whose materialized set no longer
    // covers the visible one.
    bool wal_unreadable = false;
    if (wal.on && !replay_on) {
        const long v = wal.version();
        if (v < 0) {
            // A stalled sequencer must not make the read path unavailable: serve the
            // exact base ACL and refuse every commit below (fail closed on writes).
            std::cerr << "WAL connected but its version is unreadable; commits will be refused\n";
            wal_unreadable = true;
        } else if (v > 0) {
            std::cerr << "WAL holds " << v << " committed events but REPLAY is off; refusing "
                         "to serve on top of a log it has not replayed (set REPLAY=1)\n";
            return 6;
        }
    }
    // ANY_ROLE=1 lets the writer target any role, which exercises the refusal of a
    // change to a role that has no state (an unpromoted role).
    bool any_role = false;
    if (const char* e = std::getenv("ANY_ROLE")) any_role = (std::atoi(e) != 0);
    // PROMOTE_ON_WRITE=1 promotes a cold role on its first change instead of refusing
    // it. Off by default: the default path is the measured one.
    bool promote_on_write = false;
    if (const char* e = std::getenv("PROMOTE_ON_WRITE"))
        promote_on_write = (std::atoi(e) != 0);
    // READ_DELAY_MS: test-only delay before the first read.  A stalled-commit
    // regression uses it so the fallback provably overlaps the blocked writer.
    int read_delay_ms = 0;
    if (const char* e = std::getenv("READ_DELAY_MS"))
        read_delay_ms = std::max(0, std::atoi(e));
    int selfcheck = 0;    // SELFCHECK=1: verify visible == cur at the published epoch
    if (const char* e = std::getenv("SELFCHECK")) selfcheck = (std::atoi(e) != 0);
    // REGRANT_CHECK=1: after every change, the visibility at the document's last two
    // recorded epochs must still match those epochs. A publication order that lets a
    // new grant/revert overwrite the fast-path bytes before the history is visible
    // (the regrant window of review W1) fails here on the change that opens it.
    int regrant_check = 0;
    if (const char* e = std::getenv("REGRANT_CHECK")) regrant_check = (std::atoi(e) != 0);

    auto t0 = Clock::now();
    // With a replayed log the graph is built once, from the replayed set, inside
    // the recovery block; building the base graph first would double the restart.
    const bool defer_build = replay_on && wal.on;
    for (int r : promoted) {
        auto* st = new RoleState();
        st->rid = r;
        st->grant_v = std::vector<std::atomic<uint32_t>>(n);
        st->revoke_v = std::vector<std::atomic<uint32_t>>(n);
        st->multi = std::vector<std::atomic<uint8_t>>(n);
        st->base_ep = std::vector<std::atomic<uint64_t>>(n);
        st->base_state = std::vector<std::atomic<uint8_t>>(n);
        for (int d = 0; d < n; ++d) {
            st->grant_v[d].store(0, std::memory_order_relaxed);
            st->revoke_v[d].store(0, std::memory_order_relaxed);
            st->multi[d].store(0, std::memory_order_relaxed);
            st->base_ep[d].store(0, std::memory_order_relaxed);
            st->base_state[d].store(0, std::memory_order_relaxed);
        }
        auto* g = new hnswlib::HierarchicalNSW<float>(
            &space, defer_build ? 16 : ids[r].size() + 16, 16, 100);
        for (int d : ids[r]) {
            if (!defer_build) g->addPoint(base + static_cast<size_t>(d) * dim, d);
            st->cur.insert(d);
            {
                std::unique_lock<std::shared_mutex> el(st->ever_mu);
                st->ever.insert(d);
            }
            st->base_state[d].store(1, std::memory_order_relaxed);
        }
        st->live.store(static_cast<int>(st->cur.size()), std::memory_order_relaxed);
        auto* L = new Lease();
        L->g = g;
        L->ring = std::vector<std::atomic<int>>(delta_cap);
        L->ring_ep = std::vector<std::atomic<uint64_t>>(delta_cap);
        for (int i = 0; i < delta_cap; ++i) {
            L->ring[i].store(0, std::memory_order_relaxed);
            L->ring_ep[i].store(0, std::memory_order_relaxed);
        }
        L->ring_n.store(0);
        L->a = 0;
        L->omega.store(0, std::memory_order_relaxed);
        L->refs.store(0, std::memory_order_relaxed);
        st->lease.store(L, std::memory_order_release);
        by_role[static_cast<size_t>(r)].store(st, std::memory_order_release);
        all_states.push_back(st);
    }
    const double build_s = std::chrono::duration<double>(Clock::now() - t0).count();

    // Promotion: build a role's lease on demand and publish it atomically. The role's
    // visible set is ids[r] and is exact, because the writer refuses changes to a role
    // that has no state (see apply_change): an unpromoted V(r,e) never changes.
    bool replaying = false;   // promote() must not write to the log while replaying it
    // MEM_BALLAST_MB=N touches N MB of anonymous memory and keeps it: with the
    // promotion path off, this reproduces the resident set of a run that promoted
    // roles, which separates "the working set no longer fits" from "promotion did
    // something specific".
    void* ballast = nullptr;
    std::vector<void*> ballast_chunks;
    if (const char* bm = std::getenv("MEM_BALLAST_MB")) {
        const size_t mb = static_cast<size_t>(std::atoi(bm));
        // MEM_BALLAST_CHUNK_KB splits the same number of bytes into many small
        // mappings, which is what a promotion does (five n-byte arrays plus a graph per
        // role). One big block and many small blocks differ in mapping count, not bytes,
        // so this separates "the working set" from "the shape of the allocation".
        size_t chunk_kb = 0;
        if (const char* ck = std::getenv("MEM_BALLAST_CHUNK_KB")) chunk_kb = std::atoi(ck);
        if (mb) {
            if (chunk_kb == 0) {
                ballast = std::malloc(mb << 20);
                if (ballast) std::memset(ballast, 1, mb << 20);
            } else {
                const size_t per = chunk_kb << 10;
                const size_t nchunks = (mb << 20) / per;
                for (size_t i = 0; i < nchunks; ++i) {
                    void* c = std::malloc(per);
                    if (!c) break;
                    std::memset(c, 1, per);
                    ballast_chunks.push_back(c);
                }
            }
            std::cerr << "MEM_BALLAST_MB: touched " << mb << " MB in "
                      << (chunk_kb ? ballast_chunks.size() : 1) << " chunk(s)\n";
        }
    }

    auto promote = [&](int r) -> RoleState* {
        std::unique_lock<std::shared_mutex> pl(promo_mu);
        RoleState* have = by_role[static_cast<size_t>(r)].load(std::memory_order_acquire);
        if (have) return have;
        const auto t_start = Clock::now();
        auto* st = new RoleState();
        st->rid = r;
        st->grant_v = std::vector<std::atomic<uint32_t>>(n);
        st->revoke_v = std::vector<std::atomic<uint32_t>>(n);
        st->multi = std::vector<std::atomic<uint8_t>>(n);
        st->base_ep = std::vector<std::atomic<uint64_t>>(n);
        st->base_state = std::vector<std::atomic<uint8_t>>(n);
        for (int d = 0; d < n; ++d) {
            st->grant_v[d].store(0, std::memory_order_relaxed);
            st->revoke_v[d].store(0, std::memory_order_relaxed);
            st->multi[d].store(0, std::memory_order_relaxed);
            st->base_ep[d].store(0, std::memory_order_relaxed);
            st->base_state[d].store(0, std::memory_order_relaxed);
        }
        auto* g = new hnswlib::HierarchicalNSW<float>(&space, ids[r].size() + 16, 16, 100);
        for (int d : ids[r]) {
            g->addPoint(base + static_cast<size_t>(d) * dim, d);
            st->cur.insert(d);
            {
                std::unique_lock<std::shared_mutex> el(st->ever_mu);
                st->ever.insert(d);
            }
            st->base_state[d].store(1, std::memory_order_relaxed);
        }
        st->live.store(static_cast<int>(st->cur.size()), std::memory_order_relaxed);
        auto* L = new Lease();
        L->g = g;
        L->ring = std::vector<std::atomic<int>>(delta_cap);
        L->ring_ep = std::vector<std::atomic<uint64_t>>(delta_cap);
        for (int i = 0; i < delta_cap; ++i) {
            L->ring[i].store(0, std::memory_order_relaxed);
            L->ring_ep[i].store(0, std::memory_order_relaxed);
        }
        L->ring_n.store(0);
        L->a = 0;
        L->omega.store(0, std::memory_order_relaxed);
        L->refs.store(0, std::memory_order_relaxed);
        // A promotion is a durable fact: record it before publishing, so a restart
        // rebuilds the same registry instead of refusing the log (review R3).
        uint64_t a_ep = 0;
        if (wal.on && !replaying) {
            bool durable = false;
            const long v = wal_unreadable ? -1 : wal.commit(r, "promote", -1, durable);
            if (!durable) {
                ++g_wal_refused;
                delete g;
                delete L;
                delete st;
                return nullptr;
            }
            a_ep = static_cast<uint64_t>(v);
        }
        if (a_ep) {
            L->a = a_ep;
            L->omega.store(a_ep, std::memory_order_relaxed);
            st->version.store(a_ep, std::memory_order_relaxed);
            st->next_version.store(a_ep, std::memory_order_relaxed);
        }
        static const bool promote_dry = [] {
            const char* e = std::getenv("PROMOTE_DRY");
            return e && std::atoi(e) != 0;
        }();
        if (promote_dry) {           // same build cost, nothing published
            delete g;
            delete L;
            delete st;
            return nullptr;
        }
        st->lease.store(L, std::memory_order_release);
        by_role[static_cast<size_t>(r)].store(st, std::memory_order_release);
        all_states.push_back(st);
        g_promotions.fetch_add(1, std::memory_order_relaxed);
        if (const char* pl = std::getenv("PROMOTE_LOG")) {   // role, documents, build ms
            if (FILE* pf = std::fopen(pl, "a")) {
                std::fprintf(pf, "%d %zu %.3f\n", r, ids[r].size(),
                             std::chrono::duration<double, std::milli>(
                                 Clock::now() - t_start).count());
                std::fclose(pf);
            }
        }
        g_promote_us.fetch_add(static_cast<long>(
            std::chrono::duration<double, std::micro>(Clock::now() - t_start).count()),
            std::memory_order_relaxed);
        return st;
    };

    auto cleanup_retired = [&](RoleState* st) {
        auto& r = st->retired;
        for (size_t i = 0; i < r.size();) {
            Lease* L = r[i];
            if (st->loading.load(std::memory_order_seq_cst) == 0 &&
                L->refs.load(std::memory_order_seq_cst) == 0) {
                delete L->g;
                delete L;
                r[i] = r.back();
                r.pop_back();
            } else {
                ++i;
            }
        }
    };

    auto min_live_a = [&](RoleState* st) -> uint64_t {
        uint64_t mla = st->lease.load(std::memory_order_acquire)->a;
        for (Lease* L : st->retired) mla = std::min(mla, L->a);
        return mla;
    };

    // Drop history below min_live_a, keeping the last transition <= min_live_a as
    // the base state (pins >= min_live_a can always reconstruct from it).
    auto gc_hist = [&](RoleState* st, uint64_t mla) {
        if (st->gc_done && mla <= st->last_gc_a) return;
        std::unique_lock<std::shared_mutex> lk(st->hist_mu);
        for (auto it = st->hist.begin(); it != st->hist.end();) {
            auto& v = it->second;
            size_t cut = 0;
            while (cut < v.size() && v[cut].first <= mla) ++cut;
            if (cut > 0) {
                const auto baseline = v[cut - 1];
                st->base_ep[it->first].store(baseline.first, std::memory_order_relaxed);
                st->base_state[it->first].store(baseline.second, std::memory_order_relaxed);
                v.erase(v.begin(), v.begin() + cut);
            }
            if (v.empty()) it = st->hist.erase(it); else ++it;
        }
        st->last_gc_a = mla;
        st->gc_done = true;
    };

    // Exact top-k of V(r,pin).  Caller must hold a lease ref whose a <= pin so
    // that min_live_a cannot pass pin while we scan.
    auto exact_visible = [&](RoleState* st, uint64_t pin, const float* qv,
                             std::vector<int>& out) {
        std::vector<int> docs;
        {
            // Only `ever` is needed here.  Taking exact_mu would let a writer that is
            // blocked in a durable commit (next_epoch) stall the exact fallback, so the
            // snapshot has its own reader/writer lock (ever_mu).
            std::shared_lock<std::shared_mutex> lk(st->ever_mu);
            docs.assign(st->ever.begin(), st->ever.end());
        }
        std::vector<int> vis;
        vis.reserve(docs.size());
        {
            std::shared_lock<std::shared_mutex> lk(st->hist_mu);
            for (int d : docs) {
                if (st->visible_locked(d, pin)) vis.push_back(d);
            }
        }
        out = exact_topk(base, dim, vis, qv, k);
    };

    // Build G' over a snapshot of cur, then swap the lease.  The graph build is
    // outside exact_mu so fallback snapshots are never blocked by it.
    auto do_merge = [&](RoleState* st) {
        std::vector<int> snap;
        uint64_t v0 = 0;
        {
            std::lock_guard<std::mutex> lk(st->exact_mu);
            snap.assign(st->cur.begin(), st->cur.end());
            v0 = st->version.load(std::memory_order_acquire);
        }
        auto* ng = new hnswlib::HierarchicalNSW<float>(
            &space, snap.size() + 16, 16, 100);
        for (int d : snap) ng->addPoint(base + static_cast<size_t>(d) * dim, d);

        auto* L2 = new Lease();
        L2->g = ng;
        L2->ring = std::vector<std::atomic<int>>(delta_cap);
        L2->ring_ep = std::vector<std::atomic<uint64_t>>(delta_cap);
        for (int i = 0; i < delta_cap; ++i) {
            L2->ring[i].store(0, std::memory_order_relaxed);
            L2->ring_ep[i].store(0, std::memory_order_relaxed);
        }
        L2->ring_n.store(0);
        L2->refs.store(0);
        {
            std::lock_guard<std::mutex> lk(st->exact_mu);
            Lease* old = st->lease.load(std::memory_order_relaxed);
            const uint64_t vnow = st->version.load(std::memory_order_acquire);
            L2->a = v0;
            L2->omega.store(vnow, std::memory_order_relaxed);
            const int rn = old->ring_n.load(std::memory_order_acquire);
            int j = 0;
            for (int i = 0; i < rn; ++i) {
                const uint64_t ep = old->ring_ep[i].load(std::memory_order_relaxed);
                if (ep > v0) {
                    L2->ring[j].store(old->ring[i].load(std::memory_order_relaxed),
                                      std::memory_order_relaxed);
                    L2->ring_ep[j].store(ep, std::memory_order_relaxed);
                    ++j;
                }
            }
            L2->ring_n.store(j, std::memory_order_release);
            st->lease.exchange(L2, std::memory_order_seq_cst);
            if (const char* mo = std::getenv("MERGES_OUT")) {
                if (FILE* mf = std::fopen(mo, "a")) {
                    std::fprintf(mf, "%d %llu\n", st->rid,
                                 static_cast<unsigned long long>(L2->a));
                    std::fclose(mf);
                }
            }
            Lease* prev = old;
            if (st->loading.load(std::memory_order_seq_cst) == 0 &&
                prev->refs.load(std::memory_order_seq_cst) == 0) {
                delete prev->g;
                delete prev;
            } else {
                st->retired.push_back(prev);
            }
        }
        st->hole = false;
        st->changes_since_merge = 0;
        g_merges.fetch_add(1, std::memory_order_relaxed);
    };

    // ---- read path ---------------------------------------------------------
    const bool skip_verify = []() {
        const char* e = std::getenv("SKIP_VERIFY");
        return e && std::atoi(e) != 0;
    }();
    auto query_one = [&](int q, std::vector<int>& got, uint64_t& pin_out,
                         double& lat_ms, int& role_out) -> bool {
        role_out = qroles[q];
        const float* qv = queries + static_cast<size_t>(q) * dim;
        got.clear();
        pin_out = 0;
        auto t_start = Clock::now();
        RoleState* st = by_role[static_cast<size_t>(role_out)].load(std::memory_order_acquire);
        if (!st) {
            g_exact_reads.fetch_add(1, std::memory_order_relaxed);
            got = exact_topk(base, dim, ids[role_out], qv, k);
            lat_ms = std::chrono::duration<double, std::milli>(Clock::now() - t_start).count();
            return false;
        }
        bool exact_read = false;
        bool done = false;
        for (int attempt = 0; attempt < 8 && !done; ++attempt) {
            st->loading.fetch_add(1, std::memory_order_seq_cst);
            Lease* L = st->lease.load(std::memory_order_seq_cst);
            L->refs.fetch_add(1, std::memory_order_seq_cst);
            if (st->lease.load(std::memory_order_seq_cst) != L) {
                L->refs.fetch_sub(1, std::memory_order_seq_cst);
                st->loading.fetch_sub(1, std::memory_order_seq_cst);
                g_retries.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // refs now protects L; release the load guard so reclamation is not
            // starved by readers that are merely searching.
            st->loading.fetch_sub(1, std::memory_order_seq_cst);
            const uint64_t pin = st->version.load(std::memory_order_acquire);
            const uint64_t omega = nolog ? L->a
                                         : L->omega.load(std::memory_order_acquire);
            const bool covered = cert_on && (pred_eq ? (pin == L->a)
                                                     : (pin >= L->a && pin <= omega));

            if (cert_on && !covered) {
                exact_visible(st, pin, qv, got);
                g_cert_fallbacks.fetch_add(1, std::memory_order_relaxed);
                g_exact_reads.fetch_add(1, std::memory_order_relaxed);
                exact_read = true;
                done = true;
            } else {
                // m = min(m_max, ceil(rho*k/(1-phi))), phi = dead fraction of W.
                const long W = static_cast<long>(L->g->cur_element_count.load()) +
                               L->ring_n.load(std::memory_order_acquire);
                const int live = std::max(1, st->live.load(std::memory_order_relaxed));
                double phi = 1.0 - static_cast<double>(live) / std::max<long>(1, W);
                if (phi < 0.0) phi = 0.0;
                if (phi > 0.99) phi = 0.99;
                long m = static_cast<long>(std::ceil(static_cast<double>(k) * mfac));
                m = std::min<long>(m, mmax);
                m = std::max<long>(m, k);
                m = std::min<long>(m, static_cast<long>(L->g->cur_element_count.load()));
                if (m < 1) m = 1;

                auto res = L->g->searchKnn(qv, static_cast<size_t>(m));
                std::vector<std::pair<float, int>> cand;
                cand.reserve(res.size() + L->ring_n.load(std::memory_order_relaxed) + 1);
                // A doc can be both a dead graph node and a fresh ring entry (revoke
                // then re-grant), so dedupe across the two candidate sources.
                std::unordered_set<int> seen;
                while (!res.empty()) {
                    const int d = static_cast<int>(res.top().second);
                    const float dist = res.top().first;
                    res.pop();
                    if (!st->visible(d, pin)) continue;   // dead node: filter, never return
                    if (!seen.insert(d).second) continue;
                    cand.emplace_back(dist, d);
                }
                const int rn = L->ring_n.load(std::memory_order_acquire);
                for (int i = 0; i < rn; ++i) {
                    const uint64_t ep = L->ring_ep[i].load(std::memory_order_relaxed);
                    if (ep > pin) continue;
                    const int d = L->ring[i].load(std::memory_order_relaxed);
                    if (!st->visible(d, pin)) continue;
                    if (!seen.insert(d).second) continue;
                    cand.emplace_back(
                        l2_dist(base + static_cast<size_t>(d) * dim, qv, dim), d);
                }
                const size_t kk = std::min<size_t>(static_cast<size_t>(k), cand.size());
                if (kk > 0) {
                    std::nth_element(cand.begin(), cand.begin() + kk, cand.end());
                    std::sort(cand.begin(), cand.begin() + kk);
                    for (size_t i = 0; i < kk; ++i) got.push_back(cand[i].second);
                }
                done = true;
            }
            auto t_end = Clock::now();
            lat_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();
            pin_out = pin;

            if (!exact_read && !skip_verify) {
                // Same accounting as the old driver: compare with exact V(r,pin).
                std::vector<int> truth;
                exact_visible(st, pin, qv, truth);
                std::unordered_set<int> rs(got.begin(), got.end());
                for (int d : truth) {
                    if (rs.count(d)) continue;
                    g_missing.fetch_add(1, std::memory_order_relaxed);
                    // W(L) = V(r, a_L) (the graph built at that epoch) + the log
                    // entries at or below the pin.  Graph membership is V(r, a_L),
                    // NOT "the first grant predates a_L": a document granted before
                    // the build epoch can have been revoked before that build and
                    // re-granted after it, so only its later (possibly lost) log
                    // entry can supply the candidate (review P0-1).
                    bool materialized = st->visible(d, L->a);
                    if (!materialized) {
                        const int rn2 = L->ring_n.load(std::memory_order_acquire);
                        for (int i = 0; i < rn2; ++i) {
                            if (L->ring[i].load(std::memory_order_relaxed) == d &&
                                L->ring_ep[i].load(std::memory_order_relaxed) <= pin) {
                                materialized = true;
                                break;
                            }
                        }
                    }
                    if (!materialized) g_auth.fetch_add(1, std::memory_order_relaxed);
                }
                for (int d : got) {
                    if (!st->visible(d, pin))
                        g_leaks.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (st->version.load(std::memory_order_acquire) > pin)
                g_straddled.fetch_add(1, std::memory_order_relaxed);
            L->refs.fetch_sub(1, std::memory_order_seq_cst);
        }
        if (!done) {
            // Eight consecutive lease swaps: certified exact fallback.
            st->loading.fetch_add(1, std::memory_order_seq_cst);
            Lease* L = st->lease.load(std::memory_order_seq_cst);
            L->refs.fetch_add(1, std::memory_order_seq_cst);
            st->loading.fetch_sub(1, std::memory_order_seq_cst);
            const uint64_t pin = st->version.load(std::memory_order_acquire);
            exact_visible(st, pin, qv, got);
            g_cert_fallbacks.fetch_add(1, std::memory_order_relaxed);
            pin_out = pin;
            exact_read = true;
            L->refs.fetch_sub(1, std::memory_order_seq_cst);
            lat_ms = std::chrono::duration<double, std::milli>(Clock::now() - t_start).count();
        }
        return !exact_read;
    };

    // ---- writer ------------------------------------------------------------
    std::vector<uint64_t> vpins(nq, 0);
    std::atomic<bool> readers_go{false};
    double replay_s = 0.0;
    if (replay_on && wal.on) {
        const auto tr0 = Clock::now();
        std::vector<std::array<long, 4>> ev;
        if (!wal.replay(0, ev)) {
            std::cerr << "WAL replay failed; refusing to serve an unverified state\n";
            return 4;
        }
        long grants = 0, revokes = 0, skipped = 0, promoted_on_replay = 0;
        replaying = true;
        for (const auto& e : ev) {
            if (static_cast<int>(e[2]) != 3) continue;
            const int rr = static_cast<int>(e[1]);
            if (rr < 0 || rr >= n_roles) continue;
            if (by_role[static_cast<size_t>(rr)].load(std::memory_order_acquire)) continue;
            RoleState* ns = promote(rr);
            if (!ns) {
                std::cerr << "WAL replay: could not materialize promoted role " << rr << "\n";
                return 6;
            }
            Lease* L = ns->lease.load(std::memory_order_acquire);
            L->a = e[0];
            L->omega.store(e[0], std::memory_order_relaxed);
            ns->version.store(e[0], std::memory_order_relaxed);
            ns->next_version.store(e[0], std::memory_order_relaxed);
            ++promoted_on_replay;
        }
        std::unordered_set<RoleState*> touched;
        for (const auto& e : ev) {
            const int rr = static_cast<int>(e[1]);
            if (rr < 0 || rr >= n_roles) { ++skipped; continue; }
            RoleState* st =
                by_role[static_cast<size_t>(rr)].load(std::memory_order_acquire);
            if (!st) { ++skipped; continue; }
            const int doc = static_cast<int>(e[3]);
            if (static_cast<int>(e[2]) == 3) continue;   // promotion record: pre-pass owns it
            if (doc < 0 || doc >= n) {
                std::cerr << "WAL replay: document " << doc << " outside [0," << n
                          << "); refusing a corrupt log\n";
                return 5;
            }
            const uint64_t ep = static_cast<uint64_t>(e[0]);
            touched.insert(st);
            std::lock_guard<std::mutex> lk(st->exact_mu);
            std::unique_lock<std::shared_mutex> hl(st->hist_mu);
            std::unique_lock<std::shared_mutex> el(st->ever_mu);
            if (e[2] == 1) {                       // grant
                if (st->cur.insert(doc).second) st->live.fetch_add(1);
                st->grant_v[doc].store(static_cast<uint32_t>(ep), std::memory_order_relaxed);
                st->revoke_v[doc].store(0, std::memory_order_relaxed);
                st->ever.insert(doc);
                st->hist[doc].emplace_back(ep, 1);
                if (st->hist[doc].size() > 1)
                    st->multi[doc].store(1, std::memory_order_release);
                ++grants;
            } else {                               // revoke
                if (st->cur.erase(doc)) st->live.fetch_sub(1);
                st->revoke_v[doc].store(static_cast<uint32_t>(ep), std::memory_order_relaxed);
                st->hist[doc].emplace_back(ep, 2);
                if (st->hist[doc].size() > 1)
                    st->multi[doc].store(1, std::memory_order_release);
                ++revokes;
            }
            const uint64_t nv = ep + 1;
            if (st->next_version.load() < nv) st->next_version.store(nv);
            st->version.store(ep, std::memory_order_release);
        }
        // An entry for a role this run does not materialize cannot be honoured: the
        // exact path would serve that role from the stale base ACL.
        if (skipped > 0) {
            std::cerr << "WAL replay: " << skipped
                      << " events target unpromoted roles; refusing to serve a stale ACL\n";
            return 5;
        }
        // Rebuild each touched lease at its last replayed epoch. Without this, the
        // first post-replay change advances omega over a graph and a ring that do
        // not contain the replayed grants -- a certificate that certifies nothing.
        if (defer_build) {
            // Rebuild every lease once, from the replayed set, including roles the
            // log did not touch (their base graph was deferred above).
            for (RoleState* st : all_states) {
                do_merge(st);
                gc_hist(st, min_live_a(st));
            }
        } else {
            for (RoleState* st : touched) {
                do_merge(st);
                gc_hist(st, min_live_a(st));
            }
        }
        replaying = false;
        replay_s = std::chrono::duration<double>(Clock::now() - tr0).count();
        std::cerr << "WAL replay: " << ev.size() << " events (" << grants << " grants, "
                  << revokes << " revokes, " << promoted_on_replay << " promotions); rebuilt "
                  << touched.size()
                  << " lease(s) at their last durable epoch in " << replay_s << " s\n";
    }

    std::atomic<int> changes_done{0};
    // Readers may start after `WARMUP` changes (default: half the stream, at least
    // 100). The summary printed this value long before the variable was read from the
    // environment, so a run that set WARMUP still overlapped its readers with the
    // writer -- which is exactly what a promotion build makes expensive.
    int warmup = std::min(changes, std::max(100, changes / 2));
    if (const char* e = std::getenv("WARMUP")) warmup = std::min(changes, std::max(0, std::atoi(e)));
    std::vector<int> hot(ranked.begin(), ranked.begin() + std::min<int>(K, 10));

    std::mutex commit_mu;
    // Returns 0 when the durable commit failed while the WAL is enabled; the
    // caller must then refuse to publish, because a grant that is not in the
    // log may not become visible.
    auto next_epoch = [&](int r, const char* kind, int doc, bool& durable) -> uint64_t {
        durable = !wal.on;
        if (!wal.on) return 0;
        if (wal_unreadable) { durable = false; return 0; }
        bool ok = false;
        const long v = wal.commit(r, kind, doc, ok);
        durable = ok;
        return ok ? static_cast<uint64_t>(v) : 0;
    };
    // Publish one committed change.  The durable epoch is allocated *before*
    // exact_mu is taken: a writer blocked in next_epoch() must not hold the lock
    // the exact fallback needs to snapshot the ever-set (review P1-4).  The state
    // writes therefore happen after the commit returns, and the version store at
    // the end is what makes them visible to a reader.
    auto publish_epoch = [&](RoleState* st, uint64_t epoch, bool grant, int r,
                             int changed_doc) {
        Lease* L = st->lease.load(std::memory_order_acquire);
        if (!st->hole) L->omega.store(epoch, std::memory_order_release);
        st->version.store(epoch, std::memory_order_release);
        // CHANGES_OUT replays this exact stream into the baseline drivers, so a
        // comparison can be run on one post-change snapshot (review W3).
        if (const char* co = std::getenv("CHANGES_OUT")) {
            if (FILE* cf = std::fopen(co, "a")) {
                std::fprintf(cf, "%llu %s %d %d\n",
                             static_cast<unsigned long long>(epoch),
                             grant ? "grant" : "revoke", r, changed_doc);
                std::fclose(cf);
            }
        }
        if (regrant_check && changed_doc >= 0) {
            std::vector<std::pair<uint64_t, uint8_t>> h;
            {
                std::shared_lock<std::shared_mutex> hl(st->hist_mu);
                auto it = st->hist.find(changed_doc);
                if (it != st->hist.end()) h = it->second;
            }
            if (h.size() >= 2) {
                const auto& prev = h[h.size() - 2];
                const auto& last = h[h.size() - 1];
                const bool ok_prev =
                    st->visible(changed_doc, prev.first) == (prev.second == 1);
                const bool ok_last =
                    st->visible(changed_doc, last.first) == (last.second == 1);
                if (!ok_prev || !ok_last) {
                    ++g_regrant_violations;
                    std::cerr << "REGRANT-CHECK violation role=" << r
                              << " doc=" << changed_doc
                              << " prev=(" << prev.first << "," << int(prev.second)
                              << ") visible=" << st->visible(changed_doc, prev.first)
                              << " last=(" << last.first << "," << int(last.second)
                              << ") visible=" << st->visible(changed_doc, last.first)
                              << "\n";
                }
            }
        }
    };

    // Returns the changed document, or -1 when the change was refused (WAL down) or
    // was a no-op.  `force_lost` is the fault injector: the ACL change is applied but
    // its log entry is dropped, so the watermark must freeze at the previous epoch.
    auto publish_grant = [&](RoleState* st, int r, int d, bool force_lost) -> int {
        if (st->lease.load(std::memory_order_acquire)->ring_n.load(
                std::memory_order_acquire) >= delta_cap) {
            do_merge(st);
        }
        bool durable = true;
        const uint64_t epoch = next_epoch(r, "grant", d, durable);
        if (wal.on && !durable) {
            ++g_wal_refused;                 // no state write happens below
            return -1;
        }
        bool regrant = false;
        uint64_t ep = epoch;
        {
            std::lock_guard<std::mutex> lk(st->exact_mu);
            if (st->cur.count(d)) {          // unreachable under the single-writer rule
                std::cerr << "publish_grant: duplicate grant role=" << r << " doc=" << d
                          << "\n";
                return -1;
            }
            if (ep == 0) ep = st->next_version.fetch_add(1, std::memory_order_acq_rel) + 1;
            regrant = st->ever.count(d) != 0;
            if (regrant) {
                // A regrant makes the (grant_v, revoke_v) pair ambiguous for pins
                // inside the previous lifetime (g1 <= p < g2), so retire the fast path
                // before anything else changes.  This epoch is not published until the
                // history below has been appended and version stored, and a reader that
                // pins >= epoch has synchronized with that release, hence also observes
                // multi == 1.
                st->multi[d].store(1, std::memory_order_release);
            } else {
                st->grant_v[d].store(static_cast<uint32_t>(ep), std::memory_order_relaxed);
                st->revoke_v[d].store(0, std::memory_order_relaxed);
            }
            st->cur.insert(d);
            {
                std::unique_lock<std::shared_mutex> el(st->ever_mu);
                st->ever.insert(d);
            }
            st->live.fetch_add(1, std::memory_order_relaxed);
        }
        if (regrant && regrant_check) {
            // Widen the gap between the state writes and the history append so the
            // sampler can observe it (review W1).  Off unless REGRANT_CHECK=1.
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
        {
            std::unique_lock<std::shared_mutex> lk(st->hist_mu);
            st->hist[d].emplace_back(ep, 1);
        }
        if (!force_lost) {
            Lease* L = st->lease.load(std::memory_order_acquire);
            const int slot = L->ring_n.load(std::memory_order_relaxed);
            L->ring[slot].store(d, std::memory_order_relaxed);
            L->ring_ep[slot].store(ep, std::memory_order_relaxed);
            L->ring_n.store(slot + 1, std::memory_order_release);
        } else {
            st->hole = true;
        }
        publish_epoch(st, ep, true, r, d);
        return d;
    };

    auto publish_revoke = [&](RoleState* st, int r, int victim) -> int {
        bool durable = true;
        const uint64_t epoch = next_epoch(r, "revoke", victim, durable);
        if (wal.on && !durable) {
            ++g_wal_refused;
            return -1;
        }
        uint64_t ep = epoch;
        {
            std::lock_guard<std::mutex> lk(st->exact_mu);
            if (ep == 0) ep = st->next_version.fetch_add(1, std::memory_order_acq_rel) + 1;
            const bool was = st->cur.erase(victim) != 0;
            st->revoke_v[victim].store(static_cast<uint32_t>(ep), std::memory_order_relaxed);
            if (was) st->live.fetch_sub(1, std::memory_order_relaxed);
        }
        {
            std::unique_lock<std::shared_mutex> lk(st->hist_mu);
            st->hist[victim].emplace_back(ep, 2);
        }
        publish_epoch(st, ep, false, r, victim);
        return victim;
    };

    auto apply_change = [&](std::mt19937& rng, long& grant_idx) {
        std::uniform_int_distribution<int> opd(0, 1), docd(0, n - 1);
        std::uniform_int_distribution<int> hotd(0, static_cast<int>(hot.size()) - 1);
        const int r = any_role ? (static_cast<int>(rng() % n_roles)) : hot[hotd(rng)];
        RoleState* st = by_role[static_cast<size_t>(r)].load(std::memory_order_acquire);
        if (!st) {
            if (!promote_on_write) {       // invariant: only materialized roles are
                ++g_unpromoted_change;     // mutated, so an unpromoted V(r,e) never changes
                const int done = changes_done.fetch_add(1, std::memory_order_relaxed) + 1;
                if (!readers_go.load(std::memory_order_relaxed) && done >= warmup)
                    readers_go.store(true, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::microseconds(change_us));
                return;
            }
            st = promote(r);               // cold role turns hot: build, then apply
            if (!st) {                     // PROMOTE_DRY: built and discarded
                const int done = changes_done.fetch_add(1, std::memory_order_relaxed) + 1;
                if (!readers_go.load(std::memory_order_relaxed) && done >= warmup)
                    readers_go.store(true, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::microseconds(change_us));
                return;
            }
        }
        cleanup_retired(st);
        gc_hist(st, min_live_a(st));

        const bool grant = (opd(rng) == 1);
        bool need_merge = false;
        if (grant && st->lease.load(std::memory_order_acquire)->ring_n.load(
                         std::memory_order_acquire) >= delta_cap) {
            need_merge = true;
        }
        if (merge_every > 0 && st->changes_since_merge + 1 >= merge_every) {
            need_merge = true;
        }
        if (need_merge) do_merge(st);

        int changed_doc = -1;
        if (grant) {
            ++grant_idx;
            const bool drop = (drop_grant > 0) && (grant_idx % drop_grant == 0);
            int d = -1;
            {
                std::lock_guard<std::mutex> lk(st->exact_mu);
                for (int tries = 0; tries < 100; ++tries) {
                    const int cand = docd(rng);
                    if (st->cur.count(cand)) continue;
                    d = cand;
                    break;
                }
            }
            if (d >= 0) changed_doc = publish_grant(st, r, d, drop);
        } else {
            int victim = -1;
            {
                std::lock_guard<std::mutex> lk(st->exact_mu);
                if (!st->cur.empty()) {
                    std::uniform_int_distribution<int> pick(
                        0, static_cast<int>(st->cur.size()) - 1);
                    auto it = st->cur.begin();
                    std::advance(it, pick(rng));
                    victim = *it;
                }
            }
            if (victim >= 0) changed_doc = publish_revoke(st, r, victim);
        }
        st->changes_since_merge++;
        const int done = changes_done.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!readers_go.load(std::memory_order_relaxed) && done >= warmup) {
            readers_go.store(true, std::memory_order_release);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(change_us));
    };
    // Declared at function scope: the writer lambda captures it by reference and
    // outlives the block that used to hold it (a dangling capture made some runs
    // execute zero changes and hang the readers).
    const int per_writer = (changes + writers - 1) / writers;
    std::vector<std::thread> writer_threads;
    if (!script.empty()) {
        // The script runs in its own thread so a blocked commit (the B5 regression)
        // can overlap the readers instead of finishing before they are created.
        writer_threads.emplace_back([&]() {
            for (const auto& op : script) {
                RoleState* st = by_role[static_cast<size_t>(op.role)].load(
                    std::memory_order_acquire);
                if (!st) {
                    st = promote(op.role);
                    if (!st) {
                        std::cerr << "SCRIPT: cannot materialize role " << op.role << "\n";
                        std::exit(7);
                    }
                }
                cleanup_retired(st);
                gc_hist(st, min_live_a(st));
                if (op.kind == 'm') {
                    do_merge(st);
                } else if (op.kind == 'g') {
                    bool present = false;
                    {
                        std::lock_guard<std::mutex> lk(st->exact_mu);
                        present = st->cur.count(op.doc) != 0;
                    }
                    if (present) {
                        std::cerr << "SCRIPT: grant of visible role " << op.role
                                  << " document " << op.doc << "\n";
                        std::exit(7);
                    }
                    publish_grant(st, op.role, op.doc, op.lost);
                } else {
                    bool present = false;
                    {
                        std::lock_guard<std::mutex> lk(st->exact_mu);
                        present = st->cur.count(op.doc) != 0;
                    }
                    if (!present) {
                        std::cerr << "SCRIPT: revoke of invisible role " << op.role
                                  << " document " << op.doc << "\n";
                        std::exit(7);
                    }
                    publish_revoke(st, op.role, op.doc);
                }
                st->changes_since_merge++;
                const int done = changes_done.fetch_add(1, std::memory_order_relaxed) + 1;
                if (!readers_go.load(std::memory_order_relaxed) && done >= warmup)
                    readers_go.store(true, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::microseconds(change_us));
            }
            readers_go.store(true, std::memory_order_release);
        });
    } else {
        for (int wi = 0; wi < writers; ++wi) {
            writer_threads.emplace_back([&, wi]() {
                std::mt19937 rng(seed + 7 + wi);
                long grant_idx = 0;
                for (int c = 0; c < per_writer && wi * per_writer + c < changes; ++c) {
                    std::lock_guard<std::mutex> lk(commit_mu);   // sequenced commit
                    apply_change(rng, grant_idx);
                }
            });
        }
        if (changes == 0)
            readers_go.store(true, std::memory_order_release);  // replay-only run
    }

    // REGRANT_CHECK: while the writers run, compare visible_locked() with the
    // published history under the same shared lock. A fast-path read that
    // disagrees with the history it is supposed to summarise is exactly the
    // regrant window of review W1 (bytes published before the history/flags).
    std::atomic<bool> sampler_done{false};
    std::thread sampler;
    if (regrant_check) {
        sampler = std::thread([&] {
            while (!sampler_done.load(std::memory_order_relaxed)) {
                // the writer may append a promoted state: take the shared side of promo_mu
                std::shared_lock<std::shared_mutex> pl(promo_mu);
                for (RoleState* st : all_states) {
                    std::shared_lock<std::shared_mutex> hl(st->hist_mu);
                    int seen = 0;
                    for (const auto& h : st->hist) {
                        if (h.second.size() < 2) continue;
                        for (const auto& e : h.second) {
                            const bool want = (e.second == 1);
                            const bool got = st->visible_locked(h.first, e.first);
                            if (got != want) {
                                ++g_regrant_violations;
                                std::cerr << "REGRANT-CHECK sampler: doc " << h.first
                                          << " pin " << e.first
                                          << " expected " << want << " got " << got << "\n";
                            }
                        }
                        if (++seen >= 32) break;
                    }
                }
                std::this_thread::yield();
            }
        });
    }

    // ---- readers -----------------------------------------------------------
    std::vector<std::vector<int>> gots(nq);
    std::vector<double> lat(nq, 0.0);
    std::vector<std::vector<double>> lat_exact_all(threads), lat_lease_all(threads);
    std::vector<Clock::time_point> t_start(threads), t_end(threads);
    std::vector<std::thread> readers;
    for (int t = 0; t < threads; ++t) {
        readers.emplace_back([&, t]() {
            while (!readers_go.load(std::memory_order_acquire)) {}
            if (read_delay_ms)
                std::this_thread::sleep_for(std::chrono::milliseconds(read_delay_ms));
            t_start[t] = Clock::now();
            const int lo = t * nq / threads;
            const int hi = (t + 1) * nq / threads;
            for (int q = lo; q < hi; ++q) {
                int role = 0;
                const bool used_lease = query_one(q, gots[q], vpins[q], lat[q], role);
                if (used_lease)
                    lat_lease_all[t].push_back(lat[q]);
                else
                    lat_exact_all[t].push_back(lat[q]);
            }
            t_end[t] = Clock::now();
        });
    }
    for (auto& th : readers) th.join();
    double read_s = 0.0, qps = 0.0;
    {
        Clock::time_point lo = t_start[0], hi = t_end[0];
        for (int i = 1; i < threads; ++i) {
            if (t_start[i] < lo) lo = t_start[i];
            if (t_end[i] > hi) hi = t_end[i];
        }
        read_s = std::chrono::duration<double>(hi - lo).count();
        // QPS from the per-query timings: read_s also covers the offline exact
        // verification used for missing/leak accounting, which is not query work.
        double lat_sum = 0.0;
        for (int q = 0; q < nq; ++q) lat_sum += lat[q];
        qps = lat_sum > 0 ? (static_cast<double>(nq) * threads * 1000.0) / lat_sum : 0.0;
    }
    for (auto& w : writer_threads) w.join();
    sampler_done.store(true, std::memory_order_relaxed);
    if (sampler.joinable()) sampler.join();
    readers_go.store(true, std::memory_order_release);   // short streams: unblock readers

    // Final reclamation below the minimum live lease (no readers left).
    long live_mismatch = 0;
    for (RoleState* st : all_states) {
        if (st->live.load(std::memory_order_relaxed) !=
            static_cast<int>(st->cur.size()))
            ++live_mismatch;                    // catches a rolled-back change that
                                                // moved live without moving cur (W2)
        cleanup_retired(st);
        gc_hist(st, min_live_a(st));
    }
    long state_mismatch = -1, visible_total = -1;
    if (selfcheck) {
        state_mismatch = 0;
        visible_total = 0;
        for (RoleState* st : all_states) {
            const uint64_t pin = st->version.load(std::memory_order_acquire);
            std::lock_guard<std::mutex> el(st->exact_mu);
            std::shared_lock<std::shared_mutex> hl(st->hist_mu);
            for (int d : st->ever) {
                const bool vis = st->visible_locked(d, pin);
                const bool in = st->cur.count(d) != 0;
                if (vis) ++visible_total;
                if (vis != in) ++state_mismatch;
            }
        }
    }
    long hist_rows = 0, cur_total = 0, log_total = 0;
    for (RoleState* st : all_states) {
        cur_total += st->live.load(std::memory_order_relaxed);
        if (auto* L = st->lease.load(std::memory_order_acquire))
            log_total += static_cast<long>(L->ring_n.load(std::memory_order_acquire));
        std::shared_lock<std::shared_mutex> lk(st->hist_mu);
        for (auto& it : st->hist) hist_rows += static_cast<long>(it.second.size());
    }

    const long reads = nq;
    const long merges = g_merges.load();
    std::vector<double> lat_e, lat_l;
    for (int i = 0; i < threads; ++i) {
        lat_e.insert(lat_e.end(), lat_exact_all[i].begin(), lat_exact_all[i].end());
        lat_l.insert(lat_l.end(), lat_lease_all[i].begin(), lat_lease_all[i].end());
    }
    const double lease_p50 = pctl(lat_l, 0.50), lease_p95 = pctl(lat_l, 0.95);
    const double exact_p50 = pctl(lat_e, 0.50), exact_p95 = pctl(lat_e, 0.95);
    std::cerr << "split lease_reads=" << lat_l.size() << " lease_p50=" << lease_p50
              << " lease_p95=" << lease_p95 << " exact_reads=" << lat_e.size()
              << " exact_p50=" << exact_p50 << " exact_p95=" << exact_p95 << "\n";
    const long cert_fb = g_cert_fallbacks.load();
    const long leaks = g_leaks.load();
    const long missing = g_missing.load();
    const long auth = g_auth.load();
    const long straddled = g_straddled.load();

    if (const char* qo = std::getenv("QUERIES_OUT")) {
        // One line per read: q pin role n doc... -- the input an external oracle
        // folds from the committed event stream (review P0-2).
        std::ofstream qf(qo);
        if (!qf) { std::cerr << "cannot write QUERIES_OUT " << qo << "\n"; return 8; }
        for (int q = 0; q < nq; ++q) {
            qf << q << " " << vpins[q] << " " << qroles[q] << " " << gots[q].size();
            for (int d : gots[q]) qf << " " << d;
            qf << "\n";
        }
    }
    std::filesystem::create_directories(outdir);
    if (const char* dl = std::getenv("DUMP_LAT")) {
        std::ofstream lf(outdir + "/lat.txt");
        for (int q = 0; q < nq; ++q) lf << q << " " << lat[q] << "\n";
    }
    {
        std::ofstream sm(outdir + "/summary.txt");
        sm << "certified reads=" << reads
           << " read_s=" << read_s << " qps=" << qps
           << " p50=" << pctl(lat, 0.50)
           << " p95=" << pctl(lat, 0.95)
           << " p99=" << pctl(lat, 0.99)
           << " leaks=" << leaks
           << " missing=" << missing
           << " auth=" << auth
           << " read_s=" << read_s << " qps=" << qps << " cert_fallbacks=" << cert_fb << " wal_refused=" << g_wal_refused.load() << " unpromoted_refused=" << g_unpromoted_change.load()
           << " merges=" << merges
           << " hist_rows=" << hist_rows << " log_len=" << log_total
           << " cur_total=" << cur_total << " replay_s=" << replay_s
           << " state_mismatch=" << state_mismatch << " visible_total=" << visible_total
           << " straddled=" << straddled << "\n";
    }
    {
        std::ofstream cm(outdir + "/cmd.txt");
        for (int i = 0; i < argc; ++i) cm << (i ? " " : "") << argv[i];
        cm << "\n";
        cm << "env CHANGE_US=" << change_us << " DELTA_CAP=" << delta_cap
           << " MERGE_EVERY=" << merge_every << " DROP_GRANT=" << drop_grant
           << " CERT=" << cert_on << " WRITERS=" << writers
           << " MMAX=" << mmax << " WARMUP=" << warmup
           << " SELFCHECK=" << selfcheck << "\n";
    }
    std::cout << "certified build_s=" << build_s
              << " reads=" << reads
              << " p50=" << pctl(lat, 0.50)
              << " p95=" << pctl(lat, 0.95)
              << " p99=" << pctl(lat, 0.99)
              << " leaks=" << leaks
              << " missing=" << missing
              << " auth=" << auth
              << " cert_fallbacks=" << cert_fb << " wal_refused=" << g_wal_refused.load() << " unpromoted_refused=" << g_unpromoted_change.load()
              << " merges=" << merges
              << " hist_rows=" << hist_rows
              << " cur_total=" << cur_total << " replay_s=" << replay_s
              << " state_mismatch=" << state_mismatch << " visible_total=" << visible_total
              << " straddled=" << straddled
              << " lease_retries=" << g_retries.load()
              << " regrant_violations=" << g_regrant_violations.load()
              << " live_mismatch=" << live_mismatch
              << " promotions=" << g_promotions.load()
              << " promote_ms=" << (g_promote_us.load() / 1000.0) << "\n";

    std::free(ballast);
    for (void* c : ballast_chunks) std::free(c);
    for (RoleState* st : all_states) {
        cleanup_retired(st);
        Lease* L = st->lease.exchange(nullptr);
        delete L->g;
        delete L;
        delete st;
    }
    return 0;
}
