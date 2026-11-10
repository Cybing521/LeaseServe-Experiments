#!/usr/bin/env python3
r"""Independent ACL oracle for lease_certified runs.

The kernel scores a read against ``exact_visible``, which walks the same
per-document ``visible()`` code the lease path filters with.  An error in that
code therefore moves the answer and the reference together and the run still
reports zero missing (review P0-2).  This checker rebuilds every read's
authorized set from the *committed event stream* only -- the base ACL plus the
``epoch kind role doc`` lines written by ``CHANGES_OUT`` -- and compares it with
the documents the kernel actually returned (``QUERIES_OUT``).

It needs no kernel code and no shared state:

    V(r, pin) = fold(base_acl[r], events of r with epoch <= pin)
    top_k      = k nearest documents of V(r, pin) to the query vector
    leaks      = returned \ V(r, pin)
    missing    = top_k \ returned

With ``--script`` it also knows which grants were injected as lost and which
epochs were merge points, so it can reproduce the AUTH/ANN split independently:

    W(L)       = V(r, a) union {grants a < epoch <= pin that are not lost}
    auth miss  = missing documents outside W(L)

Run:
    python3 oracle_check.py --base base.f32 --queries queries.f32 \
        --roles roles.bin --n 16 --dim 4 --k 3 \
        --changes changes.txt --merges merges.txt --reads queries.txt \
        [--script script.txt] [--expect-missing N --expect-auth N]
"""
import argparse
import os
import sys

import numpy as np


def read_f32(path, rows, dim):
    # Read only the prefix the run scored: base files may hold more vectors than n.
    a = np.fromfile(path, dtype=np.float32, count=rows * dim)
    if a.size != rows * dim:
        raise SystemExit(f"{path}: expected {rows * dim} floats, found {a.size}")
    return a.reshape(rows, dim)


def read_f32_rows(path, dim):
    """All rows of a raw float32 file; QUERIES_OUT may cover fewer than the file."""
    a = np.fromfile(path, dtype=np.float32)
    if a.size % dim:
        raise SystemExit(f"{path}: {a.size} floats is not a multiple of dim={dim}")
    return a.reshape(-1, dim)


def read_roles(path):
    b = np.fromfile(path, dtype=np.int32)
    if b.size < 1:
        raise SystemExit(f"{path}: empty")
    n_roles = int(b[0])
    out, off = {}, 1
    for _ in range(n_roles):
        if off + 2 > b.size:
            raise SystemExit(f"{path}: truncated header")
        rid, cnt = int(b[off]), int(b[off + 1])
        off += 2
        out[rid] = {int(x) for x in b[off:off + cnt]}
        off += cnt
    return out


def read_events(path):
    ev = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            ep, kind, role, doc = line.split()
            ev.append((int(ep), int(role), kind, int(doc)))
    return ev


def read_merges(path):
    out = []
    if not path or not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            role, a = line.split()
            out.append((int(role), int(a)))
    return out


def read_reads(path):
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            p = [int(x) for x in line.split()]
            q, pin, role, n = p[:4]
            out.append((q, pin, role, p[4:4 + n]))
    return out


def parse_script(path):
    """Return merge epochs and lost-grant epochs by simulating the kernel's allocator."""
    ep = 0
    merges = []            # (role, a)
    lost = set()           # epochs of grants whose log entry is dropped
    ops = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            p = line.split()
            ops.append(p)
            if p[0] == "merge":
                merges.append((int(p[1]), ep))
            elif p[0] in ("grant", "revoke"):
                ep += 1
                if p[0] == "grant" and len(p) > 3 and p[3] == "lost":
                    lost.add(ep)
    return merges, lost


class Acl:
    """Base ACL plus the committed event stream, folded per role."""

    def __init__(self, base_acl, events):
        self.base = base_acl
        self.events = {}
        for ep, role, kind, doc in events:
            self.events.setdefault(role, []).append((ep, kind, doc))
        for v in self.events.values():
            v.sort()

    def visible_at(self, role, pin):
        state = {d: True for d in self.base.get(role, set())}
        for ep, kind, doc in self.events.get(role, []):
            if ep > pin:
                break
            state[doc] = kind == "grant"
        return {d for d, on in state.items() if on}


def top_k(base, qv, docs, k):
    if not docs:
        return []
    idx = np.fromiter(sorted(docs), dtype=np.int64)
    d = np.sum((base[idx] - qv[None, :]) ** 2, axis=1)
    # The kernel's exact_topk() orders (distance, id) pairs, so a tied k-th
    # distance is resolved by the document id.  lexsort with the id as the
    # secondary key reproduces that order exactly; the top-k set is unique.
    order = np.lexsort((idx, d))[:k]
    return [int(idx[i]) for i in order]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--roles", required=True)
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--dim", type=int, required=True)
    ap.add_argument("--k", type=int, required=True)
    ap.add_argument("--changes", required=True)
    ap.add_argument("--merges", default="")
    ap.add_argument("--reads", required=True)
    ap.add_argument("--script", default="")
    ap.add_argument("--expect-missing", type=int, default=None)
    ap.add_argument("--expect-auth", type=int, default=None)
    a = ap.parse_args()

    base = read_f32(a.base, a.n, a.dim)
    # One read per query row, as written by QUERIES_OUT.
    reads = read_reads(a.reads)
    queries = read_f32_rows(a.queries, a.dim)
    for q, _, _, _ in reads:
        if q >= len(queries):
            raise SystemExit(f"{a.reads}: query index {q} outside {a.queries}")
    acl = Acl(read_roles(a.roles), read_events(a.changes))
    merges = read_merges(a.merges)
    merges_by_role = {}
    for role, ep in merges:
        merges_by_role.setdefault(role, []).append(ep)
    for v in merges_by_role.values():
        v.sort()

    if a.script:
        # The script's epoch simulation must agree with the kernel's traces.
        s_merges, lost = parse_script(a.script)
    else:
        s_merges, lost = [], set()

    leaks = missing = auth = 0
    rows = []
    for q, pin, role, got in reads:
        v = acl.visible_at(role, pin)
        top = top_k(base, queries[q], v, a.k)
        gs = set(got)
        lk = gs - v
        ms = [d for d in top if d not in gs]
        leaks += len(lk)
        missing += len(ms)
        au = 0
        if a.script:
            a_lease = 0
            for r_, ep_ in s_merges:
                if r_ == role and ep_ <= pin:
                    a_lease = max(a_lease, ep_)
            w = acl.visible_at(role, a_lease)
            for ep, kind, doc in acl.events.get(role, []):
                if a_lease < ep <= pin and kind == "grant" and ep not in lost:
                    w.add(doc)
            au = sum(1 for d in ms if d not in w)
            auth += au
        rows.append((q, pin, role, len(v), len(top), len(ms), len(lk), au))

    print(f"{'q':>3} {'pin':>4} {'role':>4} {'|V|':>4} {'top':>4} {'miss':>4} "
          f"{'leak':>4} {'auth':>4}")
    for r in rows:
        print(f"{r[0]:3d} {r[1]:4d} {r[2]:4d} {r[3]:4d} {r[4]:4d} {r[5]:4d} "
              f"{r[6]:4d} {r[7]:4d}")
    print(f"\nreads={len(rows)} leaks={leaks} missing={missing}"
          + (f" auth={auth}" if a.script else ""))

    bad = 0
    if leaks:
        print(f"FAIL: {leaks} returned documents are not authorized at their pin")
        bad += 1
    for label, expect, got_n in (("missing", a.expect_missing, missing),
                                 ("auth", a.expect_auth, auth)):
        if expect is not None and expect != got_n:
            print(f"FAIL: {label}={got_n}, expected {expect}")
            bad += 1
    print("PASS" if bad == 0 else "FAIL")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
