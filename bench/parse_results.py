#!/usr/bin/env python3
"""
parse_results.py  —  Post-process RESULT CSV files from bench_scaling.sh.

Produces:
  1. A summary table printed to stdout.
  2. Speedup / efficiency plots saved as PNG files alongside the CSV.

Usage:
  python3 bench/parse_results.py bench/results_static.csv [bench/results_dynamic.csv ...]

Dependencies:
  pip install pandas matplotlib

The CSV format (written by bench_scaling.sh) has columns:
  scenario, algo, threads, n, m, Delta, batch_size,
  runs, total_time_s, avg_time_s, avg_rounds, avg_colors

bench_snap_data_sweep.sh adds an optional leading column:
  graph_file  —  basename of the SNAP file (combined results_snap_all.csv)
"""

import sys
import os
import pandas as pd

try:
    import matplotlib
    matplotlib.use("Agg")   # non-interactive backend; safe on cluster
    import matplotlib.pyplot as plt
    HAS_MPL = True
except ImportError:
    HAS_MPL = False
    print("WARNING: matplotlib not found; skipping plots.")


# ================================================================
# Helpers
# ================================================================

def load_csv(path: str) -> pd.DataFrame:
    df = pd.read_csv(path)
    # Coerce numeric columns.
    numeric_cols = [
        "threads", "n", "m", "Delta", "batch_size",
        "runs", "total_time_s", "avg_time_s", "avg_rounds", "avg_colors",
    ]
    for col in numeric_cols:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")
    return df


def speedup_table(df: pd.DataFrame, group_cols: list, time_col: str = "avg_time_s"):
    """
    For each unique combination of group_cols (excluding 'threads'),
    compute speedup = T_1 / T_p and efficiency = speedup / p.
    """
    key_cols = [c for c in group_cols if c != "threads"]
    rows = []
    for key_vals, grp in df.groupby(key_cols):
        grp = grp.sort_values("threads")
        t1 = grp.loc[grp["threads"] == grp["threads"].min(), time_col]
        if t1.empty:
            continue
        t1_val = float(t1.iloc[0])
        for _, row in grp.iterrows():
            p = row["threads"]
            tp = row[time_col]
            rows.append({
                **{k: v for k, v in zip(key_cols, key_vals if isinstance(key_vals, tuple) else (key_vals,))},
                "threads": int(p),
                "avg_time_s": tp,
                "speedup": t1_val / tp if tp > 0 else float("nan"),
                "efficiency": (t1_val / tp) / p if tp > 0 else float("nan"),
            })
    return pd.DataFrame(rows)


# ================================================================
# Plot helpers
# ================================================================

def plot_speedup(sp: pd.DataFrame, label_col: str, out_path: str, title: str):
    if not HAS_MPL:
        return
    fig, axes = plt.subplots(1, 2, figsize=(12, 5))
    for key, grp in sp.groupby(label_col):
        grp = grp.sort_values("threads")
        axes[0].plot(grp["threads"], grp["speedup"], marker="o", label=str(key))
        axes[1].plot(grp["threads"], grp["efficiency"], marker="o", label=str(key))

    # Ideal reference lines.
    max_p = sp["threads"].max()
    xs = sorted(sp["threads"].unique())
    axes[0].plot(xs, xs, "k--", alpha=0.4, label="ideal")
    axes[1].axhline(1.0, color="k", linestyle="--", alpha=0.4, label="ideal")

    axes[0].set_xlabel("Threads"); axes[0].set_ylabel("Speedup"); axes[0].set_title("Speedup")
    axes[1].set_xlabel("Threads"); axes[1].set_ylabel("Efficiency"); axes[1].set_title("Efficiency")
    for ax in axes:
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)

    fig.suptitle(title)
    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    plt.close()
    print(f"  Saved: {out_path}")


def plot_batch_size(df: pd.DataFrame, out_path: str, title: str):
    if not HAS_MPL:
        return
    fig, axes = plt.subplots(1, 2, figsize=(12, 5))
    for algo, grp in df.groupby("algo") if "algo" in df.columns else [(None, df)]:
        grp = grp.sort_values("batch_size")
        label = str(algo) if algo else ""
        axes[0].plot(grp["batch_size"], grp["avg_time_s"], marker="o", label=label)
        if "avg_rounds" in grp.columns:
            axes[1].plot(grp["batch_size"], grp["avg_rounds"], marker="o", label=label)

    axes[0].set_xscale("log"); axes[0].set_xlabel("Batch size")
    axes[0].set_ylabel("Avg time (s)"); axes[0].set_title("Time vs batch size")
    axes[1].set_xscale("log"); axes[1].set_xlabel("Batch size")
    axes[1].set_ylabel("Max rounds"); axes[1].set_title("Rounds vs batch size")
    for ax in axes:
        ax.legend(fontsize=8); ax.grid(True, alpha=0.3)

    fig.suptitle(title)
    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    plt.close()
    print(f"  Saved: {out_path}")


def plot_rounds_hist(df: pd.DataFrame, out_path: str, title: str):
    """Bar chart: avg_rounds per algo/scenario."""
    if not HAS_MPL or "avg_rounds" not in df.columns:
        return
    sub = df[df["avg_rounds"].notna() & (df["avg_rounds"] >= 0)]
    if sub.empty:
        return
    fig, ax = plt.subplots(figsize=(8, 4))
    label_col = "algo" if "algo" in sub.columns else "scenario"
    for key, grp in sub.groupby(label_col):
        ax.plot(grp["threads"], grp["avg_rounds"], marker="o", label=str(key))
    ax.set_xlabel("Threads"); ax.set_ylabel("RecolorBatch rounds (avg)")
    ax.set_title(title)
    ax.legend(fontsize=8); ax.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    plt.close()
    print(f"  Saved: {out_path}")


# ================================================================
# Main
# ================================================================

def process_file(path: str):
    print(f"\n{'='*60}")
    print(f"File: {path}")
    print('='*60)
    stem = os.path.splitext(path)[0]

    df = load_csv(path)
    if df.empty:
        print("  (empty file)")
        return

    print(f"\nRows: {len(df)}")
    print(df.to_string(index=False))

    # Identify available scenarios.
    has_scenario = "scenario" in df.columns and df["scenario"].notna().any()
    has_algo     = "algo"     in df.columns and df["algo"].notna().any()

    # ── Strong-scaling / thread-count plots ──────────────────────
    # These require multiple threads values for the same (scenario, algo, n).
    if "threads" in df.columns and df["threads"].nunique() > 1:
        group_cols = ["threads"]
        label_col  = None
        has_graph = "graph_file" in df.columns and df["graph_file"].notna().any()
        multi_graph = has_graph and df["graph_file"].nunique() > 1
        if has_graph:
            group_cols.insert(0, "graph_file")
        if has_scenario:
            group_cols.insert(0, "scenario")
            if label_col is None and not multi_graph:
                label_col = "scenario"
        if has_algo:
            group_cols.insert(0, "algo")
            if label_col is None and not multi_graph:
                label_col = "algo"
        group_cols.append("n")

        sp = speedup_table(df, group_cols)
        if not sp.empty:
            print("\n-- Speedup table --")
            print(sp.to_string(index=False))
            if HAS_MPL and (label_col or multi_graph):
                leg_col = label_col or "algo"
                if multi_graph and has_algo and "graph_file" in sp.columns:
                    sp = sp.copy()
                    sp["_legend"] = (
                        sp["graph_file"].astype(str) + " · " + sp["algo"].astype(str)
                    )
                    leg_col = "_legend"
                plot_speedup(sp, leg_col,
                             stem + "_speedup.png",
                             f"Speedup — {os.path.basename(path)}")

    # ── Batch-size plots ─────────────────────────────────────────
    if "batch_size" in df.columns and df["batch_size"].notna().any():
        for t_val, grp in df.groupby("threads") if "threads" in df.columns else [(None, df)]:
            sub = grp[grp["scenario"].str.contains("batch_size", na=False)] \
                  if has_scenario else grp
            if sub.empty:
                continue
            tag = f"_t{int(t_val)}" if t_val is not None else ""
            plot_batch_size(sub,
                            stem + f"{tag}_batch_size.png",
                            f"Batch-size scaling — threads={t_val}")

    # ── Rounds plot ──────────────────────────────────────────────
    if "avg_rounds" in df.columns:
        plot_rounds_hist(df, stem + "_rounds.png",
                         f"RecolorBatch rounds — {os.path.basename(path)}")

    print()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(0)

    for csv_path in sys.argv[1:]:
        if not os.path.isfile(csv_path):
            print(f"WARNING: file not found: {csv_path}")
            continue
        process_file(csv_path)
