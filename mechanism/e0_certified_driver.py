#!/usr/bin/env python3
"""E0-certified driver: replay the contract scenarios against lease_certified.

The model tests in ``e0_certified_tests.py`` pin the semantics; this driver runs
the same predicates against the C++ kernel on a small ACL and asserts what the
process summary must show:

  * every run exits 0 and leaks 0 documents;
  * with the certificate on, an injected grant loss (DROP_GRANT) must be caught:
    the read falls back (cert_fallbacks > 0) instead of answering silently;
  * with the certificate off, the same fault must not raise a fallback, which is
    what "silently incomplete" means and why the certificate is the arm that
    matters;
  * version equality on a churning stream falls back on essentially every read.

Run on the server (needs the compiled binary and a base/query pair):

    B=/root/autodl-tmp/icde_cm
    python3 e0_certified_driver.py --bin $B/lease_certified \
        --base $B/base.f32 --queries $B/queries.f32 \
        --roles $B/acl/erbac200k/roles.bin --qroles $B/acl/erbac200k/qroles_head10.bin \
        --out /root/work/icde/results/e0_certified_20261108
"""
import argparse
import os
import re
import subprocess
import sys

ARMS = [
    # name, env overrides, expectation
    ("full", {"CERT": "1", "DROP_GRANT": "0"},
     {"cert_fallbacks": ("==", 0)}),
    ("versioneq", {"CERT": "1", "PRED": "1", "DROP_GRANT": "0"},
     {"cert_fallbacks": (">=", 90)}),
    ("nolog", {"CERT": "1", "NOLOG": "1", "DROP_GRANT": "0"},
     {"cert_fallbacks": (">=", 90)}),
    ("nocert", {"CERT": "0", "DROP_GRANT": "0"},
     {"cert_fallbacks": ("==", 0)}),
    ("full_hole", {"CERT": "1", "DROP_GRANT": "5"},
     {"cert_fallbacks": (">=", 1)}),
    ("nocert_hole", {"CERT": "0", "DROP_GRANT": "5"},
     {"cert_fallbacks": ("==", 0)}),
    # Review W1: with the regrant sampler on and a long change stream, no read may
    # disagree with the history it summarises. A publication order that rewrites the
    # fast-path bytes before the history/flags shows up here (it produced 962
    # violations on the pre-fix ordering with the same parameters).
    ("regrant_window", {"CERT": "1", "DROP_GRANT": "0", "REGRANT_CHECK": "1",
                        "CHANGE_US": "0"},
     {"regrant_violations": ("==", 0), "live_mismatch": ("==", 0)}, 2000),
]

FIELDS = ("reads", "p50", "p95", "leaks", "missing", "auth", "cert_fallbacks")


def parse(line):
    out = {}
    for k, v in re.findall(r"(\w+)=([-\d.eE]+)", line):
        out[k] = float(v)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--base", required=True)
    ap.add_argument("--queries", required=True)
    ap.add_argument("--roles", required=True)
    ap.add_argument("--qroles", required=True)
    ap.add_argument("--n", default="200000")
    ap.add_argument("--changes", default="60")
    ap.add_argument("--nq", default="100")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    failures = 0
    print(f"{'arm':12s} {'exit':>4s} {'reads':>6s} {'p50':>7s} {'leaks':>6s} "
          f"{'missing':>7s} {'cert_fb':>7s}  verdict")
    for arm in ARMS:
        name, env, expect = arm[0], arm[1], arm[2]
        changes = arm[3] if len(arm) > 3 else int(a.changes)
        d = os.path.join(a.out, name)
        os.makedirs(d, exist_ok=True)
        e = dict(os.environ, **{"CHANGE_US": "500", **env})
        cmd = [a.bin, a.base, a.queries, a.roles, a.qroles,
               a.n, "128", a.nq, "10", "10", str(changes), "4", "0", d, "15"]
        r = subprocess.run(cmd, env=e, capture_output=True, text=True)
        line = (r.stdout or "").strip().splitlines()
        line = line[-1] if line else ""
        m = parse(line)
        ok = r.returncode == 0 and m.get("leaks", 1) == 0
        for field, (op, bound) in expect.items():
            v = m.get(field)
            if v is None:
                ok = False
            elif op == "==" and v != bound:
                ok = False
            elif op == ">=" and v < bound:
                ok = False
        failures += 0 if ok else 1
        print(f"{name:12s} {r.returncode:4d} {m.get('reads', 0):6.0f} "
              f"{m.get('p50', 0):7.3f} {m.get('leaks', 0):6.0f} "
              f"{m.get('missing', 0):7.0f} {m.get('cert_fallbacks', 0):7.0f}  "
              f"{'PASS' if ok else 'FAIL'}")
        with open(os.path.join(d, "cmd.txt"), "w") as f:
            f.write(" ".join(f"{k}={v}" for k, v in sorted(env.items())) + "\n")
            f.write(" ".join(cmd) + "\n")
    print(f"\n{len(ARMS) - failures}/{len(ARMS)} PASS")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
