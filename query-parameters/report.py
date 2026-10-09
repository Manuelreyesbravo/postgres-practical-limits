#!/usr/bin/env python3
"""Summarise a param_limits.sh run: medians, 95% CIs, the crossover, and a plot.

Usage: report.py WORKDIR

Reads WORKDIR/raw.csv and WORKDIR/wall.txt, writes WORKDIR/summary.csv and
WORKDIR/params.svg, and prints the summary. Standard library only.

The confidence interval of each median is the distribution-free one from order
statistics (binomial with p = 0.5): no assumption about the shape of the timings.
"""
import csv
import math
import os
import sys
from collections import defaultdict

PAIRS = {
    "lookup": ("in_list", "any_array"),
    "insert": ("insert_values", "insert_unnest"),
}
LABELS = {
    "in_list": "IN ($1..$N)",
    "any_array": "= ANY($1::bigint[])",
    "insert_values": "VALUES ($1,$2),.. (N params)",
    "insert_unnest": "unnest($1, $2) (2 params)",
}
COLORS = {"in_list": "#c0392b", "insert_values": "#c0392b",
          "any_array": "#2471a3", "insert_unnest": "#2471a3"}
METRICS = ("total_ms", "planning_ms", "exec_ms")


def median_ci(values):
    """Median and a >=95% distribution-free CI; (median, lo, hi, exact_ci)."""
    x = sorted(values)
    n = len(x)
    med = x[n // 2] if n % 2 else (x[n // 2 - 1] + x[n // 2]) / 2
    # largest k with P(Binom(n, 0.5) <= k - 1) <= 0.025: CI = [x(k), x(n-k+1)]
    k, cum = 0, 0.0
    for j in range(n):
        cum += math.comb(n, j) / 2 ** n
        if cum > 0.025:
            break
        k = j + 1
    if k == 0:
        return med, x[0], x[-1], False
    return med, x[k - 1], x[n - k], True


def fmt(v):
    return f"{v:.3f}" if v < 10 else f"{v:.1f}"


def crossover(stats, capped, free, ns):
    """Smallest N from which `free` stays faster: by median, and with disjoint CIs."""
    by_median = by_ci = None
    for n in reversed(ns):
        c, f = stats.get((capped, n)), stats.get((free, n))
        if not c or not f:
            break
        if c["total_ms"][0] > f["total_ms"][0]:
            by_median = n
        else:
            break
    for n in reversed(ns):
        c, f = stats.get((capped, n)), stats.get((free, n))
        if not c or not f:
            break
        if c["total_ms"][1] > f["total_ms"][2]:
            by_ci = n
        else:
            break
    return by_median, by_ci


def svg(stats, ns_by_kind, cross, title):
    w, ph, top, gap = 760, 300, 46, 70
    ml, mr, mb = 70, 230, 40
    h = top + 2 * ph + gap + mb
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" '
           f'viewBox="0 0 {w} {h}" font-family="sans-serif" font-size="12">',
           f'<rect width="{w}" height="{h}" fill="#ffffff"/>',
           f'<text x="{ml}" y="24" font-size="15" font-weight="bold">{title}</text>']
    for i, kind in enumerate(("lookup", "insert")):
        ns = ns_by_kind.get(kind, [])
        if not ns:
            continue
        y0 = top + i * (ph + gap)
        pw = w - ml - mr
        vals = [v for a in PAIRS[kind] for n in ns if (a, n) in stats
                for v in stats[(a, n)]["total_ms"][1:3]]
        lo = 10 ** math.floor(math.log10(max(min(vals), 1e-3)))
        hi = 10 ** math.ceil(math.log10(max(vals)))
        if hi <= lo:
            hi = lo * 10
        xlo, xhi = math.log10(min(ns)), math.log10(max(ns))
        xspan = (xhi - xlo) or 1.0

        def px(n):
            return ml + (math.log10(n) - xlo) / xspan * pw

        def py(v):
            v = max(v, lo)
            return y0 + ph - (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * ph

        out.append(f'<rect x="{ml}" y="{y0}" width="{pw}" height="{ph}" fill="none" stroke="#999"/>')
        e = math.log10(lo)
        while e <= math.log10(hi) + 1e-9:
            yy = py(10 ** e)
            out.append(f'<line x1="{ml}" x2="{ml + pw}" y1="{yy:.1f}" y2="{yy:.1f}" stroke="#eee"/>')
            out.append(f'<text x="{ml - 6}" y="{yy + 4:.1f}" text-anchor="end">{10 ** e:g}</text>')
            e += 1
        for n in ns:
            xx = px(n)
            out.append(f'<line x1="{xx:.1f}" x2="{xx:.1f}" y1="{y0}" y2="{y0 + ph}" stroke="#f3f3f3"/>')
            out.append(f'<text x="{xx:.1f}" y="{y0 + ph + 16}" text-anchor="middle">{n:,}</text>')
        out.append(f'<text x="{ml + pw / 2}" y="{y0 + ph + 32}" text-anchor="middle">'
                   f'N (logical size: keys looked up, or parameters of the VALUES form)</text>')
        out.append(f'<text transform="translate(16,{y0 + ph / 2}) rotate(-90)" text-anchor="middle">'
                   f'{kind}: total ms (median, 95% CI)</text>')
        med, ci = cross.get(kind, (None, None))
        if ci:
            xx = px(ci)
            out.append(f'<line x1="{xx:.1f}" x2="{xx:.1f}" y1="{y0}" y2="{y0 + ph}" '
                       f'stroke="#555" stroke-dasharray="4,3"/>')
            out.append(f'<text x="{xx + 4:.1f}" y="{y0 + 14}" fill="#555">crossover N={ci:,}</text>')
        for j, a in enumerate(PAIRS[kind]):
            pts = [(n, stats[(a, n)]["total_ms"]) for n in ns if (a, n) in stats]
            col = COLORS[a]
            path = " ".join(f"{'M' if k == 0 else 'L'}{px(n):.1f},{py(m[0]):.1f}"
                            for k, (n, m) in enumerate(pts))
            out.append(f'<path d="{path}" fill="none" stroke="{col}" stroke-width="2"/>')
            for n, m in pts:
                xx = px(n)
                out.append(f'<line x1="{xx:.1f}" x2="{xx:.1f}" y1="{py(m[1]):.1f}" '
                           f'y2="{py(m[2]):.1f}" stroke="{col}" stroke-width="1.5"/>')
                out.append(f'<circle cx="{xx:.1f}" cy="{py(m[0]):.1f}" r="3" fill="{col}"/>')
            ly = y0 + 20 + j * 20
            out.append(f'<line x1="{ml + pw + 12}" x2="{ml + pw + 32}" y1="{ly - 4}" y2="{ly - 4}" '
                       f'stroke="{col}" stroke-width="2"/>')
            out.append(f'<text x="{ml + pw + 38}" y="{ly}">{LABELS[a]}</text>')
    out.append("</svg>")
    return "\n".join(out) + "\n"


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: report.py WORKDIR")
    wd = sys.argv[1]
    samples = defaultdict(lambda: defaultdict(list))
    bind = {}
    kinds = defaultdict(set)
    with open(os.path.join(wd, "raw.csv"), newline="") as f:
        for row in csv.DictReader(f):
            key = (row["approach"], int(row["n"]))
            kinds[row["kind"]].add(int(row["n"]))
            bind[key] = int(row["bind_params"])
            for m in METRICS:
                samples[key][m].append(float(row[m]))
    if not samples:
        sys.exit("raw.csv has no samples")

    stats = {k: {m: median_ci(v) for m, v in ms.items()} for k, ms in samples.items()}
    ns_by_kind = {k: sorted(v) for k, v in kinds.items()}

    with open(os.path.join(wd, "summary.csv"), "w", newline="") as f:
        wr = csv.writer(f, lineterminator="\n")
        wr.writerow(["kind", "approach", "n", "bind_params", "rounds", "metric",
                     "median_ms", "ci95_low_ms", "ci95_high_ms"])
        for kind, (a1, a2) in PAIRS.items():
            for n in ns_by_kind.get(kind, []):
                for a in (a1, a2):
                    if (a, n) not in stats:
                        continue
                    for m in METRICS:
                        med, lo, hi, _ = stats[(a, n)][m]
                        wr.writerow([kind, a, n, bind[(a, n)], len(samples[(a, n)][m]), m,
                                     f"{med:.3f}", f"{lo:.3f}", f"{hi:.3f}"])

    rounds = min(len(v["total_ms"]) for v in samples.values())
    exact = all(s["total_ms"][3] for s in stats.values())
    print(f"(A) Cost curve: median ms over {rounds} rounds, "
          f"[95% CI{'' if exact else '; too few rounds for a 95% CI, min..max shown'}]")
    cross = {}
    for kind, (capped, free) in PAIRS.items():
        ns = ns_by_kind.get(kind, [])
        if not ns:
            continue
        print(f"\n  {kind}: {LABELS[capped]}  vs  {LABELS[free]}")
        print(f"  {'N':>6} | {'capped total':>26} {'plan':>8} {'exec':>8} |"
              f" {'one-param total':>26} {'plan':>8} {'exec':>8} | ratio")
        for n in ns:
            c, f_ = stats.get((capped, n)), stats.get((free, n))
            if not c or not f_:
                continue
            ct, ft = c["total_ms"], f_["total_ms"]
            print(f"  {n:>6} | {fmt(ct[0]):>9} [{fmt(ct[1])}..{fmt(ct[2])}]".ljust(38)
                  + f"{fmt(c['planning_ms'][0]):>8} {fmt(c['exec_ms'][0]):>8} |"
                  + f" {fmt(ft[0]):>9} [{fmt(ft[1])}..{fmt(ft[2])}]".ljust(27)
                  + f"{fmt(f_['planning_ms'][0]):>8} {fmt(f_['exec_ms'][0]):>8} |"
                  + f" {ct[0] / ft[0]:.2f}x")
        cross[kind] = crossover(stats, capped, free, ns)

    print("\n(B) The hard wall")
    wall_path = os.path.join(wd, "wall.txt")
    if os.path.exists(wall_path):
        with open(wall_path) as f:
            for line in f:
                print("  " + line.rstrip())

    print("\n(C) Practical crossover: smallest N from which the one-parameter form")
    print("    stays faster at every larger N tested")
    for kind, (by_med, by_ci) in cross.items():
        ci_txt = f"N={by_ci:,}" if by_ci else "not separated in the tested range"
        med_txt = f"N={by_med:,}" if by_med else "never in the tested range"
        print(f"  {kind:>6}: by median {med_txt}; with non-overlapping 95% CIs {ci_txt}")

    title = "Bind parameters: N parameters vs one array parameter (PostgreSQL)"
    env = os.path.join(wd, "environment.txt")
    if os.path.exists(env):
        with open(env) as f:
            for line in f:
                if line.startswith("server_version "):
                    title = f"N Bind parameters vs one array parameter, PostgreSQL {line.split()[1]}"
    with open(os.path.join(wd, "params.svg"), "w") as f:
        f.write(svg(stats, ns_by_kind, {k: v for k, v in cross.items()}, title))


if __name__ == "__main__":
    main()
