#!/usr/bin/env python3
"""Promotion survives a restart (review R3 follow-up).

A run that promotes cold roles records each promotion in the shared WAL before
publishing it. A restart re-materializes exactly those roles, then applies the events
in order, so the recovered registry matches the one that crashed. A log that carries
an event for a role with no promotion record is still refused (exit 5) rather than
served from a stale ACL.

Run: python3 experiments/service/test_promotion_restart.py --bin <lease_certified>
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def _project_root(here):
    """Layout-agnostic root: the project tree is either <root>/experiments/<part>
    (paper + package) or <root>/<part> (the public experiments repository)."""
    for depth in (1, 2):
        root = os.path.abspath(os.path.join(here, *([os.pardir] * depth)))
        for sub in (("experiments", "cpp"), ("cpp",)):
            if os.path.isdir(os.path.join(root, *sub)):
                return root, os.path.join(root, *sub)
    return os.path.abspath(os.path.join(here, os.pardir, os.pardir)), None


ROOT, CPP = _project_root(HERE)
FIX = os.path.join(CPP or os.path.join(ROOT, "experiments", "cpp"), "test")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def start_seqd(wal):
    port = free_port()
    p = subprocess.Popen([sys.executable, os.path.join(HERE, "seqd.py"),
                          "--wal", wal, "--port", str(port)],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return p, port
        except OSError:
            time.sleep(0.05)
    p.kill()
    raise RuntimeError("seqd did not start")


def run(binary, port, outdir, changes, env_extra):
    env = dict(os.environ, WAL=f"127.0.0.1:{port}", CHANGE_US="0", CERT="1", **env_extra)
    cmd = [binary,
           os.path.join(FIX, "base.f32"), os.path.join(FIX, "queries.f32"),
           os.path.join(FIX, "roles.bin"), os.path.join(FIX, "qroles.bin"),
           "1000", "8", "5", "3", "1", str(changes), "2", "0", outdir, "15"]
    r = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=120)
    return r.returncode, (r.stderr or "") + (r.stdout or "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    a = ap.parse_args()
    for f in ("base.f32", "queries.f32", "roles.bin", "qroles.bin"):
        if not os.path.exists(os.path.join(FIX, f)):
            print(f"[SKIP] fixture {os.path.join(FIX, f)} is missing")
            return 2
    d = tempfile.mkdtemp()
    fails = 0

    def check(name, cond, detail=""):
        nonlocal fails
        print(f"[{'PASS' if cond else 'FAIL'}] {name}" + (f"  {detail}" if detail and not cond else ""))
        if not cond:
            fails += 1

    # 1. promotion writes events for roles the registry did not have at startup
    wal = os.path.join(d, "promo.wal")
    srv, port = start_seqd(wal)
    try:
        rc, out = run(a.bin, port, os.path.join(d, "a"), 30,
                      {"ANY_ROLE": "1", "PROMOTE_ON_WRITE": "1"})
        check("promotion run completes", rc == 0, f"exit {rc}: {out[-200:]}")
        check("promotion run actually promoted", "promotions=" in out and "promotions=0" not in out)
        rc, out = run(a.bin, port, os.path.join(d, "b"), 0, {"REPLAY": "1"})
        check("restart replays a promotion log", rc == 0,
              f"exit {rc}: {out.strip().splitlines()[-1] if out.strip() else ''}")
        check("the restart re-materializes the promoted roles",
              "promotions=" in out and "promotions=0" not in out.split()[-1])
        check("the replay reports the promotions it rebuilt", "promotions" in out)
    finally:
        srv.kill()
        srv.wait()

    # 2. without promotion the same shape of log replays cleanly
    wal2 = os.path.join(d, "nopromo.wal")
    srv, port = start_seqd(wal2)
    try:
        rc, out = run(a.bin, port, os.path.join(d, "c"), 30,
                      {"ANY_ROLE": "1", "PROMOTE_ON_WRITE": "0"})
        check("refusal-only run completes", rc == 0, f"exit {rc}")
        rc, out = run(a.bin, port, os.path.join(d, "d"), 0, {"REPLAY": "1"})
        check("refusal-only log replays", rc == 0, f"exit {rc}: {out[-200:]}")
    finally:
        srv.kill()
        srv.wait()

    shutil.rmtree(d, ignore_errors=True)
    print(f"\n{'PASS' if fails == 0 else 'FAIL'}: {6 - fails}/6")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
