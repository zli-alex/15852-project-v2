#!/usr/bin/env bash
# ================================================================
# bench_scaling.sh  —  Thread-count sweep for scaling experiments.
#
# Runs a benchmark binary repeatedly with PARLAY_NUM_THREADS set to
# each value in THREADS, redirecting all RESULT lines to a CSV file.
#
# Usage:
#   bash bench/bench_scaling.sh <binary> [binary_args...] [--output results.csv]
#
# Examples:
#   # Strong scaling on RMAT graph (n=1M, ~20M edges), 5 runs each:
#   make bench_static
#   bash bench/bench_scaling.sh ./bench_static 1048576 20 5 \
#        --output bench/results_static.csv
#
#   # Batch-size / weak scaling:
#   make bench_dynamic
#   bash bench/bench_scaling.sh ./bench_dynamic 262144 20 3 \
#        --output bench/results_dynamic.csv
#
#   # SNAP file (strong scaling):
#   bash bench/bench_scaling.sh ./bench_static data/roadNet-CA.txt 5 \
#        --output bench/results_roadnet.csv
#
# The script writes a CSV header on first write and then appends one
# row per RESULT line emitted by the binary.
# ================================================================

set -euo pipefail

# ── Parse arguments ─────────────────────────────────────────────
BINARY=""
OUTPUT="bench/results.csv"
BINARY_ARGS=()

i=1
while [[ $i -le $# ]]; do
  arg="${!i}"
  if [[ "$arg" == "--output" ]]; then
    i=$((i+1)); OUTPUT="${!i}"
  elif [[ -z "$BINARY" ]]; then
    BINARY="$arg"
  else
    BINARY_ARGS+=("$arg")
  fi
  i=$((i+1))
done

if [[ -z "$BINARY" ]]; then
  echo "Usage: $0 <binary> [args...] [--output file.csv]" >&2
  exit 1
fi

if [[ ! -x "$BINARY" ]]; then
  echo "ERROR: '$BINARY' is not executable. Run 'make bench_static' first." >&2
  exit 1
fi

# ── Thread counts to sweep ───────────────────────────────────────
THREADS="${BENCH_THREADS:-1 2 4 8 16 32 48 64}"

# ── CSV setup ────────────────────────────────────────────────────
mkdir -p "$(dirname "$OUTPUT")"
# Write header if file doesn't exist yet.
if [[ ! -f "$OUTPUT" ]]; then
  echo "scenario,algo,threads,n,m,Delta,batch_size,runs,total_time_s,avg_time_s,avg_rounds,avg_colors" \
    > "$OUTPUT"
fi

echo "# Sweep: binary=$BINARY args=${BINARY_ARGS[*]:-} output=$OUTPUT"
echo "# Thread counts: $THREADS"
echo ""

for t in $THREADS; do
  echo "--- threads=$t ---"

  # If NUMA_NODE is set, pin via numactl (eliminates cross-socket penalty).
  # Example: NUMA_NODE=0 bash bench/bench_scaling.sh ./bench_static ...
  if [[ -n "${NUMA_NODE:-}" ]] && command -v numactl &>/dev/null; then
    RUNNER="numactl --cpunodebind=${NUMA_NODE} --membind=${NUMA_NODE}"
  else
    RUNNER=""
  fi

  # Run the binary, capture stdout; stderr goes to terminal.
  raw_output=$(PARLAY_NUM_THREADS=$t $RUNNER "$BINARY" "${BINARY_ARGS[@]:-}" 2>&1)
  echo "$raw_output"

  # Extract RESULT lines and convert to CSV rows.
  echo "$raw_output" | grep '^RESULT' | while read -r line; do
    # Each field is key=value; strip "RESULT" prefix, then parse.
    # Fields may be: algo, scenario, threads, n, m, Delta, batch_size,
    #                runs, total_time_s, avg_time_s, avg_rounds, avg_colors
    # We normalize to the full column set (missing fields become "").
    declare -A fields
    fields=()
    for kv in $line; do
      [[ "$kv" == "RESULT" ]] && continue
      key="${kv%%=*}"; val="${kv#*=}"
      fields[$key]="$val"
    done
    # bench_static prints time_s=...; dynamic bench uses total_time_s / avg_time_s.
    [[ -z "${fields[avg_time_s]:-}" && -n "${fields[time_s]:-}" ]] \
      && fields[avg_time_s]="${fields[time_s]}"
    [[ -z "${fields[total_time_s]:-}" && -n "${fields[time_s]:-}" ]] \
      && fields[total_time_s]="${fields[time_s]}"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
      "${fields[scenario]:-}"     \
      "${fields[algo]:-}"         \
      "${fields[threads]:-}"      \
      "${fields[n]:-}"            \
      "${fields[m]:-}"            \
      "${fields[Delta]:-}"        \
      "${fields[batch_size]:-}"   \
      "${fields[runs]:-}"         \
      "${fields[total_time_s]:-}" \
      "${fields[avg_time_s]:-}"   \
      "${fields[avg_rounds]:-}"   \
      "${fields[avg_colors]:-}"   \
      >> "$OUTPUT"
  done
  echo ""
done

echo "Results written to: $OUTPUT"
echo "Run 'python3 bench/parse_results.py $OUTPUT' to generate plots."
