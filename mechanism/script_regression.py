#!/usr/bin/env python3
"""Deterministic kernel regression for the coverage certificate.

One scenario, four arms, no randomness:

    1  grant  0 7          the document a query is looking for
    2  revoke 0 7
    3  merge  0            the graph is rebuilt without 7; a = 2
    4  grant  0 7 lost     the state changes, the log entry does not

At pin 3 the document is authorized (V(0,3) = {0,7}) but it is in neither the
graph built at a = 2 nor the log, so the lease cannot supply it.

Arms:
  cert          certificate on   -> pin 3 > omega 2, exact fallback, answer is 7
  nocert        certificate off  -> graph+log path answers 0; the miss must be
                                    attributed to authorization (auth = 1).
                                    Before the P0-1 fix it was attributed to ANN
                                    (auth = 0) because "first grant epoch <= a"
                                    stood in for graph membership.
  control       no lost grant    -> the regrant is in the log, answer is 7
  nocert_control no lost grant   -> same, without the certificate

Every arm is also scored by ``oracle_check.py``, which folds the committed
event stream and the returned documents and must reproduce the kernel's own
leaks/missing/auth counts.

Run: python3 experiments/mechanism/script_regression.py --bin ./lease_certified
"""
import argparse
import array
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ORACLE = HERE / "oracle_check.py"

N, DIM, NQ, K = 16, 4, 4, 1

FAULT_SCRIPT = """\
# grant the target, revoke it, rebuild the graph without it, re-grant with a lost log entry
grant 0 7
revoke 0 7
merge 0
grant 0 7 lost
"""
CONTROL_SCRIPT = """\
grant 0 7
revoke 0 7
merge 0
grant 0 7
"""


def write_fixtures(root: Path):
    base = array.array("f")
    for d in range(N):
        v = [10.0 * (d + 1), 1.0, 0.0, 0.0]
        if d == 7:
            v = [0.0, 0.0, 0.0, 0.0]          # q0 = base[7]: the target is the top-1
        base.extend(v)
    (root / "base.f32").write_bytes(base.tobytes())

    queries = array.array("f")
    queries.extend([0.0] * DIM)                # q0
    queries.extend(base[0:DIM])                # q1-q3 point at doc 0
    for _ in range(2):
        queries.extend(base[0:DIM])
    (root / "queries.f32").write_bytes(queries.tobytes())

    roles = array.array("i", [1, 0, 1, 0])     # one role (0) holding doc 0
    (root / "roles.bin").write_bytes(roles.tobytes())
    qroles = array.array("i", [NQ] + [0] * NQ)
    (root / "qroles.bin").write_bytes(qroles.tobytes())
    return dict(n=N, dim=DIM, nq=NQ, k=K, K=1)


def parse_summary(out: str):
    line = [l for l in out.strip().splitlines() if l.startswith("certified build_s")][-1]
    return {k: float(v) for k, v in re.findall(r"(\w+)=([-\d.eE]+)", line)}


def run_arm(root: Path, binpath: str, name: str, script: str, cert: int):
    d = root / name
    d.mkdir(exist_ok=True)
    script_path = d / "script.txt"
    script_path.write_text(script)
    changes = d / "changes.txt"
    merges = d / "merges.txt"
    reads = d / "reads.txt"
    for p in (changes, merges, reads):
        if p.exists():
            p.unlink()
    env = dict(os.environ,
               SCRIPT=str(script_path), CERT=str(cert), CHANGE_US="0",
               CHANGES_OUT=str(changes), MERGES_OUT=str(merges),
               QUERIES_OUT=str(reads))
    cmd = [binpath, str(root / "base.f32"), str(root / "queries.f32"),
           str(root / "roles.bin"), str(root / "qroles.bin"),
           str(N), str(DIM), str(NQ), str(K), str(1), "0", "1", "0",
           str(d), "15"]
    r = subprocess.run(cmd, env=env, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"{name}: kernel exit {r.returncode}\n{r.stderr}")
    m = parse_summary(r.stdout)
    got = {}
    for line in reads.read_text().splitlines():
        p = [int(x) for x in line.split()]
        got[p[0]] = p[4:]
    return m, got


def oracle(root: Path, binpath: str, name: str, script: str):
    d = root / name
    cmd = [sys.executable, str(ORACLE),
           "--base", str(root / "base.f32"), "--queries", str(root / "queries.f32"),
           "--roles", str(root / "roles.bin"), "--n", str(N), "--dim", str(DIM),
           "--k", str(K), "--changes", str(d / "changes.txt"),
           "--merges", str(d / "merges.txt"), "--reads", str(d / "reads.txt"),
           "--script", str(d / "script.txt")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"{name}: oracle failed\n{r.stdout}\n{r.stderr}")
    return r.stdout


def write_churn_fixture(root: Path):
    """The e0 WAL fixture: 8 base documents, 24 grant candidates, 4 queries."""
    n, dim, nq = 32, 8, 4
    base = array.array("f")
    for i in range(n):
        v = [0.0] * dim
        v[i % dim] = float(i) + 1.0
        v[(i + 3) % dim] = float(i) * 0.5
        base.extend(v)
    (root / "base.f32").write_bytes(base.tobytes())
    q = array.array("f")
    for i in (12, 0, 5, 20):
        q.extend(base[i * dim:(i + 1) * dim])
    (root / "queries.f32").write_bytes(q.tobytes())
    (root / "roles.bin").write_bytes(array.array("i", [1, 0, 8] + list(range(8))).tobytes())
    (root / "qroles.bin").write_bytes(array.array("i", [nq, 0, 0, 0, 0]).tobytes())


CHECKS = 0
FAILED = 0


def check(name, cond, detail=""):
    global CHECKS, FAILED
    CHECKS += 1
    if not cond:
        FAILED += 1
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" + (f"  ({detail})" if detail and not cond else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        write_fixtures(root)

        cert, cert_got = run_arm(root, a.bin, "cert", FAULT_SCRIPT, 1)
        nocert, nocert_got = run_arm(root, a.bin, "nocert", FAULT_SCRIPT, 0)
        ctrl, ctrl_got = run_arm(root, a.bin, "control", CONTROL_SCRIPT, 1)
        nctrl, nctrl_got = run_arm(root, a.bin, "nocert_control", CONTROL_SCRIPT, 0)

        # The certificate rejects the read and the exact scan returns the target.
        check("cert: the lost grant is caught by an exact fallback",
              cert["cert_fallbacks"] == NQ, f"fallbacks={cert['cert_fallbacks']}")
        check("cert: no missing result", cert["missing"] == 0, f"missing={cert['missing']}")
        check("cert: fallback returns the true top-1", cert_got[0] == [7], f"got={cert_got[0]}")

        # Without the check the same stream answers from the incomplete log.
        check("nocert: no fallback (the hole is silent)", nocert["cert_fallbacks"] == 0,
              f"fallbacks={nocert['cert_fallbacks']}")
        check("nocert: the true top-1 is missing", nocert_got[0] == [0], f"got={nocert_got[0]}")
        check("nocert: missing is 1", nocert["missing"] == 1, f"missing={nocert['missing']}")
        # P0-1 regression: the miss is an authorization miss, not an ANN miss.
        check("nocert: the miss is attributed to authorization", nocert["auth"] == 1,
              f"auth={nocert['auth']}")

        # Control: with the log entry present both arms return the target.
        check("control: certified read returns the regranted document",
              ctrl["missing"] == 0 and ctrl_got[0] == [7],
              f"missing={ctrl['missing']} got={ctrl_got[0]}")
        check("nocert_control: the log entry alone is enough",
              nctrl["missing"] == 0 and nctrl_got[0] == [7],
              f"missing={nctrl['missing']} got={nctrl_got[0]}")

        # Independent oracle: recompute V(r,pin) from the committed event stream.
        for name, summary, script in (("cert", cert, FAULT_SCRIPT),
                                      ("nocert", nocert, FAULT_SCRIPT),
                                      ("control", ctrl, CONTROL_SCRIPT),
                                      ("nocert_control", nctrl, CONTROL_SCRIPT)):
            out = oracle(root, a.bin, name, script)
            m = re.search(r"leaks=(\d+) missing=(\d+)(?: auth=(\d+))?", out)
            o_leaks = int(m.group(1))
            o_missing = int(m.group(2))
            o_auth = int(m.group(3)) if m.group(3) is not None else None
            check(f"{name}: oracle sees zero leaks", o_leaks == 0, f"leaks={o_leaks}")
            check(f"{name}: oracle missing matches the kernel",
                  o_missing == summary["missing"],
                  f"oracle={o_missing} kernel={summary['missing']}")
            check(f"{name}: oracle auth matches the kernel",
                  o_auth == summary["auth"],
                  f"oracle={o_auth} kernel={summary['auth']}")

        # The same attribution on a randomized churn stream: with readers released
        # after the last change every pin is identical, so seed 5 is a fixed
        # witness (it was missing=1 auth=0 before the P0-1 fix).
        rnd = root / "random"
        rnd.mkdir(exist_ok=True)
        write_churn_fixture(rnd)
        changes = rnd / "changes.txt"
        merges = rnd / "merges.txt"
        reads = rnd / "reads.txt"
        for p in (changes, merges, reads):
            if p.exists():
                p.unlink()
        env = dict(os.environ, CERT="0", CHANGE_US="0", DROP_GRANT="3",
                   MERGE_EVERY="5", WARMUP="300", CHANGES_OUT=str(changes),
                   MERGES_OUT=str(merges), QUERIES_OUT=str(reads))
        cmd = [a.bin, str(rnd / "base.f32"), str(rnd / "queries.f32"),
               str(rnd / "roles.bin"), str(rnd / "qroles.bin"),
               "32", "8", "4", "3", "1", "300", "1", "5", str(rnd / "out"), "15"]
        r = subprocess.run(cmd, env=env, capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(f"random: kernel exit {r.returncode}\n{r.stderr}")
        m = parse_summary(r.stdout)
        check("random seed 5: every miss is an authorization miss",
              m["missing"] == m["auth"],
              f"missing={m['missing']} auth={m['auth']}")
        ocmd = [sys.executable, str(ORACLE),
                "--base", str(rnd / "base.f32"), "--queries", str(rnd / "queries.f32"),
                "--roles", str(rnd / "roles.bin"), "--n", "32", "--dim", "8",
                "--k", "3", "--changes", str(changes), "--merges", str(merges),
                "--reads", str(reads)]
        o = subprocess.run(ocmd, capture_output=True, text=True)
        if o.returncode != 0:
            raise SystemExit(f"random: oracle failed\n{o.stdout}\n{o.stderr}")
        om = re.search(r"leaks=(\d+) missing=(\d+)", o.stdout)
        check("random seed 5: oracle agrees on leaks and missing",
              int(om.group(1)) == 0 and int(om.group(2)) == int(m["missing"]),
              f"oracle={om.group(0)} kernel missing={m['missing']}")

    print(f"\n{CHECKS - FAILED}/{CHECKS} PASS")
    return 0 if FAILED == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
