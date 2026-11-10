#!/usr/bin/env python3
"""Emit the LaTeX rows for the supplement's repair table from aggregate.json."""
import json, sys
from pathlib import Path

root = Path(sys.argv[1])
d = json.loads((root / "aggregate.json").read_text())

def get(stream, changes, mfac, variant, field="missing"):
    return d.get(f"{stream}_{changes}_{mfac}_{variant}", {}).get(field)

print("% revoke-only: phi, rho, lease, lease+repair, rebuild, in-place(no repair)")
for ch in (10000, 20000, 30000):
    phi = int(ch / 500)
    for mf in (1, 3, 15):
        row = [get("revoke", ch, mf, v) for v in ("g_pre", "g_rep", "g_rebuild", "u_pre")]
        if any(x is None for x in row):
            continue
        print(f"{phi}\\% & {mf} & " + " & ".join(f"{x:.0f}" for x in row) + r" \\")

print()
print("% grant-only")
for ch in (5000, 10000):
    g = int(ch / 500)
    for mf in (1, 15):
        row = [get("grant", ch, mf, v) for v in ("g_pre", "g_rep", "g_rebuild", "g_nodelta_rep")]
        auth = [get("grant", ch, mf, v, "auth") for v in ("g_pre", "g_rep", "g_nodelta_rep")]
        if any(x is None for x in row):
            continue
        print(f"{g}\\% & {mf} & " + " & ".join(f"{x:.0f}" for x in row)
              + " & " + "/".join(f"{a:.0f}" for a in auth) + r" \\")

print()
print("% p95 (ms) for the lease arms, revoke rows")
for ch in (10000, 20000, 30000):
    phi = int(ch / 500)
    for mf in (1, 3, 15):
        a = get("revoke", ch, mf, "g_pre", "p95"); b = get("revoke", ch, mf, "g_rep", "p95")
        if a is None:
            continue
        print(f"{phi}\\% & {mf} & {a:.3f} & {b:.3f} \\\\")
