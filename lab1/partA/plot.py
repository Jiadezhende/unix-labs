#!/usr/bin/env python3
"""Generate Part A performance figures from results.csv.

Usage:
    python3 plot.py            # reads results.csv, writes PNGs into fig/
"""
import csv
import os
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, "results.csv")
FIGDIR = os.path.join(HERE, "fig")
os.makedirs(FIGDIR, exist_ok=True)

plt.rcParams.update({
    "figure.dpi": 150,
    "font.size": 11,
    "axes.grid": True,
    "grid.alpha": 0.3,
})


def load():
    data = defaultdict(list)
    with open(CSV, newline="") as f:
        for row in csv.DictReader(f):
            data[row["test"]].append({
                "buffsize": int(row["buffsize"]),
                "real": float(row["real"]),
                "user": float(row["user"]),
                "sys": float(row["sys"]),
                "bytes": int(row["bytes"]),
                "thr": float(row["throughput_MBps"]),
            })
    for k in data:
        data[k].sort(key=lambda r: r["buffsize"])
    return data


def fig1_read_throughput_time(d):
    rows = d["read"]
    bs = [r["buffsize"] for r in rows]
    thr = [r["thr"] for r in rows]
    real = [r["real"] for r in rows]

    fig, ax1 = plt.subplots(figsize=(7, 4.2))
    c1 = "#1f77b4"
    ax1.set_xscale("log", base=2)
    ax1.plot(bs, thr, "o-", color=c1, label="Throughput")
    ax1.set_xlabel("BUFFSIZE (bytes, log2)")
    ax1.set_ylabel("Throughput (MB/s)", color=c1)
    ax1.tick_params(axis="y", labelcolor=c1)

    ax2 = ax1.twinx()
    c2 = "#d62728"
    ax2.set_yscale("log")
    ax2.plot(bs, real, "s--", color=c2, label="real time")
    ax2.set_ylabel("real time (s, log)", color=c2)
    ax2.tick_params(axis="y", labelcolor=c2)
    ax2.grid(False)

    ax1.set_title("Fig.1  read(): throughput & real time vs BUFFSIZE")
    fig.tight_layout()
    out = os.path.join(FIGDIR, "fig1_read_throughput_time.png")
    fig.savefig(out)
    plt.close(fig)
    return out


def fig2_read_user_sys(d):
    rows = d["read"]
    labels = [str(r["buffsize"]) for r in rows]
    user = [r["user"] for r in rows]
    sys = [r["sys"] for r in rows]
    x = range(len(rows))

    fig, ax = plt.subplots(figsize=(7.5, 4.2))
    ax.bar(x, user, label="user (s)", color="#2ca02c")
    ax.bar(x, sys, bottom=user, label="sys (s)", color="#ff7f0e")
    ax.set_yscale("log")
    ax.set_xticks(list(x))
    ax.set_xticklabels(labels, rotation=60, ha="right", fontsize=8)
    ax.set_xlabel("BUFFSIZE (bytes)")
    ax.set_ylabel("CPU time (s, log)")
    ax.set_title("Fig.2  read(): user vs sys CPU time (stacked) by BUFFSIZE")
    ax.legend()
    ax.grid(axis="x", visible=False)
    fig.tight_layout()
    out = os.path.join(FIGDIR, "fig2_read_user_sys.png")
    fig.savefig(out)
    plt.close(fig)
    return out


def fig3_read_fread_myfread(d):
    fig, ax = plt.subplots(figsize=(7, 4.2))
    styles = {
        "read": ("o-", "#1f77b4"),
        "fread": ("s-", "#d62728"),
        "my_fread": ("^-", "#2ca02c"),
    }
    for name, (style, color) in styles.items():
        rows = d[name]
        bs = [r["buffsize"] for r in rows]
        thr = [r["thr"] for r in rows]
        ax.plot(bs, thr, style, color=color, label=name + "()")
    ax.set_xscale("log", base=2)
    ax.set_xlabel("BUFFSIZE / request size (bytes, log2)")
    ax.set_ylabel("Throughput (MB/s)")
    ax.set_title("Fig.3  read() vs fread() vs my_fread() throughput")
    ax.legend()
    fig.tight_layout()
    out = os.path.join(FIGDIR, "fig3_read_fread_myfread.png")
    fig.savefig(out)
    plt.close(fig)
    return out


def fig4_write_osync(d):
    fig, ax = plt.subplots(figsize=(7, 4.2))
    no = d["write_nosync"]
    yes = d["write_osync"]
    ax.plot([r["buffsize"] for r in no], [r["thr"] for r in no],
            "o-", color="#1f77b4", label="write (no O_SYNC)")
    ax.plot([r["buffsize"] for r in yes], [r["thr"] for r in yes],
            "s-", color="#d62728", label="write (O_SYNC)")
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xlabel("BUFFSIZE (bytes, log2)")
    ax.set_ylabel("Throughput (MB/s, log)")
    ax.set_title("Fig.4  write(): with vs without O_SYNC throughput")
    ax.legend()
    fig.tight_layout()
    out = os.path.join(FIGDIR, "fig4_write_osync.png")
    fig.savefig(out)
    plt.close(fig)
    return out


def main():
    d = load()
    for fn in (fig1_read_throughput_time, fig2_read_user_sys,
               fig3_read_fread_myfread, fig4_write_osync):
        print("wrote", fn(d))


if __name__ == "__main__":
    main()
