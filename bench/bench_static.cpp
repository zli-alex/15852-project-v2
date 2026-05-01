#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <cstdlib>

#include <parlay/primitives.h>
#include <parlay/parallel.h>
#include <parlay/sequence.h>
#include <parlay/internal/get_time.h>

#include "../dynamic_graph_color.h"
#include "helper/graph_utils.h"
#include "graph_color.h"        // existing greedy coloring for comparison

// ================================================================
// bench_static — strong-scaling benchmark for static coloring.
//
// Usage:
//   PARLAY_NUM_THREADS=<p>  ./bench_static [n] [edges_per_vertex] [runs]
//   PARLAY_NUM_THREADS=<p>  ./bench_static <snap_file.txt> [runs]
//
// Output: one RESULT line per run (machine-parseable):
//   RESULT  algo=gk24  threads=8  n=262144  m=2621440  Delta=312
//           time_s=0.4321  rounds=7  colors=287
//
// Designed to be driven by bench/bench_scaling.sh which sweeps
// PARLAY_NUM_THREADS over 1 2 4 8 16 32 48 64.
// ================================================================

using utils = graph_utils<vertex>;

// ----------------------------------------------------------------
// Emit one machine-parseable RESULT line.
// ----------------------------------------------------------------
static void emit_result(const std::string& algo,
                        long n, long m, int Delta,
                        double time_s, long rounds, int colors) {
  std::cout << "RESULT"
            << "  algo="    << algo
            << "  threads=" << parlay::num_workers()
            << "  n="       << n
            << "  m="       << m
            << "  Delta="   << Delta
            << "  time_s="  << time_s
            << "  rounds="  << rounds
            << "  colors="  << colors
            << "\n";
}

// ----------------------------------------------------------------
// Run both algorithms on the given edge list and graph, emit results.
// ----------------------------------------------------------------
static void run_bench(const graph& G, const edges& E,
                      long n, long m, int Delta, int runs) {
  for (int r = 0; r < runs; r++) {
    // GK24 batch-dynamic (static mode).
    {
      parlay::internal::timer t("", false);
      DynamicGraphColoring dgc(n, Delta);
      t.start();
      dgc.add_edge_batch(E);
      double elapsed = t.stop();
      int max_c = parlay::reduce(dgc.get_coloring(), parlay::maximum<int>());
      emit_result("gk24", n, m, Delta, elapsed,
                  dgc.last_recolor_rounds, max_c + 1);
    }

    // Greedy speculative parallel coloring (from graph_color.h).
    {
      parlay::internal::timer t("", false);
      t.start();
      auto cols = graph_coloring(G);
      double elapsed = t.stop();
      int max_c = parlay::reduce(cols, parlay::maximum<int>());
      emit_result("greedy", n, m, Delta, elapsed, /*rounds=*/-1, max_c + 1);
    }
  }
}

// ----------------------------------------------------------------
// Load a SNAP file (if filename doesn't parse as a number).
// ----------------------------------------------------------------
static bool try_load_snap(const std::string& arg,
                          graph& G_out, edges& E_out,
                          long& n_out, long& m_out, int& Delta_out) {
  // Quick check: if arg starts with a digit, treat as n.
  if (!arg.empty() && std::isdigit((unsigned char)arg[0])) return false;

  std::cout << "# Loading SNAP file: " << arg << "\n";

  // Read the SNAP edge list into edges (u < v only).
  std::ifstream fin(arg);
  if (!fin.is_open()) {
    std::cerr << "Cannot open " << arg << "\n"; return false;
  }

  std::vector<std::pair<long,long>> raw;
  std::unordered_map<long,long> id_map;
  long next_id = 0;
  std::string line;
  while (std::getline(fin, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream iss(line);
    long u, v; if (!(iss >> u >> v) || u == v) continue;
    auto remap = [&](long x) -> long {
      auto it = id_map.find(x);
      if (it == id_map.end()) { id_map[x] = next_id++; return next_id - 1; }
      return it->second;
    };
    long ru = remap(u), rv = remap(v);
    if (ru < rv) raw.push_back({ru, rv});
  }

  n_out = next_id;
  E_out = parlay::tabulate((long)raw.size(), [&](long i) -> edge {
    return {(vertex)raw[i].first, (vertex)raw[i].second};
  });
  G_out = utils::symmetrize(E_out, n_out);
  m_out = (long)E_out.size();
  Delta_out = (int)parlay::reduce(
    parlay::map(G_out, [](const auto& ns){ return ns.size(); }),
    parlay::maximum<size_t>());
  return true;
}

// ================================================================
// Main
// ================================================================
int main(int argc, char* argv[]) {
  // Defaults.
  long n   = (1L << 18);   // ~260k vertices
  long epv = 20;            // edges per vertex
  int  runs = 5;

  graph G;
  edges E;
  long  m;
  int   Delta;
  bool  from_file = false;

  if (argc >= 2) {
    from_file = try_load_snap(argv[1], G, E, n, m, Delta);
    if (!from_file) {
      try { n = std::stol(argv[1]); } catch (...) {}
    }
  }
  if (!from_file && argc >= 3) {
    try { epv = std::stol(argv[2]); } catch (...) {}
  }
  if (argc >= (from_file ? 3 : 4)) {
    try { runs = std::stoi(argv[from_file ? 2 : 3]); } catch (...) {}
  }

  if (!from_file) {
    std::cout << "# Generating RMAT graph n=" << n
              << " m~=" << n * epv << "\n";
    G = utils::rmat_symmetric_graph(n, n * epv);
    n = (long)G.size();
    E = parlay::filter(utils::to_edges(G),
                       [](edge e){ return e.first < e.second; });
    m = (long)E.size();
    Delta = (int)parlay::reduce(
      parlay::map(G, [](const auto& ns){ return ns.size(); }),
      parlay::maximum<size_t>());
  }

  std::cout << "# threads=" << parlay::num_workers()
            << " n=" << n << " m=" << m << " Delta=" << Delta
            << " runs=" << runs << "\n";

  run_bench(G, E, n, m, Delta, runs);
  return 0;
}
