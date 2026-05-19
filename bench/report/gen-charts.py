#!/usr/bin/env python3
"""results.csv -> PNG charts for the TidesDB-on-MySQL vs -on-MariaDB
comparison. Runs inside the pinned Dockerfile.charts container.

Usage (via wrapper): python gen-charts.py /data/results.csv  ->  /data/*.png

Charts produced (median over reps; NA dropped):
  1. tps_threads_<workload>.png  -- TPS vs threads, A vs B, panel per table_size
  2. contention_<workload>.png   -- TPS vs table_size (the OCC story), A vs B,
                                    one line per thread level
  3. p95_threads_<workload>.png  -- p95 latency vs threads, A vs B per table_size
Each title states it's median-over-reps and the fixed pins.
"""
import sys, os
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

CSV = sys.argv[1] if len(sys.argv) > 1 else "/data/results.csv"
OUT = os.path.dirname(CSV) or "."

df = pd.read_csv(CSV)
for col in ("tps", "qps", "p95_ms", "cpu_sec"):
    df[col] = pd.to_numeric(df[col], errors="coerce")   # "NA" -> NaN

# median over reps per (sut, workload, table_size, threads)
g = (df.groupby(["sut", "workload", "table_size", "threads"], as_index=False)
       .agg(tps=("tps", "median"),
            p95=("p95_ms", "median"),
            cpu=("cpu_sec", "median")))

workloads = sorted(g["workload"].unique())
sizes     = sorted(g["table_size"].unique())
threads   = sorted(g["threads"].unique())
SUTS = [("mysql", "A: MySQL+tidesdb-mysql"), ("mariadb", "B: MariaDB+tidesql")]
SUB  = "median over reps · TidesDB v9.2.0 both · MySQL 9.7 vs MariaDB 12.3.1"


def sel(sut, wl, tz=None, th=None):
    q = (g.sut == sut) & (g.workload == wl)
    if tz is not None: q &= g.table_size == tz
    if th is not None: q &= g.threads == th
    return g[q]


def save(fig, name):
    p = os.path.join(OUT, name)
    fig.tight_layout()
    fig.savefig(p, dpi=120)
    plt.close(fig)
    print("[charts] ->", p)


for wl in workloads:
    # 1. TPS vs threads, panel per table_size
    fig, axes = plt.subplots(1, len(sizes), figsize=(5 * len(sizes), 4),
                             squeeze=False)
    for ax, tz in zip(axes[0], sizes):
        for sut, lbl in SUTS:
            d = sel(sut, wl, tz=tz).sort_values("threads")
            if not d.empty:
                ax.plot(d["threads"], d["tps"], marker="o", label=lbl)
        ax.set_title(f"table_size={tz}")
        ax.set_xlabel("threads"); ax.set_ylabel("TPS (median)")
        ax.set_xticks(threads); ax.grid(True, alpha=.3); ax.legend(fontsize=8)
    fig.suptitle(f"{wl} — TPS vs threads\n{SUB}", fontsize=10)
    save(fig, f"tps_threads_{wl}.png")

    # 2. contention curve: TPS vs table_size, line per thread level
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for sut, lbl in SUTS:
        for th in threads:
            d = sel(sut, wl, th=th).sort_values("table_size")
            if not d.empty:
                ax.plot(d["table_size"], d["tps"], marker="o",
                        label=f"{lbl} t={th}")
    ax.set_xscale("log")
    ax.set_xlabel("table_size (log; smaller = higher write contention)")
    ax.set_ylabel("TPS (median)")
    ax.set_title(f"{wl} — contention sweep\n{SUB}", fontsize=10)
    ax.grid(True, alpha=.3); ax.legend(fontsize=7)
    save(fig, f"contention_{wl}.png")

    # 3. p95 vs threads, panel per table_size
    fig, axes = plt.subplots(1, len(sizes), figsize=(5 * len(sizes), 4),
                             squeeze=False)
    for ax, tz in zip(axes[0], sizes):
        for sut, lbl in SUTS:
            d = sel(sut, wl, tz=tz).sort_values("threads")
            if not d.empty:
                ax.plot(d["threads"], d["p95"], marker="s", label=lbl)
        ax.set_title(f"table_size={tz}")
        ax.set_xlabel("threads"); ax.set_ylabel("p95 latency ms (median)")
        ax.set_xticks(threads); ax.grid(True, alpha=.3); ax.legend(fontsize=8)
    fig.suptitle(f"{wl} — p95 latency vs threads\n{SUB}", fontsize=10)
    save(fig, f"p95_threads_{wl}.png")

print("[charts] done")
