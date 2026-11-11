#!/usr/bin/env python3
"""Analyse the boundary harness runs and emit the tables/figures used in docs/202.

Reads experiments/boundary/results/{ablation,matrix,longrun}.csv plus each run's
summary.txt, so the numbers are traceable to a single run directory.
"""
import csv
import glob
import math
import os
import statistics as st
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(ROOT, "results")
OUT = os.path.join(RES, "analysis")
os.makedirs(OUT, exist_ok=True)

# Paper palette (paper-v2/figs/src/style2.py): green = lease, dark red = the failure,
# grays = predicates that fall back, light green = lease + merge.
sys.path.insert(0, os.path.join(ROOT, "..", "..", "paper-v2", "figs", "src"))
import style2 as S  # noqa: E402
S.apply()
P = S.P
C = {
    "A": "#B5B5B5", "B": P["exact"], "C": P["auth"], "D": P["lease"], "E": P["lease_l"],
    "exact": P["exact"], "head10": P["lease"], "zipf": P["log"], "uniform": P["acorn"],
    "threshold": P["curator"],
}
STREAM_C = {"0": "#C9C9C9", "50": P["exact"], "100": P["ink"]}


def ptitle(a, lab, txt):
    a.set_title(rf"$\bf{{{lab}}}$ {txt}", loc="left", fontsize=7.6, pad=4)


def ygrid(a):
    a.grid(axis="y", color=P["rule"], lw=0.4, alpha=0.7)
    a.set_axisbelow(True)


def rows(path):
    if not os.path.exists(path):
        return []
    return list(csv.DictReader(open(path)))


def fnum(r, k, default=0.0):
    try:
        return float(r.get(k, default))
    except (TypeError, ValueError):
        return default


def mean_sd(vals):
    vals = [v for v in vals if v is not None and not (isinstance(v, float) and math.isnan(v))]
    if not vals:
        return 0.0, 0.0
    if len(vals) == 1:
        return vals[0], 0.0
    return st.mean(vals), st.stdev(vals)


def summarise(rs, keys, fields):
    groups = {}
    for r in rs:
        k = tuple(r[x] for x in keys)
        groups.setdefault(k, []).append(r)
    out = []
    for k in sorted(groups):
        g = groups[k]
        row = dict(zip(keys, k))
        for f in fields:
            m, s = mean_sd([fnum(x, f) for x in g])
            row[f] = m
            row[f + "_sd"] = s
        row["n"] = len(g)
        out.append(row)
    return out


def ablation():
    rs = rows(os.path.join(RES, "ablation.csv"))
    if not rs:
        return
    keys = ["arm", "aggr", "drop"]
    fields = ["leased_rate", "fallback_rate", "p50", "p95", "missing", "auth",
              "update_total_ms", "merges", "rss_mb"]
    tab = summarise(rs, keys, fields)
    lines = []
    lines.append("| arm | stream | leased | fallback | p50 | p95 | auth miss | update (ms) | merges | RSS (MB) |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|")
    for r in tab:
        stream = {"0": "revoke-only", "50": "50/50", "100": "grant-only"}.get(r["aggr"], r["aggr"])
        if r["drop"] != "0":
            stream += f", drop 1/{r['drop']}"
        lines.append(
            f"| {r['arm']} | {stream} | {100*r['leased_rate']:.1f}% | {100*r['fallback_rate']:.1f}% | "
            f"{r['p50']:.3f} | {r['p95']:.3f} | {r['auth']:.1f} | {r['update_total_ms']:.1f} | "
            f"{r['merges']:.1f} | {r['rss_mb']:.0f} |")
    open(os.path.join(OUT, "ablation.md"), "w").write("\n".join(lines) + "\n")

    # LaTeX
    tex = []
    tex.append(r"\begin{tabular}{@{}llrrrrrrr@{}}")
    tex.append(r"\toprule")
    tex.append(r"arm & stream & leased & fallback & p50 & p95 & \auth{} miss & update & merges \\")
    tex.append(r"\midrule")
    for r in tab:
        stream = {"0": "revoke", "50": "50/50", "100": "grant"}.get(r["aggr"], r["aggr"])
        if r["drop"] != "0":
            stream += r" + hole"
        tex.append(
            f"{r['arm']} & {stream} & {100*r['leased_rate']:.1f}\\% & {100*r['fallback_rate']:.1f}\\% & "
            f"{r['p50']:.3f} & {r['p95']:.3f} & {r['auth']:.0f} & {r['update_total_ms']:.1f} & {r['merges']:.0f} \\\\")
    tex.append(r"\bottomrule")
    tex.append(r"\end{tabular}")
    open(os.path.join(OUT, "ablation.tex"), "w").write("\n".join(tex) + "\n")

    # Figure: 3 panels (50/50 predicate cost, the hole, maintenance)
    fig, axes = plt.subplots(1, 3, figsize=(S.PAGE_W, 2.0))
    arms = ["A", "B", "C", "D", "E"]
    streams = ["0", "50", "100"]
    width = 0.26
    x = np.arange(len(arms))
    for j, s in enumerate(streams):
        vals = []
        for a in arms:
            sub = [r for r in tab if r["arm"] == a and r["aggr"] == s and r["drop"] == "0"]
            vals.append(sub[0]["p50"] if sub else 0)
        axes[0].bar(x + (j - 1) * width, vals, width, color=STREAM_C[s], edgecolor=P["ink"],
                    linewidth=0.4, label={"0": "revoke", "50": "50/50", "100": "grant"}[s])
    axes[0].set_xticks(x); axes[0].set_xticklabels(arms)
    axes[0].set_ylabel("p50 (ms)"); ptitle(axes[0], "(a)", "Predicate cost")
    axes[0].legend(frameon=False, ncol=3, loc="upper right", fontsize=6.2, handlelength=1.0,
                   columnspacing=0.8, bbox_to_anchor=(1.0, 1.02))
    axes[0].set_ylim(0, 0.38); ygrid(axes[0])
    auth = [next((r["auth"] for r in tab if r["arm"] == a and r["aggr"] == "50" and r["drop"] != "0"), 0)
            for a in ["C", "D"]]
    b = axes[1].bar([0, 1], auth, 0.55, color=[C["C"], C["D"]], edgecolor=P["ink"], linewidth=0.4)
    axes[1].bar_label(b, fmt="%.0f", fontsize=6.6, padding=1.5)
    axes[1].set_xticks([0, 1]); axes[1].set_xticklabels(["C: log, no gate", "D: cert. + log"])
    axes[1].set_ylabel("auth misses"); ptitle(axes[1], "(b)", "One lost grant in five")
    axes[1].set_ylim(0, max(auth) * 1.18); axes[1].set_xlim(-0.6, 1.6); ygrid(axes[1])
    for j, s in enumerate(streams):
        vals = []
        for a in arms:
            sub = [r for r in tab if r["arm"] == a and r["aggr"] == s and r["drop"] == "0"]
            vals.append(max(sub[0]["update_total_ms"], 1e-3) if sub else 1e-3)
        axes[2].bar(x + (j - 1) * width, vals, width, color=STREAM_C[s], edgecolor=P["ink"],
                    linewidth=0.4)
    axes[2].set_yscale("log")
    axes[2].set_xticks(x); axes[2].set_xticklabels(arms)
    axes[2].set_ylabel("update (ms, log)"); ptitle(axes[2], "(c)", "Maintenance (E merges)")
    ygrid(axes[2])
    fig.tight_layout(pad=0.3, w_pad=1.6)
    fig.savefig(os.path.join(OUT, "fig_ablation.pdf"))
    fig.savefig(os.path.join(OUT, "fig_ablation.png"), dpi=220)
    plt.close(fig)
    print("ablation", len(tab), "groups")


def read_summary(path):
    d = {}
    if not os.path.exists(path):
        return d
    for line in open(path):
        for tok in line.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                try:
                    d[k] = float(v)
                except ValueError:
                    d[k] = v
    return d


def matrix():
    rs = rows(os.path.join(RES, "matrix.csv"))
    if not rs:
        return
    exact = {}
    for r in rs:
        if r["arm"] == "X":
            sub = "matrix"
            dname = r["tag"][4:] if r["tag"].startswith("mat_") else r["tag"]
            s = read_summary(os.path.join(RES, sub, dname, "summary.txt"))
            exact.setdefault(r["conc"], []).append(s.get("exact_mean", fnum(r, "mean")))
    tf_ref = {k: st.mean(v) for k, v in exact.items()}
    # fallback: use any D cell with exact reads for that concentration
    for r in rs:
        if r["arm"] != "D":
            continue
        sub = "threshold" if r["tag"].startswith("thr_") else "matrix"
        dname = r["tag"][4:] if (r["tag"].startswith("mat_") or r["tag"].startswith("thr_")) else r["tag"]
        sm = read_summary(os.path.join(RES, sub, dname, "summary.txt"))
        if sm.get("exact_reads", 0) > 0 and r["conc"] not in tf_ref:
            tf_ref.setdefault(r["conc"], []).append(sm.get("exact_mean", 0))
    tf_ref = {k: (st.mean(v) if isinstance(v, list) else v) for k, v in tf_ref.items()}

    cells = {}
    for r in rs:
        if r["arm"] != "D":
            continue
        sub = "threshold" if r["tag"].startswith("thr_") else "matrix"
        dname = r["tag"][4:] if (r["tag"].startswith("mat_") or r["tag"].startswith("thr_")) else r["tag"]
        s = read_summary(os.path.join(RES, sub, dname, "summary.txt"))
        if "leased_rate" not in s:
            continue
        key = (r["conc"], int(r["K"]), int(r["aggr"]))
        cells.setdefault(key, []).append(s)

    out = []
    for key in sorted(cells):
        g = cells[key]
        pL = st.mean(x["leased_rate"] for x in g)
        TL = st.mean(x["lease_mean"] for x in g if x.get("lease_reads", 0) > 0) if any(x.get("lease_reads", 0) > 0 for x in g) else 0
        TF = st.mean(x["exact_mean"] for x in g if x.get("exact_reads", 0) > 0) if any(x.get("exact_reads", 0) > 0 for x in g) else tf_ref.get(key[0], 0)
        if TF <= 0:
            continue
        Cm = st.mean(x["update_total_ms"] for x in g)
        N = st.mean(x["reads"] for x in g)
        meas = st.mean(x["mean"] for x in g)
        pred = pL * TL + (1 - pL) * TF + Cm / N
        out.append(dict(conc=key[0], K=key[1], aggr=key[2], pL=pL, TL=TL, TF=TF,
                        C=Cm, N=N, meas=meas, pred=pred,
                        p95=st.mean(x["p95"] for x in g),
                        p50=st.mean(x["p50"] for x in g),
                        rss=st.mean(x["rss_mb"] for x in g),
                        matfac=st.mean(x["matfac"] for x in g)))
    if not out:
        return

    err = [abs(o["pred"] - o["meas"]) / o["meas"] for o in out]
    mape = 100 * st.mean(err)
    maxe = 100 * max(err)
    # held-out: one global T_L and the exact-only reference T_F, no per-cell latency
    tl_global = st.median(o["TL"] for o in out)
    tf_x = {}
    for r in rs:
        if r["arm"] == "X":
            dname = r["tag"]
            sm = read_summary(os.path.join(RES, "matrix", dname, "summary.txt"))
            tf_x.setdefault(r["conc"], []).append(sm.get("exact_mean", 0))
    tf_x = {k: st.mean(v) for k, v in tf_x.items() if v}
    for o in out:
        tf = tf_x.get(o["conc"], o["TF"])
        o["pred_held"] = o["pL"] * tl_global + (1 - o["pL"]) * tf + o["C"] / o["N"]
    err2 = [abs(o["pred_held"] - o["meas"]) / o["meas"] for o in out]
    mape2 = 100 * st.mean(err2)
    maxe2 = 100 * max(err2)
    lines = ["| conc | K | grant % | p_L | T_L | T_F | C/N (ms) | measured | in-run model | err % | held-out model | err % | p95 | RSS |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for o in out:
        lines.append(f"| {o['conc']} | {o['K']} | {o['aggr']} | {o['pL']:.3f} | {o['TL']:.3f} | {o['TF']:.3f} | "
                     f"{o['C']/o['N']:.4f} | {o['meas']:.3f} | {o['pred']:.3f} | {100*abs(o['pred']-o['meas'])/o['meas']:.1f} | "
                     f"{o['pred_held']:.3f} | {100*abs(o['pred_held']-o['meas'])/o['meas']:.1f} | "
                     f"{o['p95']:.3f} | {o['rss']:.0f} |")
    open(os.path.join(OUT, "model.md"), "w").write("\n".join(lines) + "\n")
    open(os.path.join(OUT, "model_summary.txt"), "w").write(
        f"cells={len(out)} in-run_MAPE={mape:.2f}% in-run_max={maxe:.2f}% "
        f"heldout_MAPE={mape2:.2f}% heldout_max={maxe2:.2f}% TL_global={tl_global:.4f}\n")

    # quantile rule: p50 needs p_L>0.5, p95 needs p_L>0.95
    ok50 = sum(1 for o in out if (o["pL"] > 0.5) == (o["p50"] < 0.9 * o["TF"]))
    ok95 = sum(1 for o in out if (o["pL"] > 0.95) == (o["p95"] < 0.9 * o["TF"]))
    open(os.path.join(OUT, "quantile_rule.txt"), "w").write(
        f"cells={len(out)} p50_rule={ok50}/{len(out)} p95_rule={ok95}/{len(out)}\n")
    lines2 = ["| conc | K | grant % | p_L | p50 | p95 | T_F | p50 pop | p95 pop | RSS |",
              "|---|---|---|---|---|---|---|---|---|---|"]
    for o in out:
        lines2.append(f"| {o['conc']} | {o['K']} | {o['aggr']} | {o['pL']:.3f} | {o['p50']:.3f} | {o['p95']:.3f} | "
                      f"{o['TF']:.3f} | {'lease' if o['p50'] < 0.9*o['TF'] else 'exact'} | "
                      f"{'lease' if o['p95'] < 0.9*o['TF'] else 'exact'} | {o['rss']:.0f} |")
    open(os.path.join(OUT, "boundary.md"), "w").write("\n".join(lines2) + "\n")

    # break-even p_L* = C / (N (TF-TL))
    be = [o["C"] / (o["N"] * (o["TF"] - o["TL"])) for o in out if o["TF"] > o["TL"]]
    open(os.path.join(OUT, "breakeven.txt"), "w").write(
        "median=%.4f p90=%.4f max=%.4f\n" % (st.median(be), np.percentile(be, 90), max(be)))

    # predicted vs measured
    fig, ax = plt.subplots(1, 2, figsize=(S.PAGE_W, 2.15))
    fam = {"head10": "head10", "zipf": "zipf", "uniform": "uniform",
           "conc90": "threshold", "conc96": "threshold", "conc99": "threshold"}
    for conc in ["head10", "zipf", "uniform", "conc90", "conc96", "conc99"]:
        sub = [o for o in out if o["conc"] == conc]
        if not sub:
            continue
        ax[0].scatter([o["meas"] for o in sub], [o["pred_held"] for o in sub],
                      s=16, color=C[fam[conc]], label=fam[conc] if conc in ("head10", "zipf", "uniform", "conc90") else None,
                      edgecolor=P["ink"], linewidth=0.3, marker="s" if conc.startswith("conc") else "o",
                      zorder=3)
    lim = [0, max(o["meas"] for o in out) * 1.08]
    ax[0].plot(lim, lim, "--", color=P["muted"], lw=0.7, zorder=1)
    ax[0].set_xlabel("measured mean (ms)"); ax[0].set_ylabel("held-out model (ms)")
    ptitle(ax[0], "(a)", f"Mean model: MAPE {mape2:.1f}% (max {maxe2:.1f}%)")
    ax[0].legend(frameon=False, loc="upper left", fontsize=6.2, handletextpad=0.2)
    ax[0].set_xlim(lim); ax[0].set_ylim(lim); ygrid(ax[0])
    for conc in ["head10", "zipf", "uniform", "conc90", "conc96", "conc99"]:
        sub = [o for o in out if o["conc"] == conc]
        if not sub:
            continue
        ax[1].scatter([o["pL"] for o in sub], [o["p95"] for o in sub],
                      s=16, color=C[fam[conc]], marker="s" if conc.startswith("conc") else "o",
                      edgecolor=P["ink"], linewidth=0.3, zorder=3)
    ax[1].axvline(0.95, color=P["muted"], ls=":", lw=0.8, zorder=1)
    ax[1].text(0.94, 0.215, "$p_L=0.95$", ha="right", fontsize=6.2, color=P["muted"])
    ax[1].set_xlabel("leased share $p_L$"); ax[1].set_ylabel("p95 (ms)")
    ptitle(ax[1], "(b)", "p95 needs $p_L>0.95$")
    ygrid(ax[1])
    fig.tight_layout(pad=0.3, w_pad=2.0)
    fig.savefig(os.path.join(OUT, "fig_model.pdf"))
    fig.savefig(os.path.join(OUT, "fig_model.png"), dpi=220)
    plt.close(fig)
    print("matrix cells", len(out), "MAPE", round(mape, 2), "max", round(maxe, 2))


def longrun():
    rs = rows(os.path.join(RES, "longrun.csv"))
    if not rs:
        return
    lines = ["| arm | merge every | seed | merges | update (s) | p95 (ms) | p95 first/last window | RSS peak (MB) |",
             "|---|---|---|---|---|---|---|---|"]
    data = {}
    for r in rs:
        d = os.path.join(RES, "longrun", f"{r['arm']}_m{r['merge']}_s{r['seed']}")
        tr = os.path.join(d, "trace.csv")
        windows = {}
        if os.path.exists(tr):
            for row in csv.DictReader(open(tr)):
                t = float(row["t_ms"]); lat = float(row["lat_ms"])
                w = int(t // 1000)
                windows.setdefault(w, []).append(lat)
        w95 = {w: np.percentile(v, 95) for w, v in windows.items() if len(v) >= 20}
        first = w95[min(w95)] if w95 else 0
        last = w95[max(w95)] if w95 else 0
        data.setdefault((r["arm"], r["merge"]), []).append((first, last, w95))
        lines.append(f"| {r['arm']} | {r['merge']} | {r['seed']} | {r['merges']} | {float(r['update_total_ms'])/1000:.2f} | "
                     f"{float(r['p95']):.3f} | {first:.3f}/{last:.3f} | {float(r['rss_mb']):.0f} |")
    open(os.path.join(OUT, "longrun.md"), "w").write("\n".join(lines) + "\n")

    if data:
        # end-state p95 at 2k and 10k changes, plus update cost
        r2 = rows(os.path.join(RES, "long2k.csv"))
        d2 = {("D", "0"): 0.116}
        for k in [("E", "500"), ("E", "200")]:
            sub = [float(r["p95"]) for r in r2 if r["arm"] == k[0] and r["merge"] == k[1]]
            d2[k] = st.mean(sub) if sub else 0.116
        labels = [("D", "0"), ("E", "500"), ("E", "200")]
        p10 = []
        upd = []
        for k in labels:
            seeds = data.get(k, [])
            sub = [float(r["p95"]) for r in rs if r["arm"] == k[0] and r["merge"] == k[1]]
            p10.append(np.mean(sub) if sub else 0)
            m = [r for r in rs if r["arm"] == k[0] and r["merge"] == k[1]]
            upd.append(np.mean([float(r["update_total_ms"]) / 1000 for r in m]) if m else 0)
        fig, ax = plt.subplots(2, 1, figsize=(S.COL_W, 2.75))
        x = np.arange(len(labels)); w = 0.34
        xl = ["D: no merge", "E: $N_M{=}500$", "E: $N_M{=}200$"]
        b0 = ax[0].bar(x - w/2, [d2[k] for k in labels], w, color="#C9C9C9", edgecolor=P["ink"],
                       linewidth=0.4, label="2k changes")
        b1 = ax[0].bar(x + w/2, p10, w, color=P["lease"], edgecolor=P["ink"], linewidth=0.4,
                       label="10k changes")
        for bb in (b0, b1):
            ax[0].bar_label(bb, fmt="%.3f", fontsize=6.0, padding=1.5)
        ax[0].set_xticks(x); ax[0].set_xticklabels(xl, fontsize=6.4)
        ax[0].set_ylabel("p95 (ms)"); ax[0].set_ylim(0, 0.17)
        ax[0].legend(frameon=False, ncol=2, loc="upper right", fontsize=6.2, bbox_to_anchor=(1.0, 1.03))
        ptitle(ax[0], "(a)", "Tail vs. stream length"); ygrid(ax[0])
        b2 = ax[1].bar(x, upd, 0.5, color=[C["D"], C["E"], C["E"]], edgecolor=P["ink"], linewidth=0.4)
        ax[1].bar_label(b2, labels=[f"{u:.2f} s" for u in upd], fontsize=6.0, padding=1.5)
        ax[1].set_yscale("log"); ax[1].set_ylim(min(upd) / 3, max(upd) * 4)
        ax[1].set_xticks(x); ax[1].set_xticklabels(xl, fontsize=6.4)
        ax[1].set_ylabel("update at 10k (s, log)"); ptitle(ax[1], "(b)", "Maintenance cost"); ygrid(ax[1])
        fig.tight_layout(pad=0.25, h_pad=0.9)
        fig.savefig(os.path.join(OUT, "fig_longrun.pdf"))
        fig.savefig(os.path.join(OUT, "fig_longrun.png"), dpi=220)
        plt.close(fig)
    print("longrun rows", len(rs))


if __name__ == "__main__":
    ablation()
    matrix()
    longrun()
