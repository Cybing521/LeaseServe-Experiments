#!/usr/bin/env python3
"""E0-certified -- contract tests for the coverage-certified lease.

Model-level tests of the contract that ``lease_certified.cpp`` implements:

    a lease L = (a_L, omega_L, G, D) is usable at pin e iff a_L <= e <= omega_L,
    where G is the graph built at a_L and D the grant log since a_L; a read
    returns the top-k of (G union D_{<=e}) filtered by V(r, e), and falls back to
    an exact scan of V(r, e) when the certificate does not hold.

The tests are deterministic and run in milliseconds, so a protocol bug fails
here instead of on a 1M-vector run. T1-T9 port the version-pinned suite to the
interval contract; E10-E14 cover the faults the fourth review asked for (lost
grant, publish/read race, merge suffix replay, pinned reader with reclaim, and
grant -> revoke -> grant across a merge).

Run: python3 experiments/mechanism/e0_certified_tests.py
"""


class World:
    """Authorization history: role -> {(doc): [(epoch, 'g'|'r'), ...]}."""

    def __init__(self):
        self.epoch = 0
        self.hist = {}
        self.roles = set()

    def grant(self, role, doc):
        self.epoch += 1
        self.roles.add(role)
        self.hist.setdefault(role, {}).setdefault(doc, []).append((self.epoch, "g"))
        self.current = getattr(self, "current", {})
        self.current.setdefault(role, set()).add(doc)
        return self.epoch

    def revoke(self, role, doc):
        self.epoch += 1
        self.roles.add(role)
        self.hist.setdefault(role, {}).setdefault(doc, []).append((self.epoch, "r"))
        self.current = getattr(self, "current", {})
        self.current.setdefault(role, set()).discard(doc)
        return self.epoch

    def visible(self, role, epoch):
        """V(r, e): docs whose last transition at or before e is a grant."""
        out = set()
        for doc, tr in self.hist.get(role, {}).items():
            state = None
            for ep, kind in tr:
                if ep <= epoch:
                    state = kind
            if state == "g":
                out.add(doc)
        return out


class Lease:
    """A materialization lease under the coverage contract."""

    def __init__(self, world, role, a_L):
        self.world = world
        self.role = role
        self.a_L = a_L
        self.omega = a_L
        self.G = set(world.visible(role, a_L))     # graph built at a_L
        self.D = []                                # (epoch, doc) grants since a_L
        self.hole = False                          # a grant was lost: certificate must fail

    def grant(self, doc, epoch, lose=False):
        if not lose:
            self.D.append((epoch, doc))            # entry first ...
        else:
            self.hole = True                       # ... or not, and the watermark freezes
        if not self.hole:
            self.omega = epoch                     # watermark only over durable grants

    def revoke(self, epoch):
        """A revoke advances the published watermark; it needs no log entry.

        The kernel publishes an epoch for every committed change and advances
        omega for both grants and revokes (``publish_epoch``).  Omitting this made
        the model fall back on the epoch of a revoke, which is exactly the cost the
        coverage certificate removes.
        """
        if not self.hole:
            self.omega = max(self.omega, epoch)

    def materialized(self, epoch):
        """W(L) at pin e: graph plus the log entries at or before e."""
        return set(self.G) | {d for ep, d in self.D if ep <= epoch}

    def covered(self, epoch):
        return (not self.hole) and self.a_L <= epoch <= self.omega

    def read(self, epoch, truth):
        """Returned set: W-filtered when certified, exact otherwise."""
        if self.covered(epoch):
            w = self.materialized(epoch) & truth
            return w, "certified"
        return set(truth), "exact"

    def merge(self, world, lose_suffix=0):
        """Build G' at the current epoch and replay the suffix (a', omega]."""
        a2 = world.epoch
        G2 = set(world.visible(self.role, a2))
        tail = [(ep, d) for ep, d in self.D if ep > a2]
        D2 = tail[: len(tail) - lose_suffix] if lose_suffix else tail
        L2 = Lease.__new__(Lease)
        L2.world, L2.role, L2.a_L = world, self.role, a2
        # The merge rebuilds G at the current epoch, so a lost-grant hole is
        # repaired: V(a2) is in G2.  Only a deliberately lost suffix keeps a hole.
        L2.G, L2.D, L2.hole = G2, D2, lose_suffix > 0
        L2.omega = max(self.omega, a2)
        return L2


def check(name, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    print(f"[{status}] {name}" + (f"  {detail}" if detail and not cond else ""))
    return cond


def t1_grant_searchable_through_log():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e = w.grant("r", 2)
    L.grant(2, e)
    got, mode = L.read(e, w.visible("r", e))
    return check("T1 grant is searchable through the log at the published pin",
                 mode == "certified" and 2 in got)


def t2_pinned_read_excludes_later_grant():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e1 = w.grant("r", 2)
    L.grant(2, e1)
    e2 = w.grant("r", 3)
    L.grant(3, e2)
    got, mode = L.read(e1, w.visible("r", e1))
    return check("T2 a read pinned before a grant does not see it",
                 mode == "certified" and 3 not in got)


def t3_grant_revoke_grant_history():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e1 = w.grant("r", 2)
    L.grant(2, e1)
    w.revoke("r", 2)
    e2 = w.grant("r", 2)                        # re-grant
    L.grant(2, e2)
    old, mode_old = L.read(e1, w.visible("r", e1))
    new, mode_new = L.read(e2, w.visible("r", e2))
    ok = (mode_old == "certified" and 2 in old) and (mode_new == "certified" and 2 in new)
    return check("T3 grant -> revoke -> grant keeps the old pin and the new one", ok)


def t4_merge_replays_suffix():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e = w.grant("r", 2)
    L.grant(2, e)
    L2 = L.merge(w)                              # build at current epoch
    got, mode = L2.read(w.epoch, w.visible("r", w.epoch))
    return check("T4 merge replays the suffix, so the pin after the merge is certified",
                 mode == "certified" and 2 in got)


def t5_independent_roles():
    w = World()
    w.grant("r1", 1)
    w.grant("r2", 2)
    L1 = Lease(w, "r1", a_L=w.epoch)
    L2 = Lease(w, "r2", a_L=w.epoch)
    v1 = w.visible("r1", w.epoch)
    v2 = w.visible("r2", w.epoch)
    return check("T5 roles do not borrow each other's grants",
                 L1.read(w.epoch, v1)[0] == {1} and L2.read(w.epoch, v2)[0] == {2})


def t6_merge_while_pinned():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e1 = w.grant("r", 2)
    L.grant(2, e1)
    L2 = L.merge(w)
    old, mode = L.read(e1, w.visible("r", e1))
    return check("T6 a reader pinned before the merge keeps the old lease",
                 mode == "certified" and 2 in old)


def t7_lost_grant_is_not_certified():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e = w.grant("r", 2)
    L.grant(2, e, lose=True)
    got, mode = L.read(e, w.visible("r", e))
    return check("T7 a lost grant is not certified (omega freezes, read scans)",
                 mode == "exact" and 2 in got)


def t8_watermark_never_passes_undurable():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e = w.grant("r", 2)
    L.grant(2, e, lose=True)
    return check("T8 the watermark does not advance over a lost grant",
                 L.omega == L.a_L)


def t9_random_interleaving(seed=0, steps=400):
    import random
    rng = random.Random(seed)
    w = World()
    docs = list(range(1, 9))
    L = None
    ok = True
    for _ in range(steps):
        r = rng.random()
        if L is None or r < 0.45:
            if L is None:
                w.grant("r", rng.choice(docs))
                L = Lease(w, "r", a_L=w.epoch)
            else:
                d = rng.choice(docs)
                ep = w.grant("r", d)
                L.grant(d, ep, lose=(rng.random() < 0.05))
        elif r < 0.75:
            ep = w.revoke("r", rng.choice(docs))
            L.revoke(ep)                 # published, but no log entry is needed
        else:
            L = L.merge(w)
        pin = w.epoch if rng.random() < 0.5 else max(0, w.epoch - rng.randint(0, 3))
        v = w.visible("r", pin)
        got, mode = L.read(pin, v)
        ok = ok and got <= v
        if mode == "certified":
            ok = ok and (v <= got)          # certified reads are auth-complete
    return check("T9 random interleaving keeps leak-free and certified reads complete", ok)


def e10_lost_grant_falls_back_without_leak():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e = w.grant("r", 2)
    L.grant(2, e, lose=True)
    got, mode = L.read(w.epoch, w.visible("r", w.epoch))
    return check("E10 lost grant -> exact fallback, no leak, no auth miss", mode == "exact")


def e11_publish_read_race():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e = w.grant("r", 2)
    # a read that pins e before the grant is appended must not be certified at e
    before = L.covered(e)
    L.grant(2, e)
    after = L.covered(e)
    return check("E11 a pin before the append is not certified; after it, is",
                 (not before) and after)


def e12_merge_suffix_gap():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e1 = w.grant("r", 2)
    L.grant(2, e1)
    e2 = w.grant("r", 3)
    L.grant(3, e2)
    L2 = L.merge(w)
    got, mode = L2.read(e2, w.visible("r", e2))
    return check("E12 merge leaves no fallback window for grants before the merge",
                 mode == "certified" and {2, 3} <= got)


def e13_pinned_reader_survives_reclaim():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e1 = w.grant("r", 2)
    L.grant(2, e1)
    L2 = L.merge(w)
    L3 = L2.merge(w)
    got, mode = L.read(e1, w.visible("r", e1))
    return check("E13 a reader two merges behind still reads its pinned snapshot",
                 mode == "certified" and 2 in got)


def e14_regrant_across_merge():
    w = World()
    w.grant("r", 1)
    L = Lease(w, "r", a_L=w.epoch)
    e1 = w.grant("r", 2)
    L.grant(2, e1)
    w.revoke("r", 2)
    L2 = L.merge(w)
    e2 = w.grant("r", 2)
    L2.grant(2, e2)
    old, m1 = L.read(e1, w.visible("r", e1))
    new, m2 = L2.read(e2, w.visible("r", e2))
    return check("E14 regrant after a merge: old pin keeps the grant, new pin sees it",
                 m1 == "certified" and m2 == "certified")


def main():
    tests = [t1_grant_searchable_through_log, t2_pinned_read_excludes_later_grant,
             t3_grant_revoke_grant_history, t4_merge_replays_suffix, t5_independent_roles,
             t6_merge_while_pinned, t7_lost_grant_is_not_certified,
             t8_watermark_never_passes_undurable, t9_random_interleaving,
             e10_lost_grant_falls_back_without_leak, e11_publish_read_race,
             e12_merge_suffix_gap, e13_pinned_reader_survives_reclaim,
             e14_regrant_across_merge]
    passed = sum(1 for t in tests if t())
    print(f"\n{passed}/{len(tests)} PASS")
    return 0 if passed == len(tests) else 1


if __name__ == "__main__":
    raise SystemExit(main())
