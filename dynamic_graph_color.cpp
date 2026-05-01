#include <iostream>
#include <string>

#include <parlay/primitives.h>
#include <parlay/sequence.h>
#include <parlay/internal/get_time.h>

#include "dynamic_graph_color.h"
#include "helper/graph_utils.h"

// Existing static greedy coloring from parlaylib examples (for comparison).
#include "graph_color.h"

// ================================================================
// Types
// ================================================================
using utils = graph_utils<vertex>;

// ================================================================
// Correctness checker
//
// Verifies:
//   1. Every vertex has a valid color in [0, Delta].
//   2. No two adjacent vertices share the same color.
// ================================================================
static bool check_coloring(const graph&                 G,
                            const parlay::sequence<int>& colors,
                            int                          Delta) {
  long n = (long)G.size();
  auto ok = parlay::tabulate(n, [&](long u) -> bool {
    if (colors[u] < 0 || colors[u] > Delta) return false;
    for (vertex v : G[u])
      if (colors[u] == colors[v]) return false;
    return true;
  });
  return parlay::count_if(ok, [](bool b) { return !b; }) == 0;
}

// Slice a parlay::sequence into a sub-sequence [start, end).
static edges slice_edges(const edges& E, long start, long end) {
  return parlay::tabulate(end - start, [&](long i) { return E[start + i]; });
}

// ================================================================
// Main driver
// ================================================================
int main(int argc, char* argv[]) {
  // Parse optional command-line argument: number of vertices.
  long n = (1L << 18); // default ~260k vertices
  if (argc >= 2) {
    try { n = std::stol(argv[1]); }
    catch (...) { std::cerr << "invalid n; using default\n"; }
  }

  // ── Generate RMAT symmetric graph ──────────────────────────────
  // rmat_symmetric_graph internally rounds n to the nearest power of two.
  std::cout << "Generating RMAT graph (~" << n << " vertices, "
            << 20 * n << " directed edges)...\n";
  parlay::internal::timer gen_t("graph generation", false);
  gen_t.start();
  auto G = utils::rmat_symmetric_graph(n, 20 * n);
  double gen_sec = gen_t.stop();
  std::cout << "Generation time: " << gen_sec << " s\n";

  n = (long)G.size(); // actual n (power of two after rounding)
  utils::print_graph_stats(G);

  // Delta = maximum degree.
  int Delta = (int)parlay::reduce(
    parlay::map(G, [](const auto& nbrs) { return nbrs.size(); }),
    parlay::maximum<size_t>());
  std::cout << "Delta (max degree) = " << Delta << "\n\n";

  // Flat undirected edge list: each edge kept once (u < v).
  auto all_directed = utils::to_edges(G);
  auto E = parlay::filter(all_directed,
                          [](edge e) { return e.first < e.second; });
  long m = (long)E.size();
  std::cout << "Undirected edges: " << m << "\n\n";

  // ================================================================
  // TEST 1 — Static coloring (all edges in one AddEdgeBatch call)
  // ================================================================
  std::cout << "=== TEST 1: Static coloring (one AddEdgeBatch) ===\n";
  {
    DynamicGraphColoring dgc(n, Delta);

    parlay::internal::timer t("T1", false);
    t.start();
    dgc.add_edge_batch(E);
    double elapsed = t.stop();
    std::cout << "Time: " << elapsed << " s\n";

    bool ok = check_coloring(G, dgc.get_coloring(), Delta);
    int max_c = parlay::reduce(dgc.get_coloring(), parlay::maximum<int>());
    std::cout << (ok ? "PASS" : "FAIL") << " — proper coloring, "
              << max_c + 1 << " / " << Delta + 1 << " colors used\n\n";
  }

  // ================================================================
  // TEST 2 — Dynamic coloring: two-phase insertion
  // ================================================================
  std::cout << "=== TEST 2: Dynamic coloring (two-phase insertion) ===\n";
  {
    long half = m / 2;
    auto E1 = slice_edges(E, 0, half);
    auto E2 = slice_edges(E, half, m);

    DynamicGraphColoring dgc(n, Delta);

    parlay::internal::timer t("T2", false);

    // Phase 1: insert first half.
    t.start();
    dgc.add_edge_batch(E1);
    std::cout << "Phase-1 time: " << t.stop() << " s\n";

    // Check against the subgraph induced by E1.
    auto G1 = utils::symmetrize(E1, n);
    bool ok1 = check_coloring(G1, dgc.get_coloring(), Delta);
    std::cout << (ok1 ? "PASS" : "FAIL")
              << " — after inserting " << half << " edges\n";

    // Phase 2: insert second half.
    t.start();
    dgc.add_edge_batch(E2);
    std::cout << "Phase-2 time: " << t.stop() << " s\n";

    bool ok2 = check_coloring(G, dgc.get_coloring(), Delta);
    int max_c = parlay::reduce(dgc.get_coloring(), parlay::maximum<int>());
    std::cout << (ok2 ? "PASS" : "FAIL")
              << " — after inserting full graph, "
              << max_c + 1 << " / " << Delta + 1 << " colors used\n\n";
  }

  // ================================================================
  // TEST 3 — Dynamic coloring: insertion then deletion
  // ================================================================
  std::cout << "=== TEST 3: Dynamic coloring (insert then delete) ===\n";
  {
    DynamicGraphColoring dgc(n, Delta);

    parlay::internal::timer t("T3", false);

    // Insert all edges.
    t.start();
    dgc.add_edge_batch(E);
    std::cout << "Insert all time: " << t.stop() << " s\n";

    bool ok_full = check_coloring(G, dgc.get_coloring(), Delta);
    std::cout << (ok_full ? "PASS" : "FAIL") << " — after full insertion\n";

    // Delete the first 10% of edges.
    long del_count = m / 10;
    auto E_del = slice_edges(E, 0, del_count);
    auto E_rem = slice_edges(E, del_count, m);

    t.start();
    dgc.delete_edge_batch(E_del);
    std::cout << "Delete 10% time: " << t.stop() << " s\n";

    // Build the residual graph for checking.
    auto G_rem = utils::symmetrize(E_rem, n);
    bool ok_del = check_coloring(G_rem, dgc.get_coloring(), Delta);
    int max_c = parlay::reduce(dgc.get_coloring(), parlay::maximum<int>());
    std::cout << (ok_del ? "PASS" : "FAIL")
              << " — after deleting " << del_count << " edges, "
              << max_c + 1 << " / " << Delta + 1 << " colors used\n\n";
  }

  // ================================================================
  // BENCHMARK — compare against parlaylib greedy static coloring
  // ================================================================
  std::cout << "=== BENCHMARK: GK24 batch-dynamic vs. greedy ===\n";
  {
    int runs = 3;
    double gk24_total = 0.0, greedy_total = 0.0;

    for (int i = 0; i < runs; i++) {
      // GK24: insert all edges in one batch (static mode).
      {
        parlay::internal::timer t("", false);
        DynamicGraphColoring dgc(n, Delta);
        t.start();
        dgc.add_edge_batch(E);
        gk24_total += t.stop(); // stop() returns elapsed seconds
      }

      // Greedy: speculative parallel coloring from graph_color.h.
      {
        parlay::internal::timer t("", false);
        t.start();
        auto cols = graph_coloring(G);
        greedy_total += t.stop();
        (void)cols;
      }
    }

    std::cout << "GK24    avg time: " << gk24_total   / runs << " s\n";
    std::cout << "Greedy  avg time: " << greedy_total / runs << " s\n\n";
  }

  // ================================================================
  // TEST 4 — Small path graph (hand-verifiable)
  // ================================================================
  std::cout << "=== TEST 4: Path graph P6 (n=6, Delta=2) ===\n";
  {
    //  0 - 1 - 2 - 3 - 4 - 5
    int sn = 6, sDelta = 2;
    edges path_edges = {{0,1},{1,2},{2,3},{3,4},{4,5}};
    auto path_G = utils::symmetrize(path_edges, sn);

    DynamicGraphColoring dgc(sn, sDelta);
    dgc.add_edge_batch(path_edges);

    bool ok = check_coloring(path_G, dgc.get_coloring(), sDelta);
    std::cout << (ok ? "PASS" : "FAIL") << " — path graph\n";
    std::cout << "Colors: ";
    for (int i = 0; i < sn; i++) std::cout << dgc.get_coloring()[i] << " ";
    std::cout << "\n\n";
  }

  // ================================================================
  // TEST 5 — Complete graph K4 (Delta=3, needs exactly 4 colors)
  // ================================================================
  std::cout << "=== TEST 5: Complete graph K4 (n=4, Delta=3) ===\n";
  {
    int sn = 4, sDelta = 3;
    edges k4_edges = {{0,1},{0,2},{0,3},{1,2},{1,3},{2,3}};
    auto k4_G = utils::symmetrize(k4_edges, sn);

    DynamicGraphColoring dgc(sn, sDelta);
    dgc.add_edge_batch(k4_edges);

    bool ok = check_coloring(k4_G, dgc.get_coloring(), sDelta);
    std::cout << (ok ? "PASS" : "FAIL") << " — K4\n";
    std::cout << "Colors: ";
    for (int i = 0; i < sn; i++) std::cout << dgc.get_coloring()[i] << " ";
    std::cout << "\n\n";
  }

  // ================================================================
  // TEST 6 — Dynamic: incremental edge-by-edge insertion (n=8 cycle)
  // ================================================================
  std::cout << "=== TEST 6: Cycle C8 built edge-by-edge ===\n";
  {
    int sn = 8, sDelta = 2;
    DynamicGraphColoring dgc(sn, sDelta);

    // Insert one edge at a time (batch size = 1).
    parlay::sequence<edge> cycle_edges = {
      {0,1},{1,2},{2,3},{3,4},{4,5},{5,6},{6,7},{7,0}
    };
    auto cycle_G = utils::symmetrize(cycle_edges, sn);

    for (auto& e : cycle_edges) {
      dgc.add_edge_batch(edges{e});
    }

    bool ok = check_coloring(cycle_G, dgc.get_coloring(), sDelta);
    std::cout << (ok ? "PASS" : "FAIL") << " — C8 edge-by-edge\n";
    std::cout << "Colors: ";
    for (int i = 0; i < sn; i++) std::cout << dgc.get_coloring()[i] << " ";
    std::cout << "\n\n";
  }

  std::cout << "All tests complete.\n";
  return 0;
}
