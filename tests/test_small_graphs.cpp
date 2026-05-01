#include <iostream>
#include <string>
#include <vector>

#include <parlay/primitives.h>
#include <parlay/sequence.h>

#include "../dynamic_graph_color.h"
#include "helper/graph_utils.h"

using utils = graph_utils<vertex>;

// ================================================================
// Test infrastructure
// ================================================================
static int g_pass = 0, g_fail = 0;

// Check all four correctness properties.
static bool full_check(const std::string& label,
                       const graph& G,
                       const DynamicGraphColoring& dgc) {
  const auto& colors = dgc.get_coloring();
  long n = (long)G.size();
  int Delta = dgc.Delta;
  bool ok = true;

  // (1) Properness + (2) palette bound
  for (long u = 0; u < n; u++) {
    if (colors[u] < 0 || colors[u] > Delta) {
      std::cerr << "  FAIL [" << label << "] vertex " << u
                << " color=" << colors[u] << " out of [0," << Delta << "]\n";
      ok = false;
    }
    for (vertex v : G[u]) {
      if (colors[u] == colors[v]) {
        std::cerr << "  FAIL [" << label << "] edge (" << u << "," << v
                  << ") same color " << colors[u] << "\n";
        ok = false;
      }
    }
  }

  // (3) No uncolored
  for (long u = 0; u < n; u++) {
    if (colors[u] < 0) {
      std::cerr << "  FAIL [" << label << "] vertex " << u << " uncolored\n";
      ok = false;
    }
  }

  if (ok) {
    int max_c = parlay::reduce(colors, parlay::maximum<int>());
    std::cout << "  PASS [" << label << "] colors used: "
              << max_c + 1 << " / " << Delta + 1 << "\n";
    g_pass++;
  } else {
    g_fail++;
  }
  return ok;
}

// Build a graph and DGC from an explicit edge list.
static std::pair<graph, DynamicGraphColoring>
make_and_color(const std::string& label,
               int n, int Delta,
               const edges& edge_list) {
  graph G = utils::symmetrize(edge_list, n);
  DynamicGraphColoring dgc(n, Delta);
  dgc.add_edge_batch(edge_list);
  full_check(label, G, dgc);
  return {std::move(G), std::move(dgc)};
}

// Verify coloring after deleting a subset of edges.
static void check_after_delete(const std::string& label,
                                DynamicGraphColoring& dgc,
                                int n,
                                const edges& remaining) {
  graph G_rem = utils::symmetrize(remaining, n);
  full_check(label, G_rem, dgc);
}

// ================================================================
// Graph constructors
// ================================================================

static edges make_complete(int n) {
  edges out;
  for (int u = 0; u < n; u++)
    for (int v = u + 1; v < n; v++)
      out.push_back({u, v});
  return out;
}

static edges make_path(int n) {
  edges out;
  for (int i = 0; i < n - 1; i++) out.push_back({i, i + 1});
  return out;
}

static edges make_cycle(int n) {
  edges out = make_path(n);
  out.push_back({n - 1, 0});
  return out;
}

static edges make_complete_bipartite(int a, int b) {
  edges out;
  for (int u = 0; u < a; u++)
    for (int v = a; v < a + b; v++)
      out.push_back({u, v});
  return out;
}

static edges make_star(int n) {
  // hub = 0, leaves = 1 .. n-1
  edges out;
  for (int i = 1; i < n; i++) out.push_back({0, i});
  return out;
}

// Petersen graph: outer 5-cycle + inner 5-cycle with cross-edges.
static edges make_petersen() {
  return edges{
    // outer pentagon: 0-1-2-3-4-0
    {0,1},{1,2},{2,3},{3,4},{4,0},
    // inner pentagram: 5-7-9-6-8-5
    {5,7},{7,9},{9,6},{6,8},{8,5},
    // spokes: 0-5, 1-6, 2-7, 3-8, 4-9
    {0,5},{1,6},{2,7},{3,8},{4,9}
  };
}

// ================================================================
// Individual test cases
// ================================================================

static void test_k2() {
  std::cout << "-- K2 (single edge) --\n";
  edges E = {{0, 1}};
  make_and_color("K2 static", 2, 1, E);
}

static void test_path() {
  std::cout << "-- Path P10 --\n";
  auto E = make_path(10);
  make_and_color("P10 static", 10, 2, E);

  // Dynamic: insert edges one at a time.
  DynamicGraphColoring dgc(10, 2);
  graph G = utils::symmetrize(E, 10);
  for (auto& e : E) dgc.add_edge_batch(edges{e});
  full_check("P10 edge-by-edge", G, dgc);
}

static void test_cycle_even() {
  std::cout << "-- Cycle C8 (even, 2-colorable) --\n";
  auto E = make_cycle(8);
  make_and_color("C8 static", 8, 2, E);
}

static void test_cycle_odd() {
  std::cout << "-- Cycle C7 (odd, needs 3 colors) --\n";
  // Delta=2, so Delta+1=3 colors available — enough for odd cycle.
  auto E = make_cycle(7);
  make_and_color("C7 static", 7, 2, E);
}

static void test_complete_k5() {
  std::cout << "-- Complete K5 --\n";
  auto E = make_complete(5);
  make_and_color("K5 static", 5, 4, E);
}

static void test_complete_k6() {
  std::cout << "-- Complete K6 --\n";
  auto E = make_complete(6);
  make_and_color("K6 static", 6, 5, E);
}

static void test_bipartite_k33() {
  std::cout << "-- Complete bipartite K3,3 --\n";
  // Delta=3 (each vertex adjacent to all 3 in the other part).
  auto E = make_complete_bipartite(3, 3);
  make_and_color("K3,3 static", 6, 3, E);
}

static void test_star() {
  std::cout << "-- Star S11 (hub + 10 leaves) --\n";
  // Hub has degree 10; Delta=10.
  auto E = make_star(11);
  make_and_color("S11 static", 11, 10, E);
}

static void test_petersen() {
  std::cout << "-- Petersen graph (3-regular, chi=3) --\n";
  auto E = make_petersen();
  make_and_color("Petersen static", 10, 3, E);
}

static void test_delete_path() {
  std::cout << "-- Path P10: insert then delete middle edge --\n";
  int n = 10, Delta = 2;
  auto E = make_path(n);
  auto [G, dgc] = make_and_color("P10 before delete", n, Delta, E);

  // Delete edge (4,5) — splits into two sub-paths.
  edges del = {{4, 5}};
  dgc.delete_edge_batch(del);
  edges remaining;
  for (auto& e : E) if (!(e == edge{4,5})) remaining.push_back(e);
  check_after_delete("P10 after deleting (4,5)", dgc, n, remaining);
}

static void test_delete_cycle() {
  std::cout << "-- Cycle C8: insert then delete one edge (becomes path) --\n";
  int n = 8, Delta = 2;
  auto E = make_cycle(n);
  auto [G, dgc] = make_and_color("C8 before delete", n, Delta, E);

  edges del = {{7, 0}};
  dgc.delete_edge_batch(del);
  auto remaining = make_path(n); // 0-1-2-...-7
  check_after_delete("C8 after deleting (7,0)", dgc, n, remaining);
}

static void test_delete_clique() {
  std::cout << "-- K5: insert all edges, delete one, verify residual --\n";
  int n = 5, Delta = 4;
  auto E = make_complete(n);
  auto [G, dgc] = make_and_color("K5 before delete", n, Delta, E);

  edges del = {{0, 1}};
  dgc.delete_edge_batch(del);
  edges remaining;
  for (auto& e : E) if (!(e == edge{0,1})) remaining.push_back(e);
  check_after_delete("K5 after deleting (0,1)", dgc, n, remaining);
}

static void test_empty_graph() {
  std::cout << "-- Empty graph (no edges) --\n";
  int n = 5, Delta = 4;
  graph G(n); // no edges
  DynamicGraphColoring dgc(n, Delta);
  // No add_edge_batch call; all vertices have their initial random colors.
  // With no edges, any coloring is valid.
  full_check("Empty n=5", G, dgc);
}

static void test_two_components() {
  std::cout << "-- Two disjoint K3 components --\n";
  // Vertices 0-2: K3; vertices 3-5: K3.
  edges E = {{0,1},{1,2},{0,2},{3,4},{4,5},{3,5}};
  make_and_color("2xK3 static", 6, 2, E);
}

static void test_large_path_batches() {
  std::cout << "-- Path P100: insert in 10 batches of 9-10 edges each --\n";
  int n = 100, Delta = 2;
  auto E = make_path(n); // 99 edges

  DynamicGraphColoring dgc(n, Delta);
  int batch_sz = 10;
  for (int start = 0; start < (int)E.size(); start += batch_sz) {
    int end_idx = std::min(start + batch_sz, (int)E.size());
    edges batch(E.begin() + start, E.begin() + end_idx);
    dgc.add_edge_batch(batch);
  }
  graph G = utils::symmetrize(E, n);
  full_check("P100 10-batch insert", G, dgc);
}

// ================================================================
// Main
// ================================================================
int main() {
  std::cout << "=== Small Graph Unit Tests ===\n\n";

  test_empty_graph();
  test_k2();
  test_path();
  test_cycle_even();
  test_cycle_odd();
  test_complete_k5();
  test_complete_k6();
  test_bipartite_k33();
  test_star();
  test_petersen();
  test_two_components();
  test_large_path_batches();
  test_delete_path();
  test_delete_cycle();
  test_delete_clique();

  std::cout << "\n=== Results: " << g_pass << " PASS, "
            << g_fail << " FAIL ===\n";
  return (g_fail > 0) ? 1 : 0;
}
