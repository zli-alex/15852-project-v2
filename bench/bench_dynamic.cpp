#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include <parlay/primitives.h>
#include <parlay/parallel.h>
#include <parlay/sequence.h>
#include <parlay/random.h>
#include <parlay/internal/get_time.h>

#include "../dynamic_graph_color.h"
#include "helper/graph_utils.h"

// ================================================================
// bench_dynamic — batch-size and weak-scaling benchmarks.
//
// Scenarios:
//   A. Batch-size scaling (fix graph, vary edges per batch):
//      Insert all m edges in batches of size 1, 10, 100, 1k, 10k, all.
//      Reports total time and average rounds per recolor_batch call.
//
//   B. Weak scaling (fix edges/thread, scale both n and threads):
//      n = threads * n_per_thread
//      Call with PARLAY_NUM_THREADS swept externally.
//
//   C. Delete-batch scaling:
//      Insert all edges, then delete them in batches of varying size.
//
// Output: RESULT lines (same format as bench_static).
//
// Usage:
//   PARLAY_NUM_THREADS=<p> ./bench_dynamic [n] [edges_per_vertex]
// ================================================================

using utils = graph_utils<vertex>;

// ----------------------------------------------------------------
// RESULT emitter
// ----------------------------------------------------------------
static void emit(const std::string& scenario,
                 long n, long m, int Delta,
                 long batch_size, int runs,
                 double total_time_s,
                 double avg_rounds,
                 int avg_colors) {
  std::cout << "RESULT"
            << "  scenario="    << scenario
            << "  threads="     << parlay::num_workers()
            << "  n="           << n
            << "  m="           << m
            << "  Delta="       << Delta
            << "  batch_size="  << batch_size
            << "  runs="        << runs
            << "  total_time_s="<< total_time_s
            << "  avg_time_s="  << total_time_s / runs
            << "  avg_rounds="  << avg_rounds
            << "  avg_colors="  << avg_colors
            << "\n";
}

// ----------------------------------------------------------------
// Slice helper
// ----------------------------------------------------------------
static edges slice_edges(const edges& E, long start, long end) {
  end = std::min(end, (long)E.size());
  return parlay::tabulate(end - start, [&](long i) { return E[start + i]; });
}

// ================================================================
// Scenario A: batch-size scaling
// ================================================================
static void bench_batch_size(long n, long m_approx, int runs) {
  std::cout << "\n# Scenario A: batch-size scaling n=" << n
            << " m~=" << m_approx << " runs=" << runs << "\n";

  auto G = utils::rmat_symmetric_graph(n, m_approx);
  n = (long)G.size();
  int Delta = (int)parlay::reduce(
    parlay::map(G, [](const auto& ns){ return ns.size(); }),
    parlay::maximum<size_t>());
  auto E_all = parlay::filter(utils::to_edges(G),
                              [](edge e){ return e.first < e.second; });
  long m = (long)E_all.size();

  // Batch sizes to test: adaptive to m so the smallest batch is at
  // most m/1000 (avoids millions of scheduler-startup overheads that
  // would stall for hours on large graphs).
  long min_bs = std::max(1L, m / 1000);
  std::vector<long> batch_sizes;
  for (long bs : {1L, 10L, 100L, 1000L, 10000L, 100000L}) {
    if (bs >= min_bs) batch_sizes.push_back(bs);
  }
  batch_sizes.push_back(m); // always include all-at-once
  // Deduplicate.
  batch_sizes.erase(std::unique(batch_sizes.begin(), batch_sizes.end()),
                    batch_sizes.end());

  for (long bs : batch_sizes) {
    if (bs > m) bs = m;
    double total_time = 0.0;
    double total_rounds = 0.0;
    int total_colors = 0;

    for (int r = 0; r < runs; r++) {
      DynamicGraphColoring dgc(n, Delta);
      long max_rounds = 0;

      parlay::internal::timer t("", false);
      t.start();
      for (long start = 0; start < m; start += bs) {
        auto batch = slice_edges(E_all, start, start + bs);
        dgc.add_edge_batch(batch);
        max_rounds = std::max(max_rounds, dgc.last_recolor_rounds);
      }
      total_time   += t.stop();
      total_rounds += (double)max_rounds;
      total_colors += parlay::reduce(dgc.get_coloring(),
                                     parlay::maximum<int>()) + 1;
    }

    emit("batch_size_insert", n, m, Delta, bs, runs,
         total_time, total_rounds / runs,
         (int)(total_colors / runs));
  }
}

// ================================================================
// Scenario B: weak scaling
//   n and m scale with num_workers so each worker handles a
//   constant fraction of the graph.
// ================================================================
static void bench_weak_scaling(long n_per_thread, long m_per_thread, int runs) {
  long p = (long)parlay::num_workers();
  long n = n_per_thread * p;
  long m_target = m_per_thread * p;

  std::cout << "\n# Scenario B: weak scaling p=" << p
            << " n=" << n << " m~=" << m_target << " runs=" << runs << "\n";

  auto G = utils::rmat_symmetric_graph(n, m_target);
  n = (long)G.size();
  int Delta = (int)parlay::reduce(
    parlay::map(G, [](const auto& ns){ return ns.size(); }),
    parlay::maximum<size_t>());
  auto E_all = parlay::filter(utils::to_edges(G),
                              [](edge e){ return e.first < e.second; });
  long m = (long)E_all.size();

  double total_time = 0.0;
  double total_rounds = 0.0;
  int total_colors = 0;
  for (int r = 0; r < runs; r++) {
    DynamicGraphColoring dgc(n, Delta);
    parlay::internal::timer t("", false);
    t.start();
    dgc.add_edge_batch(E_all);
    total_time   += t.stop();
    total_rounds += (double)dgc.last_recolor_rounds;
    total_colors += parlay::reduce(dgc.get_coloring(),
                                   parlay::maximum<int>()) + 1;
  }
  emit("weak_scaling", n, m, Delta, m, runs,
       total_time, total_rounds / runs, (int)(total_colors / runs));
}

// ================================================================
// Scenario C: delete-batch scaling
// ================================================================
static void bench_delete_batch_size(long n, long m_approx, int runs) {
  std::cout << "\n# Scenario C: delete batch-size scaling n=" << n
            << " m~=" << m_approx << " runs=" << runs << "\n";

  auto G = utils::rmat_symmetric_graph(n, m_approx);
  n = (long)G.size();
  int Delta = (int)parlay::reduce(
    parlay::map(G, [](const auto& ns){ return ns.size(); }),
    parlay::maximum<size_t>());
  auto E_all = parlay::filter(utils::to_edges(G),
                              [](edge e){ return e.first < e.second; });
  long m = (long)E_all.size();

  long min_bs_d = std::max(1L, m / 1000);
  std::vector<long> batch_sizes;
  for (long bs : {10L, 100L, 1000L, 10000L, 100000L}) {
    if (bs >= min_bs_d) batch_sizes.push_back(bs);
  }
  batch_sizes.push_back(m);
  batch_sizes.erase(std::unique(batch_sizes.begin(), batch_sizes.end()),
                    batch_sizes.end());

  for (long bs : batch_sizes) {
    if (bs > m) bs = m;
    double total_time = 0.0;
    double total_rounds = 0.0;
    int total_colors = 0;

    for (int r = 0; r < runs; r++) {
      // Always start fully inserted.
      DynamicGraphColoring dgc(n, Delta);
      dgc.add_edge_batch(E_all);
      long max_rounds = 0;

      parlay::internal::timer t("", false);
      t.start();
      for (long start = 0; start < m; start += bs) {
        auto batch = slice_edges(E_all, start, start + bs);
        dgc.delete_edge_batch(batch);
        max_rounds = std::max(max_rounds, dgc.last_recolor_rounds);
      }
      total_time   += t.stop();
      total_rounds += (double)max_rounds;
      total_colors += parlay::reduce(dgc.get_coloring(),
                                     parlay::maximum<int>()) + 1;
    }
    emit("batch_size_delete", n, m, Delta, bs, runs,
         total_time, total_rounds / runs, (int)(total_colors / runs));
  }
}

// ================================================================
// Main
// ================================================================
int main(int argc, char* argv[]) {
  long n   = (1L << 18);   // ~260k vertices
  long epv = 20;            // edges per vertex
  int  runs = 3;

  if (argc >= 2) try { n = std::stol(argv[1]); } catch (...) {}
  if (argc >= 3) try { epv = std::stol(argv[2]); } catch (...) {}
  if (argc >= 4) try { runs = std::stoi(argv[3]); } catch (...) {}

  long m_approx = n * epv;

  std::cout << "# bench_dynamic  threads=" << parlay::num_workers()
            << "  n=" << n << "  m~=" << m_approx
            << "  runs=" << runs << "\n";

  bench_batch_size(n, m_approx, runs);
  bench_delete_batch_size(n, m_approx, runs);

  // Weak scaling: each thread handles ~4096 vertices and ~40k edges.
  bench_weak_scaling(/*n_per_thread=*/4096, /*m_per_thread=*/40000, runs);

  return 0;
}
