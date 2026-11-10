#!/usr/bin/env python3
"""Sequencer daemon: the single version authority, with a write-ahead log.

The paper's contract assumes one writer orders policy changes. This process is
that ordering point, factored out of the C++ harness so that it can be
restarted, replicated by replay, and queried over a socket. It is a control-plane
component: it sits off the read path, and readers keep serving at their pinned
version while it is down.

Protocol (newline-delimited JSON, one request per line):
    {"op":"commit","role":R,"kind":"grant"|"revoke","doc":D}
        -> {"ok":true,"version":V}
    {"op":"version"}
        -> {"version":V}
    {"op":"replay","from":N}
        -> {"events":[[version, role, kind, doc], ...]}

Durability: every commit is appended to the WAL and fsync'd before the version
is acknowledged. On restart the log is replayed to rebuild in-memory state, so a
killed sequencer resumes at the same version with the same history.

Run: python3 seqd.py --port 19999 --wal /tmp/seq.wal [--rtt-ms 2]
"""
import argparse
import json
import os
import socket
import socketserver
import sys
import threading
import time

LOCK = threading.Lock()


class State:
    def __init__(self, wal_path, rtt_ms=0.0, stall_after=0):
        self.wal_path = wal_path
        self.rtt_ms = rtt_ms
        self.stall_after = stall_after   # test knob: stop acknowledging after N commits
        self.commits = 0
        self.version = 0
        self.events = []          # (version, role, kind, doc)
        self.wal = open(wal_path, "a", buffering=1, encoding="utf-8")
        self.failed = False       # a failed append/fsync freezes the version
        self.fail_reason = ""
        self._replay()
        self.commits = len(self.events)   # --stall-after counts recovered commits too

    def _replay(self):
        """Rebuild state from the log, refusing anything that is not a clean prefix.

        A crash can leave a torn final record. It must be *truncated* before the next
        append: appending after a partial line would merge the two records into one
        unparseable line and silently lose the new commit on the next restart (the
        eighth review's P0-1). A corrupt record that is not the tail, a gap in the
        stored version, or a version that disagrees with its position means the log
        cannot be trusted, and the sequencer refuses to start rather than renumber.
        """
        if not os.path.exists(self.wal_path):
            return
        with open(self.wal_path, "rb") as f:
            data = f.read()
        good = 0                    # bytes covered by the accepted prefix
        expected = 0
        torn_tail = False
        for raw in data.splitlines(keepends=True):
            if not raw.endswith(b"\n"):
                torn_tail = True    # final record without its newline: incomplete
                break
            try:
                ev = json.loads(raw.decode("utf-8"))
            except (json.JSONDecodeError, UnicodeDecodeError):
                torn_tail = (good + len(raw) == len(data))
                if not torn_tail:
                    raise RuntimeError(
                        f"WAL {self.wal_path}: corrupt record at byte {good}, "
                        f"{len(data) - good - len(raw)} bytes after it; refusing to start")
                break
            expected += 1
            if ev.get("version") != expected:
                raise RuntimeError(
                    f"WAL {self.wal_path}: record {expected} carries version "
                    f"{ev.get('version')}; refusing to renumber")
            self.events.append((expected, ev["role"], ev["kind"], ev["doc"]))
            good += len(raw)
        self.version = expected
        if good != len(data):
            # drop the torn tail so the next append starts on a record boundary
            with open(self.wal_path, "r+b") as f:
                f.truncate(good)
                f.flush()
                os.fsync(f.fileno())
            sys.stderr.write(
                f"WAL {self.wal_path}: truncated {len(data) - good} byte(s) of an "
                f"incomplete tail record; recovered version {self.version}\n")

    def commit(self, role, kind, doc):
        with LOCK:
            if self.stall_after and self.commits >= self.stall_after:
                # Fault injection for the stalled-commit regression: hold the
                # request forever instead of acknowledging it.
                time.sleep(3600)
            if self.failed:
                raise RuntimeError(
                    f"sequencer failed ({self.fail_reason}); restart from the WAL "
                    f"before accepting commits")
            # The version is the position in the durable log. It must not move
            # until the record is on disk: incrementing first would leave a gap
            # that makes the next restart reject the log.
            v = self.version + 1
            rec = {"version": v, "role": role, "kind": kind, "doc": doc}
            try:
                self.wal.write(json.dumps(rec) + "\n")
                self.wal.flush()
                os.fsync(self.wal.fileno())
            except BaseException as e:   # OSError on write/fsync, disk full, EIO
                # Do not renumber and do not accept another commit. The record
                # may be absent (write failed) or present-but-unconfirmed (fsync
                # failed); only replay can decide, so the sequencer stays frozen.
                self.failed = True
                self.fail_reason = f"{type(e).__name__}: {e}"
                raise
            self.version = v
            self.events.append((v, role, kind, doc))
            self.commits += 1
            return v


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        for line in self.rfile:
            try:
                req = json.loads(line)
            except json.JSONDecodeError:
                self.wfile.write(b'{"ok":false,"error":"bad json"}\n')
                continue
            if STATE.rtt_ms:
                time.sleep(STATE.rtt_ms / 1000.0)     # emulated WAN round trip
            op = req.get("op")
            if op == "commit":
                try:
                    v = STATE.commit(int(req["role"]), req["kind"], int(req["doc"]))
                    resp = {"ok": True, "version": v}
                except Exception as e:
                    resp = {"ok": False, "error": "commit refused",
                            "detail": str(e)}
            elif op == "version":
                resp = {"version": STATE.version, "failed": STATE.failed}
            elif op == "replay":
                if STATE.failed:
                    resp = {"ok": False, "error": "sequencer failed"}
                    self.wfile.write((json.dumps(resp) + "\n").encode())
                    self.wfile.flush()
                    continue
                frm = int(req.get("from", 0))
                # kind is emitted numerically (1 grant, 2 revoke) so a C++ client
                # can parse the array without a JSON string decoder.
                # kind: 1 grant, 2 revoke, 3 promote (a durable "this role is
                # materialized" record, so a restart rebuilds the same registry)
                kk = {"grant": 1, "revoke": 2, "promote": 3}
                resp = {"events": [[v, r, kk.get(k, 2), d]
                                   for (v, r, k, d) in STATE.events if v > frm]}
            elif op == "shutdown":
                resp = {"ok": True}
                self.wfile.write((json.dumps(resp) + "\n").encode())
                self.wfile.flush()
                os._exit(0)
            else:
                resp = {"ok": False, "error": "unknown op"}
            self.wfile.write((json.dumps(resp) + "\n").encode())
            self.wfile.flush()


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    global STATE
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=19999)
    ap.add_argument("--wal", default="/tmp/seq.wal")
    ap.add_argument("--rtt-ms", type=float, default=0.0)
    ap.add_argument("--stall-after", type=int, default=0,
                    help="test only: stop acknowledging commits after N")
    a = ap.parse_args()
    STATE = State(a.wal, a.rtt_ms, a.stall_after)
    print(f"seqd: recovered version={STATE.version} events={len(STATE.events)} "
          f"wal={a.wal} rtt={a.rtt_ms}ms", flush=True)
    with Server(("0.0.0.0", a.port), Handler) as srv:
        srv.serve_forever()


if __name__ == "__main__":
    main()
