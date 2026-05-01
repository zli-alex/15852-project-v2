#!/usr/bin/env bash
# ================================================================
# bench_snap_data_sweep.sh
#
# For every SNAP-style edge list in data/, runs a full thread sweep
# with bench_static (gk24 + greedy), so you can compare performance
# across graph size (n, m, Delta in RESULT lines) and thread count.
#
# Prerequisites:
#   make bench_static
#   Place graphs under data/*.txt (plain SNAP edge lists, gunzipped).
#
# Usage (from repository root):
#   bash bench/bench_snap_data_sweep.sh [data_dir] [runs_per_thread]
#
# Environment (optional):
#   BENCH_THREADS   — space-separated thread counts (default: 1 2 4 8 16 32 48 64)
#   BENCH_BINARY    — path to bench_static (default: ./bench_static)
#   NUMA_NODE       — if set and numactl exists, pin to this NUMA node
#
# Outputs:
#   bench/results_snap_<GraphName>.csv   — one CSV per graph (same schema as bench_scaling.sh)
#   bench/results_snap_all.csv           — all graphs concatenated with leading graph_file column
#
# Example:
#   bash bench/bench_snap_data_sweep.sh data 3
#   BENCH_THREADS="1 2 4 8" NUMA_NODE=0 bash bench/bench_snap_data_sweep.sh ./my_snap_folder 5
# ================================================================

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DATA_DIR="${1:-data}"
RUNS="${2:-3}"
BINARY="${BENCH_BINARY:-./bench_static}"
SCALE_SCRIPT="$ROOT/bench/bench_scaling.sh"

if [[ ! -x "$BINARY" ]]; then
  echo "ERROR: '$BINARY' not found or not executable." >&2
  echo "  Run:  make bench_static" >&2
  exit 1
fi

if [[ ! -d "$DATA_DIR" ]]; then
  echo "ERROR: data directory '$DATA_DIR' does not exist." >&2
  exit 1
fi

shopt -s nullglob
mapfile -t GRAPHS < <(find "$DATA_DIR" -maxdepth 1 -type f \( -name '*.txt' -o -name '*.edges' \) \
  ! -iname 'readme*' ! -iname '*.md' | LC_ALL=C sort)

if [[ ${#GRAPHS[@]} -eq 0 ]]; then
  echo "No graph files found in '$DATA_DIR' (*.txt or *.edges)." >&2
  echo "Download SNAP edge lists into that folder (see project docs / SNAP site)." >&2
  exit 1
fi

COMBINED="bench/results_snap_all.csv"
mkdir -p bench
echo "graph_file,scenario,algo,threads,n,m,Delta,batch_size,runs,total_time_s,avg_time_s,avg_rounds,avg_colors" \
  > "$COMBINED"

echo "# bench_snap_data_sweep"
echo "#   data_dir=$DATA_DIR  runs=$RUNS  binary=$BINARY"
echo "#   graphs=${#GRAPHS[@]}"
echo "#   threads=${BENCH_THREADS:-1 2 4 8 16 32 48 64}"
echo ""

for graph_path in "${GRAPHS[@]}"; do
  base="$(basename "$graph_path")"
  stem="${base%.*}"
  out="bench/results_snap_${stem}.csv"

  echo "========================================"
  echo "# Graph: $graph_path  ->  $out"
  echo "========================================"

  bash "$SCALE_SCRIPT" "$BINARY" "$graph_path" "$RUNS" --output "$out"

  # Append body rows to combined file with graph_file prefix (skip header line 1).
  tail -n +2 "$out" | awk -v g="$base" '{ print g "," $0 }' >> "$COMBINED"

  echo ""
done

echo "Per-graph CSVs: bench/results_snap_<name>.csv"
echo "Combined CSV:  $COMBINED"
echo "Plots:         python3 bench/parse_results.py $COMBINED"
