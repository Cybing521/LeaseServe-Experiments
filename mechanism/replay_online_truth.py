#!/usr/bin/env python3
"""Offline truth for the 30k online lease run (review 1/3: same-trajectory oracle).

The performance run records, per read, the pin and the returned documents
(QUERIES_OUT) and, per applied change, its committed epoch (CHANGES_OUT).  This
script folds the *event stream* -- not the kernel's visibility code -- into the
role's visible set at each pin, computes the exact top-k with numpy, and scores
the recorded answers.  A missed document counts as auth only when it is not a node
of the lease graph (see the comment in main); the run must have merges=0.  Latency is untouched: the run that is scored is the same
run whose p50/p95 are reported.

Usage: replay_online_truth.py base.f32 queries.f32 roles.bin reads.txt changes.txt
"""
import sys
import numpy as np

BASE, QUERIES, ROLES, READS, CHANGES = sys.argv[1:6]
DIM, K = 128, 10


def load_roles(path):
    raw = open(path, 'rb').read()
    off = 0

    def i32():
        nonlocal off
        v = int(np.frombuffer(raw, dtype=np.int32, count=1, offset=off)[0])
        off += 4
        return v
    n_roles = i32()
    ids = [[] for _ in range(n_roles)]
    for _ in range(n_roles):
        rid, cnt = i32(), i32()
        ids[rid] = np.frombuffer(raw, dtype=np.int32, count=cnt, offset=off).copy()
        off += 4 * cnt
    return ids


def main():
    base = np.fromfile(BASE, dtype=np.float32).reshape(-1, DIM)
    queries = np.fromfile(QUERIES, dtype=np.float32).reshape(-1, DIM)
    ids = load_roles(ROLES)

    changes = []
    for line in open(CHANGES):
        ep, kind, role, doc = line.split()
        changes.append((int(ep), kind, int(role), int(doc)))
    changes.sort(key=lambda x: x[0])

    reads = []
    for line in open(READS):
        f = line.split()
        q, pin, role, n = int(f[0]), int(f[1]), int(f[2]), int(f[3])
        reads.append((q, pin, role, [int(x) for x in f[4:4 + n]]))

    churn_role = reads[0][2]
    # G's node set.  The lease graph is built over the role's initial visible set and,
    # with no merge in the run (the caller checks merges=0 in summary.txt), never
    # gains a node.  A document outside `graph` can reach a read only through the log.
    graph = set(int(x) for x in ids[churn_role])

    by_pin = {}
    for q, pin, role, got in reads:
        by_pin.setdefault(pin, []).append((q, got))

    # Attribution follows the candidate-coverage definition, from observed answers:
    #   auth  = an exact top-k document that is NOT a node of G and was not returned
    #           (upper bound under ties; auth_strict counts only distance < d_k).
    #           The log scan is exact and TopK is exact over C∩S, so a true top-k
    #           document held by the log is always returned; a miss outside G therefore
    #           means no candidate source held it at that pin.
    #   other = a missed document that IS a node of G: the graph search (ann) or the
    #           candidate budget (sel) lost it.  The two are not separated here.
    # log_truth / log_returned count the non-vacuous cases: exact top-k documents that
    # only the log could have supplied, and how many of them the read returned.
    # Ties: SIFT is integer-valued, so distances are exact and several documents can
    # share the k-th distance.  Scoring is tie-aware: T is every visible document at
    # distance <= d_k, a returned document is a hit iff it is in T, and
    # missing = k - hits.  (A fixed top-k set would make the count depend on which of
    # the tied documents numpy happens to pick.)  `strict` restricts to distance < d_k,
    # where membership in the exact top-k is unambiguous.
    missing = auth = auth_strict = other = leaks = checked = tied_reads = 0
    log_truth = log_returned = reads_with_log_doc = 0
    for pin in sorted(by_pin):
        vis = set(graph)
        for ep, kind, r, doc in changes:
            if ep > pin:
                break
            if r == churn_role:
                if kind == 'grant':
                    vis.add(doc)
                else:
                    vis.discard(doc)
        vis_arr = np.array(sorted(vis), dtype=np.int64)
        in_graph = np.array([int(d) in graph for d in vis_arr])
        V = base[vis_arr].astype(np.float64)
        vn = (V * V).sum(1)
        group = by_pin[pin]
        kk = min(K, V.shape[0])
        for start in range(0, len(group), 256):
            part = group[start:start + 256]
            chunk = np.stack([queries[q] for q, _ in part]).astype(np.float64)
            d2 = (chunk * chunk).sum(1)[:, None] + vn[None, :] - 2.0 * chunk @ V.T
            dk = np.partition(d2, kk - 1, axis=1)[:, kk - 1]
            for row, (q, got) in enumerate(part):
                gotset = set(got)
                leaks += len(gotset - vis)
                le = d2[row] <= dk[row]
                lt = d2[row] < dk[row]
                T = set(int(x) for x in vis_arr[le])
                tied_reads += 1 if len(T) > kk else 0
                hits = min(kk, len(T & gotset))
                m = kk - hits
                checked += kk
                missing += m
                log_T = set(int(x) for x in vis_arr[le & ~in_graph])
                log_strict = set(int(x) for x in vis_arr[lt & ~in_graph])
                log_truth += len(log_T)
                log_returned += len(log_T & gotset)
                reads_with_log_doc += 1 if log_T else 0
                a_up = min(m, len(log_T - gotset))
                auth += a_up
                auth_strict += len(log_strict - gotset)
                other += m - a_up
    n = len(reads)
    pre = sum(1 for ep, _, r, _ in changes if r == churn_role and ep <= min(by_pin))
    print(f"replay reads={n} checked={checked} missing={missing} auth={auth} "
          f"auth_strict={auth_strict} ann_or_sel={other} leaks={leaks} "
          f"recall={1.0 - missing / max(1, checked):.6f}")
    print(f"log_only_truth={log_truth} log_only_returned={log_returned} "
          f"reads_with_log_only_truth={reads_with_log_doc} reads_with_tie_at_k={tied_reads}")
    print(f"pins={len(by_pin)} first_pin={min(by_pin)} last_pin={max(by_pin)} "
          f"changes={len(changes)} committed_before_first_read={pre} churn_role={churn_role}")


if __name__ == '__main__':
    main()
