#include <iostream>
#include <string>

#include <parlay/primitives.h>
#include <parlay/sequence.h>
#include <parlay/internal/get_time.h>

#include "../dynamic_graph_color.h"
// snap_loader.h pulls helper/graph_utils.h (only once — graph_utils.h
// has no include guard, so do not include it again here).
#include "snap_loader.h"

// ================================================================
// Test SNAP real-world graphs.
//
// Usage:
//   ./test_snap <path/to/edges.txt> [<path/to/edges2.txt> ...]
//
// Each file is loaded, colored, and checked.  The program exits with
// a non-zero code if any check fails.
//
// Example (after downloading from https://snap.stanford.edu/data/):
//   ./test_snap data/email-Enron.txt data/roadNet-CA.txt
//
// Recommended SNAP datasets (all undirected):
//   email-Enron.txt        37k  / 184k
//   com-amazon.ungraph.txt 335k / 926k
//   com-dblp.ungraph.txt   317k / 1.05M
//   roadNet-CA.txt         1.97M / 2.77M
//   com-youtube.ungraph.txt 1.13M / 2.99M
// ================================================================

// graph_utils<vertex> alias `utils` is defined in snap_loader.h.

// ================================================================
// Correctness checker
// ================================================================
static bool check_coloring(const graph& G,
                            const parlay::sequence<int>& colors,
                            int Delta) {
  long n = (long)G.size();
  auto ok = parlay::tabulate(n, [&](long u) -> bool {
    if (colors[u] < 0 || colors[u] > Delta) return false;
    for (vertex v : G[u])
      if (colors[u] == colors[v]) return false;
    return true;
  });
  return parlay::count_if(ok, [](bool b) { return !b; }) == 0;
}

static edges slice_edges(const edges& E, long start, long end) {
  return parlay::tabulate(end - start, [&](long i) { return E[start + i]; });
}

// ================================================================
// Test one file
// ================================================================
static bool test_snap_file(const std::string& path) {
  std::cout << "\n=== " << path << " ===\n";

  // Load graph.
  parlay::internal::timer load_t("load", false);
  load_t.start();
  auto [G, n, m, Delta] = load_snap_graph(path);
  double load_sec = load_t.stop();

  if (n == 0) {
    std::cout << "SKIP (empty or unreadable)\n";
    return true; // don't count as failure
  }

  std::cout << "n=" << n << " m=" << m << " Delta=" << Delta
            << " load_time=" << load_sec << "s\n";

  // Build undirected edge list (u < v only) for feeding to add_edge_batch.
  long n_out;
  auto E = snap_to_edges(path, n_out);
  // n_out should equal n from load_snap_graph; use the loaded n.

  bool all_ok = true;

  // ── Test 1: static coloring (insert all at once) ──────────────
  {
    parlay::internal::timer t("static", false);
    DynamicGraphColoring dgc(n, Delta);
    t.start();
    dgc.add_edge_batch(E);
    double elapsed = t.stop();

    bool ok = check_coloring(G, dgc.get_coloring(), Delta);
    int max_c = parlay::reduce(dgc.get_coloring(), parlay::maximum<int>());
    std::cout << (ok ? "PASS" : "FAIL")
              << " static  time=" << elapsed << "s"
              << " colors=" << max_c + 1 << "/" << Delta + 1 << "\n";
    if (!ok) all_ok = false;
  }

  // ── Test 2: two-phase dynamic insertion ───────────────────────
  {
    long half = (long)E.size() / 2;
    auto E1 = slice_edges(E, 0, half);
    auto E2 = slice_edges(E, half, (long)E.size());
    auto G1 = utils::symmetrize(E1, n);

    parlay::internal::timer t("2-phase", false);
    DynamicGraphColoring dgc(n, Delta);

    t.start();
    dgc.add_edge_batch(E1);
    double t1 = t.stop();
    bool ok1 = check_coloring(G1, dgc.get_coloring(), Delta);

    t.start();
    dgc.add_edge_batch(E2);
    double t2 = t.stop();
    bool ok2 = check_coloring(G, dgc.get_coloring(), Delta);

    std::cout << (ok1 ? "PASS" : "FAIL")
              << " 2-phase phase1 time=" << t1 << "s\n";
    std::cout << (ok2 ? "PASS" : "FAIL")
              << " 2-phase phase2 time=" << t2 << "s\n";
    if (!ok1 || !ok2) all_ok = false;
  }

  // ── Test 3: insert then delete 10% ───────────────────────────
  {
    long del_count = (long)E.size() / 10;
    if (del_count > 0) {
      auto E_del = slice_edges(E, 0, del_count);
      auto E_rem = slice_edges(E, del_count, (long)E.size());
      auto G_rem = utils::symmetrize(E_rem, n);

      parlay::internal::timer t("del", false);
      DynamicGraphColoring dgc(n, Delta);
      dgc.add_edge_batch(E);

      t.start();
      dgc.delete_edge_batch(E_del);
      double elapsed = t.stop();

      bool ok = check_coloring(G_rem, dgc.get_coloring(), Delta);
      int max_c = parlay::reduce(dgc.get_coloring(), parlay::maximum<int>());
      std::cout << (ok ? "PASS" : "FAIL")
                << " delete-10%  time=" << elapsed << "s"
                << " colors=" << max_c + 1 << "/" << Delta + 1 << "\n";
      if (!ok) all_ok = false;
    }
  }

  return all_ok;
}

// ================================================================
// Main
// ================================================================
int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cout << "Usage: test_snap <snap_file.txt> [<snap_file2.txt> ...]\n\n"
              << "Downloads from https://snap.stanford.edu/data/\n"
              << "Recommended files (ascending size):\n"
              << "  email-Enron.txt       (37k nodes, 184k edges)\n"
              << "  com-amazon.ungraph.txt (335k, 926k)\n"
              << "  com-dblp.ungraph.txt   (317k, 1.05M)\n"
              << "  roadNet-CA.txt         (1.97M, 2.77M)\n"
              << "  com-youtube.ungraph.txt (1.13M, 2.99M)\n";
    return 0;
  }

  int failures = 0;
  for (int i = 1; i < argc; i++) {
    if (!test_snap_file(argv[i])) failures++;
  }

  std::cout << "\n=== SNAP tests: "
            << (argc - 1 - failures) << " PASS, "
            << failures << " FAIL ===\n";
  return failures > 0 ? 1 : 0;
}
