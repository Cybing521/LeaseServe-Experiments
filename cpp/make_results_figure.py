#!/usr/bin/env python3
"""Fig. 3 -- the results figure (four panels, one page width).

Data provenance (do not edit numbers here without editing the source run):
  (a) regime boundary: docs/117 (128d decouple sweep, Gold 6330, 3 runs) and
      results/crossover_5rep_20261107/SUMMARY_5rep.md (768d, 5 reps, 9950X).
  (b) three-axis decomposition: docs/137 table (= results/b1_1m_20261104/*.json,
      3 seeds mean); identical totals to tab:laws.
  (c) grant + revoke, five designs: experiments/mechanism/results/arms_20261104/
      uniform_s{0..4}.json (five seeds).
  (d) 10M: synthetic 128d (results/p4_20261107/cpp128, 5 seeds) and real SIFT10M
      (results/sift10m_real_20261107 K=3; results/sift10m_K10 K=10, seed 0).

Run from paper-v2/:
    FIG_OUT=figs MPLCONFIGDIR=/tmp/mplcache python3 ../experiments/cpp/make_results_figure.py
"""
import glob
import json
import os

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap, TwoSlopeNorm

OUT = os.environ.get("FIG_OUT") or os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "..", "paper-v2", "figs")
os.makedirs(OUT, exist_ok=True)

plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Times New Roman", "STIX Two Text", "DejaVu Serif"],
    "mathtext.fontset": "stix",
    "font.size": 8.4,
    "axes.labelsize": 8.6,
    "xtick.labelsize": 8.2,
    "ytick.labelsize": 8.2,
    "legend.fontsize": 7.8,
    "axes.linewidth": 0.55,
    "axes.edgecolor": "black",
    "axes.labelcolor": "black",
    "xtick.direction": "in",
    "ytick.direction": "in",
    "xtick.major.width": 0.5,
    "ytick.major.width": 0.5,
    "xtick.major.size": 2.4,
    "ytick.major.size": 2.4,
    "savefig.dpi": 600,
})

# One palette for the whole paper: the three axes, then the design families.
C_SEL = "#0072B2"     # candidate budget
C_ANN = "#D55E00"     # navigation
C_AUTH = "#009E73"    # authorization gap
C_LEASE = "#0072B2"
C_NEUTRAL = "#6E6E6E"
C_SYNTH = "#B9B9B9"
C_REAL = "#1F6FB2"


def panel_label(ax, text, dx=0.02, dy=0.965):
    ax.text(dx, dy, text, transform=ax.transAxes, ha="left", va="top", fontsize=8.2)


# ---------------------------------------------------------------- (a) regime
# exact / lease speedup; >1 means the lease is faster.
ratios = np.array([
    [np.nan, 1.34, 4.39, 8.35, 10.2],
    [0.68, 3.58, 15.2, 24.5, 40.6],
])
cell_labels = [["—", "1.3×", "4.4×", "8.4×", "10×"],
               ["0.68×", "3.6×", "15×", "25×", "41×"]]
cmap = LinearSegmentedColormap.from_list(
    "lease", ["#f7fbff", "#c6dbef", "#6baed6", "#2171b5", "#08306b"])
norm = TwoSlopeNorm(vmin=0.5, vcenter=1.0, vmax=42)

fig = plt.figure(figsize=(7.16, 3.45))
gs = fig.add_gridspec(2, 2, left=0.098, right=0.985, top=0.90, bottom=0.10,
                      wspace=0.30, hspace=0.55)
axa = fig.add_subplot(gs[0, 0])
axb = fig.add_subplot(gs[0, 1])
axc = fig.add_subplot(gs[1, 0])
axd = fig.add_subplot(gs[1, 1])

for i in range(2):
    for j in range(5):
        r = ratios[i, j]
        if np.isnan(r):
            axa.add_patch(plt.Rectangle((j, i), 1, 1, facecolor="white",
                                        edgecolor="#bbbbbb", lw=0.6, ls=":"))
            axa.text(j + 0.5, i + 0.5, "—", ha="center", va="center", fontsize=8.8)
        else:
            face = "#f2f2f2" if r < 1 else cmap(norm(r))
            axa.add_patch(plt.Rectangle((j, i), 1, 1, facecolor=face,
                                        edgecolor="black" if r < 1 else "white",
                                        lw=1.1 if r < 1 else 0.4))
            axa.text(j + 0.5, i + 0.5, cell_labels[i][j], ha="center", va="center",
                     fontsize=8.6, fontweight="bold")
axa.set_xlim(0, 5)
axa.set_ylim(2, 0)
axa.set_xticks([0.5, 1.5, 2.5, 3.5, 4.5])
axa.set_xticklabels(["2k", "5k", "20k", "36k", "50k"], fontsize=8.2)
axa.set_yticks([0.5, 1.5])
axa.set_yticklabels(["128d\nSIFT", "768d\nwiki"], fontsize=8.2)
axa.set_xlabel("documents per role", fontsize=8.6)
axa.tick_params(length=0)
for s in axa.spines.values():
    s.set_visible(False)
sm = plt.cm.ScalarMappable(cmap=cmap, norm=norm)
cb = fig.colorbar(sm, ax=axa, fraction=0.055, pad=0.02)
cb.set_label("exact / lease speedup", fontsize=7.4, labelpad=2)
cb.ax.tick_params(labelsize=7.0, length=1.8)
cb.outline.set_linewidth(0.4)
panel_label(axa, "(a)", dx=-0.06, dy=1.10)

# ---------------------------------------------------------------- (b) axes
acls = ["Uniform\n(0.5%)", "ERBAC\n(7-10%)", "OpenAlex\n(real)"]
designs = ["lease", "static", "partition", "shared"]
# (auth, ann, sel) per (design, acl), docs/137 (= results/b1_1m_20261104, 3 seeds)
# five-seed means (b1_1m_5seed, seeds 0-4)
decomp = {
    "lease":     [(0, 0.4, 0), (0, 17.4, 0), (0, 1.6, 0)],
    "static":    [(2987.8, 0, 0), (2980.0, 9.8, 0), (2786.0, 0, 0)],
    "partition": [(23.0, 344.4, 7053.4), (12.6, 90.4, 0), (30.2, 145.6, 3.6)],
    "shared":    [(0, 60.8, 16227.2), (0, 754.2, 338.8), (0, 428.8, 9647.2)],
}
def fmt(t):
    return "0" if t < 1e-6 else (f"{t/1000:.1f}k" if t >= 1000 else f"{t:.0f}")


x = np.arange(len(acls)) * 1.0
w = 0.20
for k, d in enumerate(designs):
    auth = np.array([decomp[d][i][0] for i in range(3)], float)
    ann = np.array([decomp[d][i][1] for i in range(3)], float)
    sel = np.array([decomp[d][i][2] for i in range(3)], float)
    tot = auth + ann + sel
    off = (k - 1.5) * w
    axb.bar(x + off, auth, w * 0.94, color=C_AUTH, edgecolor="black", linewidth=0.25)
    axb.bar(x + off, ann, w * 0.94, bottom=auth,
            color=C_ANN, edgecolor="black", linewidth=0.25)
    axb.bar(x + off, sel, w * 0.94, bottom=auth + ann,
            color=C_SEL, edgecolor="black", linewidth=0.25)
    # a bar this small is invisible: label its total so the design stays locatable
    for i in range(3):
        if tot[i] < 300:
            axb.text(x[i] + off, tot[i] + 320, fmt(tot[i]), ha="center", va="bottom",
                     fontsize=7.0, color="black")

tot_by_design = {d: [sum(decomp[d][i]) for i in range(3)] for d in designs}
group_lines = [a.replace("\n", " ") for a in acls]
axb.set_ylim(0, 18200)
axb.set_yticks([0, 5000, 10000, 15000])
axb.set_yticklabels(["0", "5k", "10k", "15k"])
axb.set_xticks(x)
axb.set_xticklabels(group_lines, fontsize=8.0)
axb.set_ylabel("missing top-10 results", fontsize=8.4)
from matplotlib.patches import Patch
handles = [Patch(facecolor=C_SEL, edgecolor="black", lw=0.25, label="sel"),
           Patch(facecolor=C_ANN, edgecolor="black", lw=0.25, label="ann"),
           Patch(facecolor=C_AUTH, edgecolor="black", lw=0.25, label="auth")]
axb.legend(handles=handles, loc="lower center", ncol=3, frameon=False,
           handlelength=0.9, columnspacing=0.9, fontsize=7.4,
           bbox_to_anchor=(0.5, 1.00))
panel_label(axb, "(b)", dx=-0.14, dy=1.12)

# ---------------------------------------------------------------- (c) grant
# Read the five seeds instead of pasting the means: the leak bar is the only
# non-structural safety number in the figure, so it must come from the files.
_ARM_JSON = sorted(glob.glob(os.path.join(
    os.path.dirname(__file__), "..", "mechanism", "results", "arms_20261104", "uniform_s*.json")))
if not _ARM_JSON:
    raise SystemExit("figure: no uniform_s*.json under mechanism/results/arms_20261104")
_source = [json.load(open(p))["arms"] for p in _ARM_JSON]


def _mean(arm, key):
    return sum(s[arm][key] for s in _source if arm in s) / len(_source)


_key = {"static": "static_nofilter", "static+f": "static_filter", "lease": "incremental",
        "shared+f": "shared", "partition+f": "partition"}
g_arms = list(_key)
g_missing = [_mean(_key[a], "missing") for a in g_arms]
g_p50 = [_mean(_key[a], "p50") for a in g_arms]
g_leaks = [_mean(_key[a], "leaks") for a in g_arms]
yg = np.arange(len(g_arms))[::-1]
colors = [C_NEUTRAL, C_NEUTRAL, C_LEASE, C_NEUTRAL, C_NEUTRAL]
axc.barh(yg, g_missing, 0.62, color=colors, edgecolor="black", linewidth=0.35)
axc.barh(yg, g_leaks, 0.62, color="#E8B4AF", edgecolor="#8C2F27", linewidth=0.45)
for yi, m, p, lk in zip(yg, g_missing, g_p50, g_leaks):
    axc.text(max(m, lk) + 90, yi, f"{m:g}", va="center", ha="left", fontsize=7.2)
    axc.text(6100, yi, f"{p:.1f} ms", va="center", ha="right", fontsize=7.0,
             color="#333333")
axc.set_yticks(yg)
axc.set_yticklabels(g_arms, fontsize=7.2)
axc.set_xlim(0, 6300)
axc.set_xticks([0, 2500, 5000])
axc.set_xlabel("missing top-10 results (of 5,000); right: p50", fontsize=8.2)
axc.tick_params(axis="y", length=0)
axc.text(g_leaks[0] / 2, yg[0], "584", ha="center", va="center", fontsize=6.8,
         color="#5A1A15")
panel_label(axc, "(c)", dx=-0.14, dy=1.10)

# ---------------------------------------------------------------- (d) 10M
groups = ["K=3\n(3 leases)", "K=10\n(full coverage)"]
synth = [1549, 4494]
real = [17, 52]
xg2 = np.arange(2)
axd.bar(xg2 - 0.17, synth, 0.32, color=C_SYNTH, edgecolor="black", linewidth=0.35,
        label="synthetic Gaussian")
axd.bar(xg2 + 0.17, real, 0.32, color=C_REAL, edgecolor="black", linewidth=0.35,
        label="real SIFT10M")
for xi, v in zip(xg2 - 0.17, synth):
    axd.annotate(f"{v:,}", (xi, v), ha="center", va="bottom", fontsize=6.8,
                 xytext=(0, 1), textcoords="offset points")
for xi, v in zip(xg2 + 0.17, real):
    axd.annotate(f"{v:,}", (xi, v), ha="center", va="bottom", fontsize=6.8,
                 xytext=(0, 1), textcoords="offset points")
axd.set_yscale("log")
axd.set_ylim(8, 20000)
axd.set_yticks([10, 100, 1000, 10000])
axd.set_yticklabels(["10", "100", "1k", "10k"])
axd.set_xticks(xg2)
axd.set_xticklabels(groups, fontsize=7.6)
axd.set_ylabel("missing (of 5,000)", fontsize=8.4)
axd.legend(loc="center", frameon=False, fontsize=7.2, handlelength=1.0)
panel_label(axd, "(d)", dx=-0.14, dy=1.10)

for ext in ("pdf", "png"):
    path = os.path.join(OUT, "results." + ext)
    fig.savefig(path, bbox_inches="tight", pad_inches=0.02, facecolor="white")
    print("wrote", path)
