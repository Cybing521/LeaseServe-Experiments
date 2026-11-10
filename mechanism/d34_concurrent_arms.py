#!/usr/bin/env python3
"""D34: CONCURRENT replay -- lag becomes queueing, auth becomes real.

d33 replays events sequentially, so a repairing arm never falls behind:
its auth axis is trivially 0 and its "lag" is only the per-event repair
wall time.  d34 runs 4 reader threads + 1 writer thread against the SAME
event stream with REAL wall-clock spacing (compressed by --time-scale):

  writer   sleeps until each event's scheduled instant, repairs, and
           records lag_queue(e) = t_done(e) - t_commit(e) -- the delay
           until the index actually reflects the change, INCLUDING time
           queued behind earlier repairs.
  readers  continuously query (pinned version, pinned-visible filter),
           scored per read against the pinned exact truth.  The axis split
           differs from d33/correctness.py: here a missing document is `sel`
           when it was in the arm's raw candidate list and `ann` otherwise,
           whereas d33 splits on membership in the candidate set C* after
           budget truncation.  The two definitions disagree on documents that
           survive the budget by distance but are not in raw; compare axes
           within one harness, not across.

Core new signal: under backlog (repair time > event interval), the lease
arm's auth axis becomes POSITIVE -- a grant is committed but the insert
has not landed while readers keep querying.  d33 structurally cannot show
this; d34 turns the lag story into a correctness story.

Scenarios (wiki-100k vectors):
  uniform ACL, slow clock  -> repairs keep up (auth stays ~0)
  erbac ACL,  fast clock   -> big roles, repair > interval -> backlog

Run (mimo, /root/work/icde/b1):
  python3 d34_concurrent_arms.py --n 100000 --dim 128 --K 10 --threads 4 \
    --time-scale 1.0 --base base.f32 --queries queries.f32 \
    --roles acl_r100_v1k/roles.bin --qroles acl_r100_v1k/qroles.bin \
    --events stream_s0/events.jsonl --out out/conc_uniform_ts1.0.json
"""
import argparse
import json
import os
import threading
import time

import numpy as np

from d32_churn_arms import co_access_groups, exact_topk, read_qroles, read_roles_bin
from correctness import ArmStats

import hnswlib


class CIdx:
    """Concurrent-safe index wrapper: hnswlib labels ARE document ids, so the
    reader never maps labels through a mutable array (the d32 wrapper's
    label->ids mapping is the race that crashed concurrent runs).
    add_items with unique labels and knn_query are safe to run concurrently
    per hnswlib docs; mark_deleted/unmark_deleted are serialized behind the
    writer's own single thread."""

    def __init__(self, dim, ef=50):
        self.idx = hnswlib.Index(space="l2", dim=dim)
        self.ef = ef
        self.present, self.marked = set(), set()
        self.lock = threading.Lock()  # serializes writer-side mutations only

    def build(self, base, ids, threads=8, headroom=None):
        if headroom is None:
            # grants over a long stream can reach tens of thousands per role;
            # scale the reservation with the role (25% covers a 200-event
            # stream at frac=0.01 on a 1M-scale role)
            headroom = max(80000, len(ids) // 4)
        ids = np.asarray(sorted(int(i) for i in ids), dtype=np.int64)
        # Pre-allocate room for future grants: resize_index while readers are
        # querying is the crash window (cand error / munmap), so we never
        # resize at runtime.
        self.idx.init_index(max_elements=len(ids) + headroom, ef_construction=100, M=16)
        self.idx.set_num_threads(threads)
        self.idx.add_items(base[ids], ids)
        self.present = set(int(x) for x in ids)
        return self

    def search(self, qv, C):
        avail = self.idx.get_current_count()
        labels, _ = self.idx.knn_query(qv, k=min(C, max(avail, 1)))
        return [int(x) for x in labels[0]]

    def add(self, base, docs):
        with self.lock:
            new = [int(x) for x in docs if int(x) not in self.present]
            if new:
                self.idx.add_items(base[np.asarray(new, dtype=np.int64)],
                                   np.asarray(new, dtype=np.int64))
                self.present.update(new)
            for x in docs:
                x = int(x)
                if x in self.marked:
                    self.idx.unmark_deleted(x)
                    self.marked.discard(x)

    def revoke(self, docs):
        with self.lock:
            for x in docs:
                x = int(x)
                if x in self.present and x not in self.marked:
                    try:
                        self.idx.mark_deleted(x)
                    except RuntimeError:
                        pass
                    self.marked.add(x)

    def searchable(self):
        return self.present - self.marked


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--K", type=int, default=10)
    ap.add_argument("--ef", type=int, default=20)
    ap.add_argument("--groups", type=int, default=8)
    ap.add_argument("--threads", type=int, default=4, help="reader threads")
    ap.add_argument("--time-scale", type=float, default=1.0,
                    help="real seconds per logical ms (1.0 = t_ms as milliseconds)")
    ap.add_argument("--max-reads", type=int, default=60000, help="per-arm read cap")
    ap.add_argument("--base", required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--roles", required=True)
    ap.add_argument("--qroles", required=True)
    ap.add_argument("--events", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    base = np.fromfile(a.base, dtype=np.float32).reshape(-1, a.dim)
    qs = np.fromfile(a.queries, dtype=np.float32).reshape(-1, a.dim)
    role_docs = read_roles_bin(a.roles)
    qroles_all = read_qroles(a.qroles).astype(int)
    events = [json.loads(line) for line in open(a.events) if line.strip()]
    k = a.K

    counts = np.bincount(qroles_all, minlength=len(role_docs))
    ranked = sorted([r for r in range(len(role_docs)) if len(role_docs[r]) > 0],
                    key=lambda r: -counts[r])
    promoted = set(ranked[: a.K])

    class Policy:
        def __init__(self):
            self.lock = threading.Lock()
            self.version = 0
            self.state = {r: set(int(x) for x in role_docs[r]) for r in range(len(role_docs))}
            self.history = {r: [(0, frozenset(self.state[r]))] for r in promoted}

        def apply(self, r, op, docs, version):
            with self.lock:
                self.state[r] = (self.state[r] | set(docs)) if op == "grant" else (self.state[r] - set(docs))
                self.version = version
                if r in promoted:
                    self.history[r].append((version, frozenset(self.state[r])))

        def pin(self, r):
            with self.lock:
                v = self.version
                for ver, fs in reversed(self.history[r]):
                    if ver <= v:
                        return v, fs
                return v, frozenset()

    policy = Policy()

    t0 = time.time()
    shared_idx = CIdx(a.dim).build(base, range(a.n))
    static_idx, lease_idx = {}, {}
    for r in promoted:
        static_idx[r] = CIdx(a.dim).build(base, role_docs[r])
        lease_idx[r] = CIdx(a.dim).build(base, role_docs[r])
    gsets, role_group = co_access_groups(role_docs, a.n, a.groups)
    part_idx = {g: CIdx(a.dim).build(base, gsets[g]) for g in range(len(gsets))}
    build_s = round(time.time() - t0, 2)

    doc_roles = {}
    for r in set(role_group):
        for d_ in role_docs[r]:
            doc_roles.setdefault(int(d_), set()).add(r)

    stats = {arm: ArmStats(arm) for arm in ("shared", "static", "lease", "partition")}
    lag_q = {arm: [] for arm in ("lease", "partition")}
    backlog = {arm: 0 for arm in ("lease", "partition")}
    truth_cache = {}
    truth_lock = threading.Lock()

    obs_q = [(i, int(qroles_all[i])) for i in range(len(qroles_all))
             if int(qroles_all[i]) in promoted][:500]

    def do_repair(arm, op, r, docs):
        """Repair ops; returns repair wall seconds. Static/shared: no-op."""
        if arm == "lease" and r in promoted:
            t = time.perf_counter()
            if op == "grant":
                lease_idx[r].add(base, docs)
            else:
                lease_idx[r].revoke(docs)
            return time.perf_counter() - t
        if arm == "partition" and r in role_group:
            g = role_group[r]
            t = time.perf_counter()
            if op == "grant":
                part_idx[g].add(base, docs)
            else:
                drop = [d_ for d_ in docs
                        if not any(d_ in policy.state[rr] for rr in doc_roles.get(d_, ())
                                   if rr != r and role_group.get(rr) == g)]
                part_idx[g].revoke(drop)
            return time.perf_counter() - t
        return 0.0

    def searchable(arm, r):
        if arm == "shared":
            return None  # everything
        if arm == "static":
            return static_idx[r].present
        if arm == "lease":
            return lease_idx[r].searchable()
        return part_idx[role_group[r]].searchable()

    stop = threading.Event()
    counters = {arm: [0, 0] for arm in stats}  # [reads, leaks] atomic via GIL ops
    missing_acc = {arm: dict(total=0, auth=0, ann=0, sel=0) for arm in stats}

    def reader(tid):
        j = tid
        while not stop.is_set() and counters["lease"][0] < a.max_reads:
            i, r = obs_q[j % len(obs_q)]
            j += 1
            qv = qs[i % len(qs)]
            v, snap = policy.pin(r)
            for arm in stats:
                s = searchable(arm, r)
                C = k * a.ef
                if arm in ("static", "lease") and s is not None:
                    C = min(C, max(k, int(0.8 * len(s))))
                t = time.perf_counter()
                if arm == "shared":
                    raw = shared_idx.search(qv, C)
                elif arm == "static":
                    raw = static_idx[r].search(qv, C)
                elif arm == "lease":
                    raw = lease_idx[r].search(qv, C)
                else:
                    raw = part_idx[role_group[r]].search(qv, C)
                ms = (time.perf_counter() - t) * 1e3
                got = [x for x in raw if x in snap][:k]
                key = (i, r, v)
                with truth_lock:
                    if key not in truth_cache:
                        truth_cache[key] = frozenset(exact_topk(base, qv, snap, k))
                    truth = truth_cache[key]
                got_set = set(got)
                raw_set = set(raw)
                st = stats[arm]
                st.reads += 1
                st.leaks += sum(1 for x in got if x not in snap)
                for d_ in truth:
                    if d_ in got_set:
                        continue
                    if s is None or d_ in s:
                        if d_ in raw_set:
                            missing_acc[arm]["sel"] += 1
                        else:
                            missing_acc[arm]["ann"] += 1
                    else:
                        missing_acc[arm]["auth"] += 1
                    missing_acc[arm]["total"] += 1
                st.lat.append(ms)
                counters[arm][0] += 1

    def writer():
        t_start = time.time()
        last_done = {arm: None for arm in lag_q}
        for e in events:
            t_sched = t_start + e["t_ms"] * a.time_scale / 1000.0
            now = time.time()
            if t_sched > now:
                time.sleep(t_sched - now)
            t_commit = time.time()
            policy.apply(int(e["role"]), e["op"], e["doc_ids"], int(e["version"]))
            for arm in ("lease", "partition"):
                dt = do_repair(arm, e["op"], int(e["role"]), e["doc_ids"])
                t_done = time.time()
                lag_q[arm].append((t_done - t_commit) * 1e3)
                if last_done[arm] is not None and t_commit < last_done[arm]:
                    backlog[arm] += 1
                last_done[arm] = t_done
        # let readers drain up to 2s past the last commit
        stop.wait(2.0)
        stop.set()

    th_w = threading.Thread(target=writer)
    th_r = [threading.Thread(target=reader, args=(t,)) for t in range(a.threads)]
    run_start = time.time()
    for th in [th_w] + th_r:
        th.start()
    for th in [th_w] + th_r:
        th.join()
    run_s = round(time.time() - run_start, 2)

    def pct(xs, q):
        return round(float(np.percentile(xs, q)), 3) if xs else None

    out = dict(
        protocol="b1_concurrent_v1", n=a.n, dim=a.dim, k=k, ef=a.ef,
        groups=len(gsets), memory=round(sum(len(s) for s in gsets) / a.n, 3),
        time_scale=a.time_scale,
        interval_eff_ms=round((events[1]["t_ms"] - events[0]["t_ms"]) * a.time_scale, 3)
        if len(events) > 1 else None,
        n_events=len(events), build_s=build_s, run_s=run_s,
        events_file=os.path.abspath(a.events),
        writer={arm: dict(lag_queue_p50=pct(v, 50), lag_queue_p95=pct(v, 95),
                          lag_queue_max=pct(v, 100), backlog_events=backlog[arm])
                for arm, v in lag_q.items()},
        arms={},
    )
    for arm in stats:
        st = stats[arm]
        m = missing_acc[arm]
        out["arms"][arm] = dict(
            reads=st.reads, leaks=st.leaks,
            leak_rate=round(st.leaks / st.reads, 6) if st.reads else 0.0,
            missing=m,
            p50=(round(float(np.percentile(st.lat, 50)), 3) if st.lat else None),
            p95=(round(float(np.percentile(st.lat, 95)), 3) if st.lat else None),
        )
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.out, "w") as fh:
        json.dump(out, fh, indent=2)
    print(json.dumps(dict(time_scale=a.time_scale,
                          writer=out["writer"],
                          arms={arm: {kk: vv for kk, vv in d.items() if kk != "p95"}
                                for arm, d in out["arms"].items()}), indent=1))


if __name__ == "__main__":
    main()
