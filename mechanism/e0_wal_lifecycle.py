#!/usr/bin/env python3
"""E0-WAL -- lifecycle tests for replay, continued updates and commit failure.

The model suite (``e0_certified_tests.py``) pins the interval contract on an
abstract lease; this suite drives the C++ kernel through the lifecycle that the
third review found untested:

  A1  grant -> crash -> replay -> revoke -> query     (watermark vs rebuilt log)
  A2  grant -> revoke -> grant -> crash -> replay     (re-grant history)
  A3  crash -> replay -> continued updates, no merge  (watermark before rebuild)
  A4  crash -> replay -> continued updates with merges (a', omega] suffix)
  B1  stalled sequencer -> attempted commit           (fail closed, no hang)
  B2  unreachable sequencer at startup                (refuse to run)
  B3  replay RPC fails                                (refuse to serve)
  B4  log entry for an unpromoted role                (refuse a stale ACL)

Each run is checked against the kernel's own exact oracle (``missing``,
``auth``, ``leaks``) *and* against two recovery invariants exposed under
``SELFCHECK=1``:

  state_mismatch  per-document visible(d, pin) != cur(d) at the published epoch
  visible_total   |V(r, pin)| must equal cur_total (no phantom, no lost grant)

The second check matters because the exact oracle shares the visibility code:
a replay that fails to rebuild ``multi``/``hist`` makes oracle and answer wrong
together, so ``missing`` stays zero while the served set is wrong.

Run on the server (the fixtures are generated; no SIFT copy is needed):

    python3 experiments/mechanism/e0_wal_lifecycle.py \
        --bin /root/work/icde/lease_certified --seqd experiments/service/seqd.py

Locally: build the kernel first, then pass ``--bin``.
"""
import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

FIELDS = re.compile(r"(\w+)=([-\d.eE]+)")
CHECKS = 0


# ---------------------------------------------------------------- fixtures ---
def write_fixtures(root: Path):
    """One role (id 0) over docs 0..7; docs 8..31 are grant candidates."""
    import array

    n, dim, nq = 32, 8, 4
    base = array.array("f")
    for i in range(n):
        v = [0.0] * dim
        v[i % dim] = float(i) + 1.0
        v[(i + 3) % dim] = float(i) * 0.5
        base.extend(v)
    (root / "base.f32").write_bytes(base.tobytes())

    queries = array.array("f")
    for i in (12, 0, 5, 20):
        queries.extend(base[i * dim:(i + 1) * dim])
    (root / "queries.f32").write_bytes(queries.tobytes())

    roles = array.array("i", [1, 0, 8] + list(range(8)))
    (root / "roles.bin").write_bytes(roles.tobytes())
    qroles = array.array("i", [nq, 0, 0, 0, 0])
    (root / "qroles.bin").write_bytes(qroles.tobytes())
    return dict(n=n, dim=dim, nq=nq, k=3, K=1)


def write_wal(path: Path, events):
    """events: [(role, 'grant'|'revoke', doc), ...] in commit order."""
    with open(path, "w", encoding="utf-8") as f:
        for version, (role, kind, doc) in enumerate(events, 1):
            f.write(json.dumps({"version": version, "role": role, "kind": kind,
                                "doc": doc}) + "\n")


# ---------------------------------------------------------------- servers ----
def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_port(port, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.02)
    return False


class SeqD:
    def __init__(self, seqd_path, wal_path, port, stall_after=0):
        self.port = port
        cmd = [sys.executable, str(seqd_path), "--port", str(port), "--wal", str(wal_path)]
        if stall_after:
            cmd += ["--stall-after", str(stall_after)]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True)

    def wait(self):
        if not wait_port(self.port):
            raise RuntimeError("seqd did not start")
        return self

    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()


class Blackhole:
    """Accepts connections, never answers: a stalled (not dead) sequencer."""

    def __init__(self, port):
        self.port = port
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", port))
        self.sock.listen(8)
        self.stop = False
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        while not self.stop:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            # hold the connection open without a reply; the client's socket
            # timeout is what has to turn this into a refused commit
            threading.Thread(target=self._hold, args=(conn,), daemon=True).start()

    @staticmethod
    def _hold(conn):
        try:
            while conn.recv(4096):
                pass
        except OSError:
            pass

    def close(self):
        self.stop = True
        try:
            self.sock.close()
        except OSError:
            pass


# ------------------------------------------------------------------ kernel ---
def run_kernel(binary, root, data, outdir, changes, port=None, replay=False,
               extra_env=None, timeout=20):
    outdir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env.update({"CERT": "1", "SELFCHECK": "1", "CHANGE_US": "50"})
    if port is not None:
        env["WAL"] = f"127.0.0.1:{port}"
        env["WAL_TIMEOUT_MS"] = "300"
    if replay:
        env["REPLAY"] = "1"
    env.update(extra_env or {})
    cmd = [str(binary), str(root / "base.f32"), str(root / "queries.f32"),
           str(root / "roles.bin"), str(root / "qroles.bin"),
           str(data["n"]), str(data["dim"]), str(data["nq"]), str(data["k"]),
           str(data["K"]), str(changes), "1", "1", str(outdir), "15"]
    try:
        proc = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=timeout)
        timed_out = False
    except subprocess.TimeoutExpired as e:
        return {"exit": None, "timeout": True,
                "stdout": (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or ""),
                "stderr": (e.stderr or b"").decode() if isinstance(e.stderr, bytes) else (e.stderr or ""),
                "summary": {}}
    summary = {}
    sm = outdir / "summary.txt"
    if sm.exists():
        summary = {k: v for k, v in FIELDS.findall(sm.read_text())}
    return {"exit": proc.returncode, "timeout": timed_out, "stdout": proc.stdout,
            "stderr": proc.stderr, "summary": summary}


def f(summary, key, default=None):
    try:
        return float(summary[key])
    except (KeyError, TypeError, ValueError):
        return default


def check(name, cond, detail=""):
    global CHECKS
    CHECKS += 1
    print(f"[{'PASS' if cond else 'FAIL'}] {name}" + ("" if cond else f"  {detail}"))
    return bool(cond)


# ------------------------------------------------------------------- cases ---
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="path to the compiled lease_certified")
    ap.add_argument("--seqd", default=str(Path(__file__).resolve().parents[1] / "service" / "seqd.py"))
    ap.add_argument("--workdir", default=None)
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()

    binary = Path(a.bin).resolve()
    seqd_path = Path(a.seqd).resolve()
    root = Path(a.workdir) if a.workdir else Path(tempfile.mkdtemp(prefix="e0-wal-"))
    root.mkdir(parents=True, exist_ok=True)
    data = write_fixtures(root)
    ok = True
    started = []

    def seqd(wal_name, events):
        wal = root / wal_name
        write_wal(wal, events)
        port = free_port()
        s = SeqD(seqd_path, wal, port).wait()
        started.append(s)
        return s

    try:
        # A1: grant -> crash -> replay -> revoke -> query.
        s = seqd("wal_a1.jsonl", [(0, "grant", 12)])
        r = run_kernel(binary, root, data, root / "a1", changes=1, port=s.port, replay=True)
        sm = r["summary"]
        ok &= check("A1 replay+revoke: process exits 0", r["exit"] == 0, str(r["exit"]))
        ok &= check("A1 replay+revoke: no authorization miss", f(sm, "auth") == 0, str(sm))
        ok &= check("A1 replay+revoke: no missing result", f(sm, "missing") == 0, str(sm))
        ok &= check("A1 replay+revoke: no certified fallback", f(sm, "cert_fallbacks") == 0, str(sm))
        ok &= check("A1 replay+revoke: visible == cur",
                    f(sm, "state_mismatch") == 0 and f(sm, "visible_total") == f(sm, "cur_total"),
                    str(sm))
        s.close(); started.remove(s)

        # A2: grant -> revoke -> grant -> crash -> replay (no new change).
        s = seqd("wal_a2.jsonl", [(0, "grant", 12), (0, "revoke", 12), (0, "grant", 12)])
        r = run_kernel(binary, root, data, root / "a2", changes=0, port=s.port, replay=True)
        sm = r["summary"]
        ok &= check("A2 re-grant: replay exits 0", r["exit"] == 0, str(r["exit"]))
        ok &= check("A2 re-grant: doc 12 is visible after replay", f(sm, "visible_total") == 9, str(sm))
        ok &= check("A2 re-grant: visible == cur", f(sm, "state_mismatch") == 0, str(sm))
        ok &= check("A2 re-grant: reads certified, no fallback", f(sm, "cert_fallbacks") == 0, str(sm))
        s.close(); started.remove(s)

        # A3: replay followed by updates *without* a merge. This is the case the
        # old replay path failed: the first change advanced omega over a graph and
        # ring that did not hold the replayed grant.
        s = seqd("wal_a3.jsonl", [(0, "grant", 12)])
        r = run_kernel(binary, root, data, root / "a3", changes=2, port=s.port, replay=True)
        sm = r["summary"]
        ok &= check("A3 replay+updates (no merge): no auth miss", f(sm, "auth") == 0, str(sm))
        ok &= check("A3 replay+updates (no merge): no missing result", f(sm, "missing") == 0, str(sm))
        ok &= check("A3 replay+updates (no merge): visible == cur",
                    f(sm, "state_mismatch") == 0 and f(sm, "visible_total") == f(sm, "cur_total"),
                    str(sm))
        s.close(); started.remove(s)

        # A4: merge -> replay -> three more updates, each followed by a merge.
        s = seqd("wal_a4.jsonl", [(0, "grant", 12)])
        r = run_kernel(binary, root, data, root / "a4", changes=3, port=s.port,
                       replay=True, extra_env={"MERGE_EVERY": "1"})
        sm = r["summary"]
        ok &= check("A4 merge+replay: no auth miss", f(sm, "auth") == 0, str(sm))
        ok &= check("A4 merge+replay: no missing result", f(sm, "missing") == 0, str(sm))
        ok &= check("A4 merge+replay: visible == cur", f(sm, "state_mismatch") == 0, str(sm))
        ok &= check("A4 merge+replay: recovery and later merges ran", f(sm, "merges", 0) >= 2, str(sm))
        s.close(); started.remove(s)

        # B1: stalled sequencer. A commit must time out, be refused, and leave no state.
        port = free_port()
        bh = Blackhole(port)
        r = run_kernel(binary, root, data, root / "b1", changes=1, port=port, replay=False, timeout=15)
        sm = r["summary"]
        ok &= check("B1 stalled WAL: process does not hang", not r["timeout"] and r["exit"] == 0,
                    str((r["exit"], r["timeout"])))
        ok &= check("B1 stalled WAL: commit refused", f(sm, "wal_refused", 0) >= 1, str(sm))
        ok &= check("B1 stalled WAL: refused change leaves no state",
                    f(sm, "hist_rows") == 0 and f(sm, "log_len") == 0, str(sm))
        ok &= check("B1 stalled WAL: reads stay exact and correct",
                    f(sm, "missing") == 0 and f(sm, "leaks") == 0, str(sm))
        bh.close()

        # B2: unreachable sequencer at startup.
        dead_port = free_port()
        r = run_kernel(binary, root, data, root / "b2", changes=1, port=dead_port, replay=False)
        ok &= check("B2 unreachable WAL: exits 3 before serving", r["exit"] == 3, str(r["exit"]))

        # B3: replay RPC fails -> refuse to serve.
        port = free_port()
        bh = Blackhole(port)
        r = run_kernel(binary, root, data, root / "b3", changes=0, port=port, replay=True)
        ok &= check("B3 failed replay: exits 4", r["exit"] == 4, str(r["exit"]))
        bh.close()

        # B4: log entry for an unpromoted role -> refuse a stale ACL.
        s = seqd("wal_b4.jsonl", [(1, "grant", 12)])
        r = run_kernel(binary, root, data, root / "b4", changes=0, port=s.port, replay=True)
        ok &= check("B4 unpromoted role in log: exits 5", r["exit"] == 5, str(r["exit"]))
        s.close(); started.remove(s)

        # B5: the exact fallback must not wait for a writer stuck in a durable
        # commit (review P1-4).  Op 1 stores a lost grant (omega freezes at 0), so
        # every read at pin 1 falls back; op 2 blocks in the sequencer.  Readers
        # start 400 ms into the 1.5 s commit timeout: with the commit inside
        # exact_mu the fallback waits out the timeout (~1.1 s p50), with the
        # commit outside it the read is served in milliseconds.
        port = free_port()
        (root / "wal_b5.jsonl").unlink(missing_ok=True)   # a rerun must start empty
        s2 = SeqD(seqd_path, root / "wal_b5.jsonl", port, stall_after=1).wait()
        started.append(s2)
        b5 = root / "b5"
        b5.mkdir(exist_ok=True)
        (b5 / "script.txt").write_text("grant 0 9 lost\ngrant 0 10\n")
        r = run_kernel(binary, root, data, b5, changes=0, port=s2.port,
                       extra_env={"SCRIPT": str(b5 / "script.txt"), "WARMUP": "1",
                                  "READ_DELAY_MS": "400", "CHANGE_US": "0",
                                  "WAL_TIMEOUT_MS": "1500"}, timeout=30)
        sm = r["summary"]
        ok &= check("B5 stalled commit: process exits 0", r["exit"] == 0, str(r["exit"]))
        ok &= check("B5 stalled commit: the second commit is refused",
                    f(sm, "wal_refused", 0) >= 1, str(sm))
        ok &= check("B5 stalled commit: fallback returns the right answer",
                    f(sm, "missing") == 0 and f(sm, "leaks") == 0, str(sm))
        ok &= check("B5 stalled commit: every read falls back",
                    f(sm, "cert_fallbacks") == data["nq"], str(sm))
        ok &= check("B5 stalled commit: fallback is not blocked by the writer",
                    f(sm, "p50", 1e9) < 500, f"p50={f(sm, 'p50')} ms")
        s2.close(); started.remove(s2)
    finally:
        for s in started:
            s.close()
        if not a.keep and not a.workdir:
            shutil.rmtree(root, ignore_errors=True)

    print(f"\n{CHECKS} checks, {'ALL PASS' if ok else 'FAILURES PRESENT'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
