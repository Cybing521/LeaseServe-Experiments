#!/usr/bin/env python3
"""Aggregate results/repair_20261109/*/summary.txt into a markdown table + JSON.

Each run directory holds one seed.  The post-maintenance variants are:
  g_pre        lease's published graph, no repair
  g_rep        after ROE+RDN+RUE
  g_rebuild    rebuilt from the visible set (deep maintenance)
  u_pre        re-inserted graph, no repair
  u_rep        re-inserted graph after the same repair
  g_nodelta_rep  repaired graph without the grant log (coverage control)
"""
import json, re, statistics as st, sys
from pathlib import Path

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
runs = []
for d in sorted(root.glob("*/")):
    cmd = (d / "cmd.txt").read_text().splitlines()[0] if (d / "cmd.txt").exists() else ""
    sm = d / "summary.txt"
    if not sm.exists():
        continue
    m = dict(re.findall(r"(\w+)=([\w.]+)", cmd))
    rec = {"dir": d.name, "aggr": int(m.get("AGGR", -1)), "changes": int(m.get("changes", 0)),
           "mfac": int(m.get("mfac", 0)), "seed": int(m.get("seed", 0))}
    for line in sm.read_text().splitlines():
        parts = line.split()
        if parts[0] == "post":
            name = parts[1]
            rec.setdefault("post", {})[name] = {k: float(v) for k, v in
                                                (p.split("=") for p in parts[2:])}
        elif parts[0] == "health":
            rec.setdefault("health", {})[parts[1]] = {k: float(v) for k, v in
                                                      (p.split("=") for p in parts[2:])}
        elif parts[0] == "maintenance":
            rec.setdefault("maint", {}).update({k: float(v) for k, v in
                                                (p.split("=") for p in parts[1:])})
    runs.append(rec)

def agg(key, variant, field):
    vals = [r["post"][variant][field] for r in runs
            if key(r) and "post" in r and variant in r["post"]]
    if not vals:
        return None
    return (st.mean(vals), st.pstdev(vals) if len(vals) > 1 else 0.0, len(vals))

def key_of(r):
    return (r["aggr"], r["changes"], r["mfac"])

groups = {}
for r in runs:
    groups.setdefault(key_of(r), []).append(r)

print("| stream | changes | dead% | rho | variant | missing (mean±sd) | auth | p95 ms |")
print("|---|---|---|---|---|---|---|---|")
order = ["g_pre", "g_rep", "g_rebuild", "u_pre", "u_rep", "g_nodelta_rep"]
out = {}
for k in sorted(groups):
    aggr, changes, mfac = k
    nrole = 10
    per = changes / nrole
    dead = per / 5000.0
    label = "grant" if aggr == 100 else "revoke"
    for v in order:
        ms, sds, n = [], [], 0
        for r in groups[k]:
            if "post" in r and v in r["post"]:
                ms.append(r["post"][v]["missing"]); sds.append(r["post"][v]["p95"]); n += 1
        if not ms:
            continue
        print(f"| {label} | {changes} | {dead*100:.0f}% | {mfac} | {v} | "
              f"{st.mean(ms):.1f}±{(st.pstdev(ms) if n > 1 else 0):.1f} | "
              f"{st.mean([r['post'][v]['auth'] for r in groups[k] if v in r['post']]):.1f} | "
              f"{st.mean(sds):.3f} |")
        out[f"{label}_{changes}_{mfac}_{v}"] = {
            "missing": st.mean(ms), "missing_sd": st.pstdev(ms) if n > 1 else 0.0,
            "auth": st.mean([r["post"][v]["auth"] for r in groups[k] if v in r["post"]]),
            "p50": st.mean([r["post"][v]["p50"] for r in groups[k] if v in r["post"]]),
            "p95": st.mean(sds), "seeds": n}

hs = {}
for r in runs:
    if "health" in r:
        for g in ("g_pre", "g_rep", "u_pre", "u_rep"):
            for f, val in r["health"].get(g, {}).items():
                hs.setdefault(g, {}).setdefault(f, []).append(val)
print("\nhealth (sum over 10 roles, mean across all runs):")
for g, d in hs.items():
    print(" ", g, {f: round(st.mean(v), 1) for f, v in d.items()},
          f"(n={len(d.get('alive', []))})")
ms = [r["maint"]["repair_ms_g"] for r in runs if "maint" in r]
rb = [r["maint"]["rebuild_ms"] for r in runs if "maint" in r]
if ms:
    print(f"\nrepair_ms_g mean={st.mean(ms):.1f}  rebuild_ms mean={st.mean(rb):.0f}  n={len(ms)}")
Path(root / "aggregate.json").write_text(json.dumps(out, indent=1))
print("\nwrote", root / "aggregate.json")
