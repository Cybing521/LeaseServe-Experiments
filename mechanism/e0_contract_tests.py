#!/usr/bin/env python3
"""E0-contract -- the coverage-certified lease contract, interval model.

This file used to be the version-pinned suite: nine tests that accepted a lease
only at ``e == a_L``.  It now exercises the coverage interval used by the paper:

    L = (a_L, omega_L, G, D) is usable at pin e iff a_L <= e <= omega_L and no
    grant was lost; a certified read returns W(e) cap V(r,e), and an uncertified
    read scans V(r,e) exactly.

The model is imported from ``e0_certified_tests.py`` so the two suites cannot
drift apart.  The nine tests pin the contract itself: interval boundaries, a
grant extending omega, a pinned read, a revoke advancing omega without a log
entry, a lost grant freezing it, a merge repairing the hole, a reader pinned
across a merge, a regrant across a merge, and a randomized interleaving.

Run: python3 experiments/mechanism/e0_contract_tests.py
"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from e0_certified_tests import Lease, World  # noqa: E402

CHECKS = 0
FAILED = 0


def check(name, cond, detail=""):
    global CHECKS, FAILED
    CHECKS += 1
    if not cond:
        FAILED += 1
    print(f"[{'PASS' if cond else 'FAIL'}] {name}"
          + (f"  ({detail})" if detail and not cond else ""))
    return bool(cond)


def c1_interval_bounds():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)          # a = 1
    e = w.grant("r", 2)
    L.grant(2, e)                            # omega = 2
    inside, m_in = L.read(1, w.visible("r", 1))
    below, m_below = L.read(0, w.visible("r", 0))
    above, m_above = L.read(3, w.visible("r", 3))
    ok = (m_in == "certified" and m_below == "exact" and m_above == "exact"
          and below == w.visible("r", 0) and above == w.visible("r", 3))
    return check("C1 a lease is usable exactly on [a_L, omega_L]", ok)


def c2_grant_extends_omega():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e = w.grant("r", 2)
    L.grant(2, e)
    got, mode = L.read(e, w.visible("r", e))
    ok = mode == "certified" and 2 in got and L.omega == e and L.covered(e)
    return check("C2 a grant extends omega and is searchable at the new pin", ok)


def c3_pinned_read_excludes_later_grant():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e1 = w.grant("r", 2)
    L.grant(2, e1)
    e2 = w.grant("r", 3)
    L.grant(3, e2)
    got, mode = L.read(e1, w.visible("r", e1))
    ok = mode == "certified" and 2 in got and 3 not in got
    return check("C3 a read pinned before a grant does not see it", ok)


def c4_revoke_needs_no_log():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e1 = w.grant("r", 2)
    L.grant(2, e1)                           # omega = e1
    e2 = w.revoke("r", 2)
    L.revoke(e2)                             # published epoch, no log entry
    got, mode = L.read(e2, w.visible("r", e2))
    ok = mode == "certified" and 2 not in got and L.omega == e2
    return check("C4 a revoke advances omega without a log entry", ok)


def c5_lost_grant_freezes_omega():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    before = L.omega
    e = w.grant("r", 2)
    L.grant(2, e, lose=True)
    got, mode = L.read(e, w.visible("r", e))
    ok = (not L.covered(e)) and mode == "exact" and 2 in got and L.omega == before
    return check("C5 a lost grant freezes omega and forces an exact scan", ok)


def c6_merge_repairs_hole_and_replays_suffix():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e = w.grant("r", 2)
    L.grant(2, e, lose=True)                 # hole at pin e
    L2 = L.merge(w)                          # rebuilds G at the current epoch
    got, mode = L2.read(w.epoch, w.visible("r", w.epoch))
    ok = ((not L.covered(w.epoch)) and L2.covered(w.epoch)
          and mode == "certified" and 2 in got)
    e3 = w.grant("r", 3)
    L2.grant(3, e3)                          # suffix after the merge
    got3, mode3 = L2.read(e3, w.visible("r", e3))
    ok = ok and mode3 == "certified" and 3 in got3
    return check("C6 a merge repairs the hole and replays its suffix", ok)


def c7_reader_pinned_across_merge():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e1 = w.grant("r", 2)
    L.grant(2, e1)
    L2 = L.merge(w)
    old, m_old = L.read(e1, w.visible("r", e1))
    new, m_new = L2.read(w.epoch, w.visible("r", w.epoch))
    ok = (m_old == "certified" and 2 in old
          and m_new == "certified" and 2 in new)
    return check("C7 a reader pinned before a merge keeps the old lease", ok)


def c8_regrant_across_merge():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e1 = w.grant("r", 2)
    L.grant(2, e1)
    ep = w.revoke("r", 2)
    L.revoke(ep)
    L2 = L.merge(w)                          # graph without 2
    e2 = w.grant("r", 2)                     # regrant after the merge
    L2.grant(2, e2)
    old, m_old = L.read(e1, w.visible("r", e1))
    new, m_new = L2.read(e2, w.visible("r", e2))
    ok = (m_old == "certified" and 2 in old
          and m_new == "certified" and 2 in new)
    return check("C8 a regrant after a merge keeps the old pin and the new one", ok)


def c9_random_interleaving(seed=0, steps=400):
    rng = random.Random(seed)
    w = World()
    docs = list(range(1, 9))
    w.grant("r", rng.choice(docs))
    L = Lease(w, "r", a_L=w.epoch)
    ok = True
    certified = fallback = 0
    for _ in range(steps):
        r = rng.random()
        if r < 0.45:
            d = rng.choice(docs)
            ep = w.grant("r", d)
            L.grant(d, ep, lose=(rng.random() < 0.05))
        elif r < 0.75:
            ep = w.revoke("r", rng.choice(docs))
            L.revoke(ep)
        else:
            L = L.merge(w)
        pin = w.epoch if rng.random() < 0.5 else max(0, w.epoch - rng.randint(0, 3))
        v = w.visible("r", pin)
        got, mode = L.read(pin, v)
        ok = ok and set(got) <= v                     # no leak, ever
        if mode == "certified":
            certified += 1
            ok = ok and v <= set(got)                 # certified is complete
        else:
            fallback += 1
            ok = ok and set(got) == v                 # fallback is exact
    return check("C9 random interleaving: no leak, certified complete, fallback exact",
                 ok, f"certified={certified} fallback={fallback}")


def main():
    tests = [c1_interval_bounds, c2_grant_extends_omega,
             c3_pinned_read_excludes_later_grant, c4_revoke_needs_no_log,
             c5_lost_grant_freezes_omega, c6_merge_repairs_hole_and_replays_suffix,
             c7_reader_pinned_across_merge, c8_regrant_across_merge,
             c9_random_interleaving]
    passed = sum(1 for t in tests if t())
    print(f"\n{passed}/{len(tests)} contract tests passed")
    return 0 if passed == len(tests) else 1


if __name__ == "__main__":
    raise SystemExit(main())
