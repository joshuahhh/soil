#!/usr/bin/env python3
"""Plot bench run CSVs (ms,rmse) as convergence curves on one graph."""

import argparse
import csv
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


def run_label(path):
    # bench/view_123.some.label.csv -> "some.label"
    parts = os.path.basename(path).split(".")
    return ".".join(parts[1:-1]) or parts[0]


def main():
    p = argparse.ArgumentParser()
    p.add_argument("csvs", nargs="+")
    p.add_argument("-o", "--out", default="bench.png")
    args = p.parse_args()

    plt.figure(figsize=(10, 6))
    for path in sorted(args.csvs):
        secs, rmse = [], []
        with open(path) as f:
            for row in csv.DictReader(f):
                secs.append(float(row["ms"]) / 1000.0)
                rmse.append(float(row["rmse"]))
        if not secs:
            continue
        plt.plot(secs, rmse, label=run_label(path), linewidth=1.5)

    plt.xlabel("seconds since start")
    plt.ylabel("RMSE vs fully loaded frame")
    plt.title("convergence to fully loaded view")
    plt.grid(alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(args.out, dpi=120)


if __name__ == "__main__":
    main()
