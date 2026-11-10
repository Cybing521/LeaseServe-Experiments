#!/usr/bin/env python3
"""D32: churn arms -- add the two missing competitors to D26's Table III.

Same protocol as D26 (4 readers + 1 writer, 200 changes of 5% of the role,
50/50 grant/revoke, 1M SIFT, 1000-role Uniform ACL, K=10 promoted roles,
filter against the pinned ACL).  Five arms share one schedule:

NOTE: `cur` is now the pinned snapshot.  Before 2026-11-10 it was policy.current(r),
i.e. the live ACL, which made the filtered arms measure leakage@response while the
scorer measured leakage@pin.  Results under experiments/results/churn_matrix_20261107
predate this fix and must be regenerated before they are quoted.

  static_nofilter   stale per-role index, no verification          -> leaks
  static_filter     stale per-role index + filter                  -> no leak, grants missing
  incremental       per-role index, repaired on every change       -> no leak, no missing
  shared            ONE full-corpus index + filter, zero maintenance
                    (a grant needs no index work: the document is already indexed;
                     the cost is recall under a selective filter)
  partition         co-access role groups, one index per group, repaired on change

Run (1M, llm_01):
  python3 d32_churn_arms.py --n 1000000 --dim 128 --nq 1000 --K 10 --changes 200 \
     --seed 0 --base base.f32 --queries queries.f32 \
     --roles acl/r1000_v5k/roles.bin --qroles acl/r1000_v5k/qroles.bin \
     --out out/arms_20261104/uniform_s0.json
"""
import argparse
import json
import os
import threading
import time

import numpy as np

try:
    import hnswlib
except ImportError:  # pragma: no cover
    hnswlib = None


def pctl(x, q):
    return round(float(np.percentile(x, q)), 3) if len(x) else None


def exact_topk(base, qv, docs, k):
    if len(docs) == 0:
        return []
    a = np.asarray(sorted(docs), dtype=np.int64)
    d = ((base[a] - qv) ** 2).sum(1)
    order = np.argsort(d)[: min(k, len(a))]
    return [int(a[i]) for i in order]


class Index:
    def __init__(self, dim, M=16, efc=100, efs=50):
        self.dim, self.efs = dim, efs
        self.index = hnswlib.Index(space="l2", dim=dim)
        self.n = 0

    def build(self, base, ids):
        ids = np.asarray(sorted(int(i) for i in ids), dtype=np.int64)
        self.index.init_index(max_elements=max(len(ids), 1), ef_construction=100, M=16)
        self.index.set_ef(self.efs)
        self.index.set_num_threads(4)
        self.index.add_items(base[ids], np.arange(len(ids)))
        self.ids, self.n = ids, len(ids)
        return self

    def search(self, qv, k):
        self.index.set_ef(max(k, self.efs))
        labels, _ = self.index.knn_query(qv, k=min(k, self.n))
        # labels are insertion positions; map back to document ids
        return [int(self.ids[x]) for x in labels[0]]

    def append(self, base, ids):
        ids = np.asarray(sorted(int(i) for i in ids), dtype=np.int64)
        self.index.resize_index(self.n + len(ids))
        self.index.add_items(base[ids], np.arange(self.n, self.n + len(ids)))
        self.ids = np.concatenate([self.ids, ids])
        self.n += len(ids)

    def mark(self, ids):
        pos = {int(d): i for i, d in enumerate(self.ids)}
        for d in ids:
            i = pos.get(int(d))
            if i is not None:
                try:
                    self.index.mark_deleted(i)
                except RuntimeError:
                    pass


def read_roles_bin(path):
    with open(path, "rb") as f:
        n_roles = int(np.fromfile(f, dtype=np.int32, count=1)[0])
        out = []
        for _ in range(n_roles):
            _, cnt = np.fromfile(f, dtype=np.int32, count=2)
            out.append(np.fromfile(f, dtype=np.int32, count=int(cnt)))
    return out


def read_qroles(path):
    with open(path, "rb") as f:
        n = int(np.fromfile(f, dtype=np.int32, count=1)[0])
        return np.fromfile(f, dtype=np.int32, count=n)


def co_access_groups(role_docs, n, groups, seed=0):
    """Greedy co-access grouping with a per-group union cap."""
    doc_roles = [[] for _ in range(n)]
    for r, docs in enumerate(role_docs):
        for d in docs:
            doc_roles[int(d)].append(r)
    pair = {}
    for rs in doc_roles:
        for i in range(len(rs)):
            for j in range(i + 1, len(rs)):
                key = (rs[i], rs[j]) if rs[i] < rs[j] else (rs[j], rs[i])
                pair[key] = pair.get(key, 0) + 1
    order = sorted(range(len(role_docs)), key=lambda r: -len(role_docs[r]))
    cap = int(n * 1.3 / groups)
    gset, gmem = [], []
    role_group = {}
    for r in order:
        docs = set(int(x) for x in role_docs[r])
        if not docs:
            continue
        best, best_s = -1, -1.0
        for g in range(len(gset)):
            grow = len(docs - gset[g])
            if len(gset[g]) + grow > cap:
                continue
            s = 0.0
            for m in gmem[g]:
                key = (r, m) if r < m else (m, r)
                s += pair.get(key, 0)
            if s > best_s:
                best, best_s = g, s
        if best < 0:
            gset.append(set())
            gmem.append([])
            best = len(gset) - 1
        gset[best] |= docs
        gmem[best].append(r)
        role_group[r] = best
    return gset, role_group


class Policy:
    def __init__(self, role_docs, n):
        self.lock = threading.Lock()
        self.version = 0
        self.n = n
        self.docs = {r: frozenset(int(x) for x in d) for r, d in enumerate(role_docs) if len(d) > 0}
        self.history = {r: [(0, self.docs[r])] for r in self.docs}

    def snapshot_locked(self, r, v):
        for ver, fs in reversed(self.history[r]):
            if ver <= v:
                return fs
        return frozenset()

    def change(self, r, op, frac, rng):
        with self.lock:
            cur = self.docs[r]
            if not cur:
                return None
            k = max(1, int(len(cur) * frac))
            if op == "revoke":
                drop = rng.choice(list(cur), size=min(k, len(cur)), replace=False)
                new = cur - set(int(x) for x in drop)
                changed = [int(x) for x in drop]
            else:
                pool = np.setdiff1d(np.arange(self.n), np.fromiter(cur, dtype=np.int64))
                if len(pool) == 0:
                    return None
                add = rng.choice(pool, size=min(k, len(pool)), replace=False)
                new = cur | set(int(x) for x in add)
                changed = [int(x) for x in add]
            self.version += 1
            self.docs[r] = new
            self.history[r].append((self.version, new))
            return self.version, changed

    def current(self, r):
        with self.lock:
            return self.docs[r]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=1000000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--nq", type=int, default=1000)
    ap.add_argument("--K", type=int, default=10)
    ap.add_argument("--changes", type=int, default=200)
    ap.add_argument("--grant-ratio", type=float, default=0.5)
    ap.add_argument("--revoke-frac", type=float, default=0.05)
    ap.add_argument("--hot-frac", type=float, default=0.8)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--write-sleep", type=float, default=0.0005)
    ap.add_argument("--groups", type=int, default=8)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--base", required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--roles", required=True)
    ap.add_argument("--qroles", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--ef", type=int, default=20, help="candidate multiplier over k")
    a = ap.parse_args()

    base = np.fromfile(a.base, dtype=np.float32).reshape(-1, a.dim)
    qs = np.fromfile(a.queries, dtype=np.float32).reshape(-1, a.dim)
    role_docs = read_roles_bin(a.roles)
    qroles_all = read_qroles(a.qroles)
    nq = min(a.nq, len(qroles_all))
    qroles = qroles_all[:nq].astype(int)

    counts = np.bincount(qroles, minlength=len(role_docs))
    ranked = sorted([r for r in range(len(role_docs)) if len(role_docs[r]) > 0],
                    key=lambda r: -counts[r])
    promoted = set(ranked[: a.K])
    hot = ranked[: max(a.K, 1)]

    t0 = time.time()
    static = {r: Index(a.dim).build(base, role_docs[r]) for r in promoted}
    inc = {r: Index(a.dim).build(base, role_docs[r]) for r in promoted}
    shared = Index(a.dim).build(base, range(a.n))
    gsets, role_group = co_access_groups(role_docs, a.n, a.groups)
    part = {g: Index(a.dim).build(base, gsets[g]) for g in range(len(gsets))}
    build_s = time.time() - t0
    union_total = sum(len(s) for s in gsets)

    policy = Policy(role_docs, a.n)
    inc_lock = threading.Lock()
    part_lock = threading.Lock()
    part_index = {g: set(int(x) for x in gsets[g]) for g in range(len(gsets))}
    barrier = threading.Barrier(a.threads + 1)
    results, rlock = [], threading.Lock()

    def writer():
        barrier.wait()
        wr = np.random.default_rng(a.seed + 7)
        for _ in range(a.changes):
            r = int(wr.choice(hot)) if wr.random() < a.hot_frac else int(wr.choice(list(policy.docs)))
            op = "grant" if wr.random() < a.grant_ratio else "revoke"
            res = policy.change(r, op, a.revoke_frac, wr)
            if res is not None:
                _, changed = res
                if r in inc:
                    with inc_lock:
                        if op == "revoke":
                            inc[r].mark(changed)
                        else:
                            inc[r].append(base, changed)
                if r in role_group:
                    g = role_group[r]
                    with part_lock:
                        if op == "revoke":
                            part[g].mark(changed)
                        else:
                            new = [d for d in changed if d not in part_index[g]]
                            if new:
                                part[g].append(base, new)
                                part_index[g].update(new)
            time.sleep(a.write_sleep)

    def reader(t):
        barrier.wait()
        lo = t * nq // a.threads
        hi = (t + 1) * nq // a.threads
        for i in range(lo, hi):
            r = int(qroles[i])
            qv = qs[i % len(qs)]
            with policy.lock:
                v = policy.version
                snap = policy.snapshot_locked(r, v)
            # Filter with the pinned snapshot, not the live ACL: every arm is scored
            # for leaks and missing against V(r, v), so filtering with current(r) would
            # mix leakage@response into a leakage@pin number.  (d33 already does this.)
            cur = snap
            # static arms
            if r in static:
                t0 = time.perf_counter()
                got = static[r].search(qv, a.K * a.ef)[: a.K]
                ms = (time.perf_counter() - t0) * 1e3
                base_got = got
            else:
                t0 = time.perf_counter()
                base_got = exact_topk(base, qv, snap, a.K)
                ms = (time.perf_counter() - t0) * 1e3
            with rlock:
                results.append(("static_nofilter", i, r, v, base_got, ms))
                results.append(("static_filter", i, r, v, [x for x in base_got if x in cur][: a.K], ms))
            # incremental
            if r in inc:
                t0 = time.perf_counter()
                with inc_lock:
                    got = inc[r].search(qv, a.K * a.ef)
                got = [x for x in got if x in cur][: a.K]
                ms = (time.perf_counter() - t0) * 1e3
            else:
                got = exact_topk(base, qv, snap, a.K)
            with rlock:
                results.append(("incremental", i, r, v, got, ms))
            # shared: full-corpus index, filter only
            t0 = time.perf_counter()
            got = shared.search(qv, a.K * a.ef)
            got = [x for x in got if x in cur][: a.K]
            ms = (time.perf_counter() - t0) * 1e3
            with rlock:
                results.append(("shared", i, r, v, got, ms))
            # partition
            if r in role_group:
                g = role_group[r]
                t0 = time.perf_counter()
                with part_lock:
                    got = part[g].search(qv, a.K * a.ef)
                got = [x for x in got if x in cur][: a.K]
                ms = (time.perf_counter() - t0) * 1e3
            else:
                got = exact_topk(base, qv, snap, a.K)
                ms = 0.0
            with rlock:
                results.append(("partition", i, r, v, got, ms))

    threads = [threading.Thread(target=writer)]
    threads += [threading.Thread(target=reader, args=(t,)) for t in range(a.threads)]
    t_start = time.time()
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    run_s = time.time() - t_start

    # score against the exact truth of the version each read pinned
    truth_cache = {}

    def truth(i, r, v):
        key = (i, r, v)
        if key not in truth_cache:
            with policy.lock:
                snap = policy.snapshot_locked(r, v)
            truth_cache[key] = set(exact_topk(base, qs[i % len(qs)], snap, a.K))
        return truth_cache[key]

    stats = {x: dict(leaks=0, missing=0, reads=0, lat=[]) for x in
             ("static_nofilter", "static_filter", "incremental", "shared", "partition")}
    for arm, i, r, v, got, ms in results:
        tset = truth(i, r, v)
        with policy.lock:
            snap = policy.snapshot_locked(r, v)
        got_set = set(got)
        stats[arm]["reads"] += 1
        stats[arm]["lat"].append(ms)
        stats[arm]["leaks"] += len([x for x in got if x not in snap])
        stats[arm]["missing"] += len([x for x in tset if x not in got_set])

    out = dict(n=a.n, nq=nq, K=a.K, changes=a.changes, seed=a.seed,
               build_s=round(build_s, 2), run_s=round(run_s, 2),
               groups=len(gsets), memory=round(union_total / a.n, 3),
               arms={})
    for arm, s in stats.items():
        out["arms"][arm] = dict(reads=s["reads"], leaks=s["leaks"], missing=s["missing"],
                                p50=pctl(s["lat"], 50), p95=pctl(s["lat"], 95),
                                mem=1.0 if arm == "shared" else None)
    out["arms"]["static_nofilter"]["mem"] = round(sum(len(role_docs[r]) for r in promoted) / a.n, 3)
    out["arms"]["static_filter"]["mem"] = out["arms"]["static_nofilter"]["mem"]
    out["arms"]["incremental"]["mem"] = out["arms"]["static_nofilter"]["mem"]
    out["arms"]["partition"]["mem"] = out["memory"]
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w") as fh:
        json.dump(out, fh, indent=2)
    print(json.dumps(out["arms"], indent=1))


if __name__ == "__main__":
    main()
