#!/usr/bin/env python3
"""Emit the four LaTeX tables used by docs/202 from the raw run CSVs."""
import csv, os, re, glob, statistics as st

ROOT = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(os.path.dirname(ROOT))
RES = os.path.join(ROOT, "results")
OUT = os.path.join(RES, "analysis", "paper_snippets.tex")


def rows(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def mean(rs, k):
    return st.mean(float(r[k]) for r in rs)


def ablation_table():
    rs = rows(os.path.join(RES, "ablation.csv"))
    keys = sorted({(r["arm"], r["aggr"], r["drop"]) for r in rs})
    out = [
        r"% A-E ablation. 1M SIFT, Uniform 1k roles, head-10, K=10, 2000 changes, 4R+1W, 3 seeds.",
        r"\begin{table*}[t]\centering\footnotesize",
        r"\caption{One controlled kernel, five mechanisms. Only the validity rule changes; graph, queries, stream "
        r"and scoring are shared. \emph{stream}: revoke-only, 50/50, grant-only; \emph{hole}: every "
        r"fifth grant is dropped from the log and the watermark freezes. \emph{auth} counts missing "
        r"top-10 results whose document had no candidate path; leaks are zero in every cell. Update is "
        r"the total apply time of 2{,}000 changes, including merges for E (every 100 changes per role); "
        r"seed-to-seed sd of p50/p95 is at most 0.004\,ms.}",
        r"\label{tab:ae}",
        r"\begin{tabular}{@{}llrrrrrr@{}}\toprule",
        r"arm & stream & leased & fallback & p50 & p95 & auth & update (ms) \\",
        r"\midrule",
    ]
    for a, ag, dr in keys:
        g = [r for r in rs if r["arm"] == a and r["aggr"] == ag and r["drop"] == dr]
        stream = {"0": "revoke", "50": "50/50", "100": "grant"}[ag] + (" + hole" if dr != "0" else "")
        out.append(
            f"{a} & {stream} & {100*mean(g,'leased_rate'):.1f}\\% & {100*mean(g,'fallback_rate'):.1f}\\% & "
            f"{mean(g,'p50'):.3f} & {mean(g,'p95'):.3f} & {mean(g,'auth'):.0f} & {mean(g,'update_total_ms'):.1f} \\\\")
    out += [r"\bottomrule\end{tabular}\end{table*}", ""]
    return out


def five_arm_table():
    arms = {}
    for f in sorted(glob.glob(os.path.join(PROJ, "experiments", "results", "arms_e1_20261107", "uniform_s*.txt"))):
        for line in open(f):
            m = re.match(r"(\w+) reads=(\d+) p50=([\d.]+) p95=([\d.]+) p99=([\d.]+) leaks=(\d+) missing=(\d+)", line)
            if m:
                arms.setdefault(m.group(1), []).append(
                    (float(m.group(3)), float(m.group(4)), int(m.group(7))))
    pin = []
    for f in sorted(glob.glob(os.path.join(PROJ, "experiments", "results", "arms_e1_v2_20261108", "uniform_s*.txt"))):
        for line in open(f):
            m = re.match(r"pin_no_delta reads=(\d+) p50=([\d.]+) p95=([\d.]+) p99=([\d.]+) leaks=(\d+) missing=(\d+)", line)
            if m:
                pin.append((float(m.group(2)), float(m.group(3)), int(m.group(6))))
    out = [
        r"\begin{table}[t]\centering\footnotesize",
        r"\caption{Five arms, 1M Uniform, head-10, $K{=}10$, 1{,}000 changes, 4R+1W, three seeds "
        r"(mean$\pm$sd). The first four are one runtime (\texttt{arms\_e1\_20261107/}); the "
        r"pin-without-log arm is from the 11-08 rerun (\texttt{arms\_e1\_v2\_20261108/}) and is not "
        r"on the same seeds.}",
        r"\label{tab:arms}",
        r"\begin{tabular}{@{}lrrr@{}}\toprule",
        r"arm & p50 (ms) & p95 (ms) & missing \\\midrule",
    ]
    for a in ["exact", "static", "unversioned", "lease"]:
        v = arms[a]
        out.append(f"{a} & {st.mean(x[0] for x in v):.4f}$\\pm${st.stdev(x[0] for x in v):.4f} & "
                   f"{st.mean(x[1] for x in v):.4f} & {st.mean(x[2] for x in v):.0f} \\\\")
    out.append(f"pin, no log$^\\dagger$ & {st.mean(x[0] for x in pin):.4f}$\\pm${st.stdev(x[0] for x in pin):.4f} & "
               f"{st.mean(x[1] for x in pin):.4f} & {st.mean(x[2] for x in pin):.0f} \\\\")
    out += [r"\bottomrule\end{tabular}\end{table}", ""]
    return out


def merge_table():
    lr = rows(os.path.join(RES, "longrun.csv"))
    l2 = rows(os.path.join(RES, "long2k.csv"))

    def g(rs, arm, mg):
        return [r for r in rs if r["arm"] == arm and r["merge"] == mg]

    out = [
        r"\begin{table}[t]\centering\footnotesize",
        r"\caption{Merge: 2k vs 10k changes (head-10, 50/50 stream, 3 seeds). The 2k E rows are "
        r"measured with the same cadence (\texttt{long2k}). RSS is an upper bound: the harness does "
        r"not reclaim old leases, so E includes stale graphs.}",
        r"\label{tab:merge}",
        r"\begin{tabular}{@{}lrrrrr@{}}\toprule",
        r"policy & merges & update & p95 @2k & p95 @10k & RSS \\\midrule",
    ]
    out.append(
        f"D (no merge) & 0 & {st.mean(float(r['update_total_ms'])/1000 for r in g(lr,'D','0')):.2f}\\,s & "
        f"0.116 & {st.mean(float(r['p95']) for r in g(lr,'D','0')):.3f} & "
        f"{st.mean(float(r['rss_mb']) for r in g(lr,'D','0')):.0f}\\,MB \\\\")
    for mg in ["500", "200"]:
        out.append(
            f"E (m{{=}}{mg}) & {st.mean(float(r['merges']) for r in g(lr,'E',mg)):.0f} & "
            f"{st.mean(float(r['update_total_ms'])/1000 for r in g(lr,'E',mg)):.2f}\\,s & "
            f"{st.mean(float(r['p95']) for r in g(l2,'E',mg)):.3f} & "
            f"{st.mean(float(r['p95']) for r in g(lr,'E',mg)):.3f} & "
            f"{st.mean(float(r['rss_mb']) for r in g(lr,'E',mg)):.0f}\\,MB \\\\")
    out += [r"\bottomrule\end{tabular}\end{table}", ""]
    return out


def quantile_table():
    rs = rows(os.path.join(RES, "matrix.csv"))
    out = [
        r"\begin{table}[t]\centering\footnotesize",
        r"\caption{The p95 cliff. Same $K{=}100$ build, three query mixes whose top-100 roles carry "
        r"90\%, 95\% and 99\% of the queries; p95 stays in the exact population until the fallback "
        r"share drops below 5\%.}",
        r"\label{tab:quantile}",
        r"\begin{tabular}{@{}lrrrr@{}}\toprule",
        r"top-100 mass & $p_L$ & p50 & p95 & $T_F$ \\\midrule",
    ]
    for conc, label in [("conc90", "90\\%"), ("conc96", "95\\%"), ("conc99", "99\\%")]:
        g = [r for r in rs if r["conc"] == conc]
        out.append(f"{label} & {mean(g,'leased_rate'):.3f} & {mean(g,'p50'):.3f} & "
                   f"{mean(g,'p95'):.3f} & {mean(g,'exact_p50'):.3f} \\\\")
    out += [r"\bottomrule\end{tabular}\end{table}", ""]
    return out


if __name__ == "__main__":
    lines = []
    for fn in [ablation_table, five_arm_table, merge_table, quantile_table]:
        lines += fn()
    open(OUT, "w").write("\n".join(lines) + "\n")
    print("wrote", OUT, len(lines), "lines")
