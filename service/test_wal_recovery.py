#!/usr/bin/env python3
"""Sequencer recovery tests: the eighth review's WAL tail case (P0-1/P0-3).

The bug it pins down: a crash leaves a torn final record, `_replay` skipped it without
truncating, the next commit appended behind it, and the next restart parsed the merged
line as garbage -- so a commit the sequencer had acknowledged was gone.

Run: python3 experiments/service/test_wal_recovery.py
"""
import importlib.util
import json
import os
import sys
import tempfile

spec = importlib.util.spec_from_file_location(
    "seqd", os.path.join(os.path.dirname(os.path.abspath(__file__)), "seqd.py"))
seqd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seqd)

fails = 0
total = 0


def check(name, cond, detail=""):
    global fails, total
    total += 1
    print(f"[{'PASS' if cond else 'FAIL'}] {name}{('  ' + detail) if detail and not cond else ''}")
    if not cond:
        fails += 1


def write_wal(path, records, tail=""):
    with open(path, "w", encoding="utf-8") as f:
        for r in records:
            f.write(json.dumps(r) + "\n")
        f.write(tail)


def main():
    d = tempfile.mkdtemp()
    p = os.path.join(d, "seq.wal")

    # 1. torn tail: recover the good prefix, truncate, and keep committing
    write_wal(p, [{"version": 1, "role": 0, "kind": "grant", "doc": 13},
                  {"version": 2, "role": 0, "kind": "revoke", "doc": 7}],
              tail='{"version": 3, "role": 0, "kind": "gr')
    s = seqd.State(p)
    check("torn tail: recovered version 2", s.version == 2, f"got {s.version}")
    size_after_replay = os.path.getsize(p)
    check("torn tail: truncated to the last good record", size_after_replay > 0 and
          open(p, encoding="utf-8").read().endswith("\n"),
          f"size {size_after_replay}")
    v = s.commit(0, "grant", 42)
    check("torn tail: the next commit is version 3", v == 3, f"got {v}")
    s2 = seqd.State(p)
    check("torn tail: the acknowledged commit survives a restart", s2.version == 3,
          f"got {s2.version}")
    check("torn tail: the event list matches the log", len(s2.events) == 3,
          f"got {len(s2.events)}")

    # 2. a version gap must not be renumbered
    p2 = os.path.join(d, "gap.wal")
    write_wal(p2, [{"version": 1, "role": 0, "kind": "grant", "doc": 1},
                   {"version": 3, "role": 0, "kind": "grant", "doc": 2}])
    try:
        seqd.State(p2)
        check("version gap: refuses to start", False, "started anyway")
    except RuntimeError:
        check("version gap: refuses to start", True)

    # 3. corruption before the tail must not be silently dropped
    p3 = os.path.join(d, "mid.wal")
    write_wal(p3, [{"version": 1, "role": 0, "kind": "grant", "doc": 1}],
              tail="{not json}\n" + json.dumps({"version": 2, "role": 0, "kind": "grant", "doc": 3}) + "\n")
    try:
        seqd.State(p3)
        check("mid-file corruption: refuses to start", False, "started anyway")
    except RuntimeError:
        check("mid-file corruption: refuses to start", True)


    # 4. a failed append must not consume a version
    p4 = os.path.join(d, "fail_write.wal")
    s = seqd.State(p4)

    class BoomWal:
        def __init__(self, f):
            self.f = f
        def write(self, *a, **k):
            raise OSError("injected write failure")
        def flush(self):
            return self.f.flush()
        def fileno(self):
            return self.f.fileno()
        def close(self):
            return self.f.close()

    good = s.wal
    s.wal = BoomWal(good)
    try:
        s.commit(0, "grant", 1)
        check("write failure: commit raises", False, "returned")
    except OSError:
        check("write failure: commit raises", True)
    check("write failure: version stays at 0", s.version == 0, f"got {s.version}")
    check("write failure: sequencer frozen", s.failed is True)
    try:
        s.commit(0, "grant", 2)
        check("write failure: next commit refused", False, "returned")
    except RuntimeError:
        check("write failure: next commit refused", True)
    s.wal = good
    s.wal.close()
    s2 = seqd.State(p4)
    check("write failure: restart sees version 0", s2.version == 0, f"got {s2.version}")
    check("write failure: next durable version is 1", s2.commit(0, "grant", 3) == 1)
    s2.wal.close()

    # 5. a failed fsync may leave the record on disk; recovery decides, no gap
    p5 = os.path.join(d, "fail_fsync.wal")
    s = seqd.State(p5)
    orig_fsync = seqd.os.fsync
    def bad_fsync(fd):
        raise OSError("injected fsync failure")
    seqd.os.fsync = bad_fsync
    try:
        s.commit(0, "grant", 4)
        check("fsync failure: commit raises", False, "returned")
    except OSError:
        check("fsync failure: commit raises", True)
    finally:
        seqd.os.fsync = orig_fsync
    check("fsync failure: version stays at 0", s.version == 0, f"got {s.version}")
    check("fsync failure: sequencer frozen", s.failed is True)
    s.wal.close()
    s2 = seqd.State(p5)
    check("fsync failure: recovery sees at most one record", s2.version in (0, 1),
          f"got {s2.version}")
    base = s2.version
    v = s2.commit(0, "grant", 5)
    check("fsync failure: next version has no gap", v == base + 1,
          f"base {base}, got {v}")
    s2.wal.close()

    print(f"\n{'PASS' if fails == 0 else 'FAIL'}: {total - fails}/{total}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
