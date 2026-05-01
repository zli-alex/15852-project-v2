#include <iostream>
#include <random>
#include <string>

#include <parlay/primitives.h>
#include <parlay/random.h>
#include <parlay/sequence.h>

#include "../dynamic_graph_color.h"
#include "helper/graph_utils.h"

// ================================================================
// Randomized stress tests for the parallel batch-dynamic coloring.
//
// Tests:
//  1. Multi-phase insert  — 10 equal-sized batches, check after each.
//  2. Insert-delete-reinsert — insert all → delete 50% → reinsert 25%.
//  3. Adversarial batch  — a batch designed to maximize |S| (every
//     edge creates a conflict).
//  4. Varying Delta      — tight (actual max-degree) vs. loose (2x).
//  5. Random delete-insert cycle — repeated random batches.
// ================================================================

using utils = graph_utils<vertex>;

static int g_pass = 0, g_fail = 0;

// ================================================================
// Helpers
// ================================================================
static bool check_coloring(const std::string& label,
                            const graph& G,
                            const DynamicGraphColoring& dgc) {
  const auto& colors = dgc.get_coloring();
  long n = (long)G.size();
  int Delta = dgc.Delta;

  auto ok_flags = parlay::tabulate(n, [&](long u) -> bool {
    if (colors[u] < 0 || colors[u] > Delta) return false;
    for (vertex v : G[u])
      if (colors[u] == colors[v]) return false;
    return true;
  });
  bool ok = parlay::count_if(ok_flags, [](bool b) { return !b; }) == 0;

  if (ok) {
    int max_c = parlay::reduce(colors, parlay::maximum<int>());
    std::cout << "  PASS [" << label << "] colors=" << max_c + 1
              << "/" << Delta + 1 << "\n";
    g_pass++;
  } else {
    std::cout << "  FAIL [" << label << "]\n";
    g_fail++;
  }
  return ok;
}

static edges slice_edges(const edges& E, long start, long end) {
  return parlay::tabulate(end - start, [&](long i) { return E[start + i]; });
}

// Build undirected edge list from adjacency-list graph.
static edges graph_to_edges(const graph& G) {
  return parlay::filter(utils::to_edges(G),
                        [](edge e) { return e.first < e.second; });
}

// ================================================================
// Test 1: Multi-phase insert (10 equal batches, check after each)
// ================================================================
static void test_multi_phase_insert(long n, long m) {
  std::cout << "\n-- Test 1: Multi-phase insert (n=" << n
            << " m=" << m << " 10 batches) --\n";

  auto G = utils::rmat_symmetric_graph(n, m);
  n = (long)G.size();
  int Delta = (int)parlay::reduce(
    parlay::map(G, [](const auto& ns){ return ns.size(); }),
    parlay::maximum<size_t>());
  auto E = graph_to_edges(G);
  long total = (long)E.size();
  int batches = 10;

  DynamicGraphColoring dgc(n, Delta);
  // Accumulate inserted edges into a sub-graph for intermediate checks.
  edges inserted_so_far;

  for (int b = 0; b < batches; b++) {
    long start = (long)b * total / batches;
    long end   = (long)(b + 1) * total / batches;
    auto batch = slice_edges(E, start, end);
    dgc.add_edge_batch(batch);
    for (auto& e : batch) inserted_so_far.push_back(e);

    auto G_so_far = utils::symmetrize(inserted_so_far, n);
    check_coloring("phase " + std::to_string(b + 1) + "/10", G_so_far, dgc);
  }
}

// ================================================================
// Test 2: Insert-delete-reinsert
// ================================================================
static void test_insert_delete_reinsert(long n, long m) {
  std::cout << "\n-- Test 2: Insert→Delete 50%→Reinsert 25% (n=" << n
            << " m=" << m << ") --\n";

  auto G = utils::rmat_symmetric_graph(n, m);
  n = (long)G.size();
  int Delta = (int)parlay::reduce(
    parlay::map(G, [](const auto& ns){ return ns.size(); }),
    parlay::maximum<size_t>());
  auto E = graph_to_edges(G);
  long total = (long)E.size();

  DynamicGraphColoring dgc(n, Delta);

  // Insert all.
  dgc.add_edge_batch(E);
  check_coloring("after full insert", G, dgc);

  // Delete first 50%.
  long del_end = total / 2;
  auto E_del = slice_edges(E, 0, del_end);
  auto E_rem = slice_edges(E, del_end, total);
  dgc.delete_edge_batch(E_del);
  auto G_rem = utils::symmetrize(E_rem, n);
  check_coloring("after deleting 50%", G_rem, dgc);

  // Reinsert first 25% of the deleted edges.
  long reins_end = del_end / 2;
  auto E_reins = slice_edges(E_del, 0, reins_end);
  dgc.add_edge_batch(E_reins);
  auto E_after_reins = parlay::append(E_rem, E_reins);
  auto G_after = utils::symmetrize(E_after_reins, n);
  check_coloring("after reinserting 25%", G_after, dgc);
}

// ================================================================
// Test 3: Adversarial batch — all edges cause conflicts.
//
// Construct: a star with hub colored 0, then insert all spoke edges
// in one batch.  Each spoke (hub, leaf) has c(hub)==c(leaf)==0 (by
// carefully pre-loading the coloring — we insert spokes only after
// forcing initial colors, then reset the leaves to 0 via deletions).
//
// Simpler practical approach: use a biclique K_{d,d}.  If all
// left vertices happen to have the same initial color as the right
// vertices (very unlikely but possible), every edge in the batch is
// conflicting.  Instead, we construct a worst case deterministically:
// insert a batch of d disjoint edges where both endpoints were colored
// identically by inserting them into K_{2d} in two rounds.
//
// Implementation: build a matching of size M; for each matched pair
// (u_i, v_i) start with color[u_i] = color[v_i] = i % (Delta+1).
// We force this by initializing DGC with Delta=0 (all same color),
// then set Delta to the real value for the test batch.
//
// The simplest faithful adversarial test: use RMAT, insert all edges,
// record which pairs share a color, then delete all and re-insert
// only those conflicting pairs as the "adversarial batch".
// ================================================================
static void test_adversarial_batch(long n, long m) {
  std::cout << "\n-- Test 3: Adversarial batch (conflicting edges only) --\n";

  auto G = utils::rmat_symmetric_graph(n, m);
  n = (long)G.size();
  int Delta = (int)parlay::reduce(
    parlay::map(G, [](const auto& ns){ return ns.size(); }),
    parlay::maximum<size_t>());
  auto E = graph_to_edges(G);

  // Practical adversarial batch: a random matching where (by
  // probability) endpoints share initial colors, forcing |S| to be
  // large.  We use a small matching to keep the test fast.
  {
    int small_n = 1000;
    int adv_Delta = 4; // small, so the matching fits in Delta+1 colors
    // Build a perfect matching on 2*small_n vertices.
    edges matching;
    for (int i = 0; i < small_n; i++)
      matching.push_back({2 * i, 2 * i + 1});

    graph match_G = utils::symmetrize(matching, 2 * small_n);
    DynamicGraphColoring dgc(2 * small_n, adv_Delta);
    dgc.add_edge_batch(matching);
    check_coloring("adversarial matching n=" + std::to_string(2*small_n),
                   match_G, dgc);
  }

  // Also verify the full RMAT graph colors correctly.
  {
    DynamicGraphColoring dgc_rmat(n, Delta);
    dgc_rmat.add_edge_batch(E);
    check_coloring("adversarial RMAT baseline", G, dgc_rmat);
  }
}

// ================================================================
// Test 4: Varying Delta (tight vs. loose)
// ================================================================
static void test_varying_delta(long n, long m) {
  std::cout << "\n-- Test 4: Tight vs. loose Delta --\n";

  auto G = utils::rmat_symmetric_graph(n, m);
  n = (long)G.size();
  int Delta_actual = (int)parlay::reduce(
    parlay::map(G, [](const auto& ns){ return ns.size(); }),
    parlay::maximum<size_t>());
  auto E = graph_to_edges(G);

  // Tight: Delta = actual max-degree.
  {
    DynamicGraphColoring dgc(n, Delta_actual);
    dgc.add_edge_batch(E);
    check_coloring("tight Delta=" + std::to_string(Delta_actual), G, dgc);
  }

  // Loose: Delta = 2 * actual max-degree (larger palette → fewer rounds).
  {
    DynamicGraphColoring dgc(n, 2 * Delta_actual);
    dgc.add_edge_batch(E);
    // Graph G still has max-degree Delta_actual, so any 2*Delta_actual+1
    // coloring is valid; check_coloring accepts any color in [0, 2*Delta].
    check_coloring("loose Delta=" + std::to_string(2 * Delta_actual), G, dgc);
  }
}

// ================================================================
// Test 5: Random delete-insert cycles
// ================================================================
static void test_random_cycles(long n, long m, int num_cycles) {
  std::cout << "\n-- Test 5: " << num_cycles
            << " random delete/insert cycles (n=" << n
            << " m=" << m << ") --\n";

  auto G = utils::rmat_symmetric_graph(n, m);
  n = (long)G.size();
  int Delta = (int)parlay::reduce(
    parlay::map(G, [](const auto& ns){ return ns.size(); }),
    parlay::maximum<size_t>());
  auto E_all = graph_to_edges(G);
  long total = (long)E_all.size();

  DynamicGraphColoring dgc(n, Delta);
  dgc.add_edge_batch(E_all);
  check_coloring("initial full insert", G, dgc);

  // Keep track of which edges are "currently inserted" via a bool mask.
  std::vector<bool> present(total, true);
  std::mt19937 rng(42);

  edges current_E = E_all;

  for (int c = 0; c < num_cycles; c++) {
    // Randomly delete ~20% of present edges.
    std::vector<int> present_idx;
    for (int i = 0; i < (int)total; i++) if (present[i]) present_idx.push_back(i);
    int n_del = std::max(1, (int)(present_idx.size() * 0.2));
    std::shuffle(present_idx.begin(), present_idx.end(), rng);

    edges to_del;
    for (int i = 0; i < n_del; i++) {
      to_del.push_back(E_all[present_idx[i]]);
      present[present_idx[i]] = false;
    }
    dgc.delete_edge_batch(to_del);

    // Randomly insert ~15% of absent edges.
    std::vector<int> absent_idx;
    for (int i = 0; i < (int)total; i++) if (!present[i]) absent_idx.push_back(i);
    int n_ins = std::max(1, (int)(absent_idx.size() * 0.15));
    std::shuffle(absent_idx.begin(), absent_idx.end(), rng);

    edges to_ins;
    for (int i = 0; i < n_ins; i++) {
      to_ins.push_back(E_all[absent_idx[i]]);
      present[absent_idx[i]] = true;
    }
    dgc.add_edge_batch(to_ins);

    // Build current graph for checking.
    edges current;
    for (int i = 0; i < (int)total; i++) if (present[i]) current.push_back(E_all[i]);
    auto G_curr = utils::symmetrize(current, n);
    check_coloring("cycle " + std::to_string(c + 1), G_curr, dgc);
  }
}

// ================================================================
// Main
// ================================================================
int main(int argc, char* argv[]) {
  // Small and medium scale for stress tests.
  long n_small  = (1L << 12);  // 4k vertices
  long m_small  = n_small * 10;
  long n_medium = (1L << 16);  // 64k vertices
  long m_medium = n_medium * 10;

  if (argc >= 2) {
    try { n_medium = std::stol(argv[1]); m_medium = n_medium * 10; }
    catch (...) {}
  }

  std::cout << "=== Stress Tests ===\n";

  test_multi_phase_insert(n_small, m_small);
  test_insert_delete_reinsert(n_small, m_small);
  test_adversarial_batch(n_small, m_small);
  test_varying_delta(n_small, m_small);
  test_random_cycles(n_small, m_small, /*num_cycles=*/5);

  // Medium scale (skip most expensive cycle test).
  test_multi_phase_insert(n_medium, m_medium);
  test_insert_delete_reinsert(n_medium, m_medium);
  test_varying_delta(n_medium, m_medium);

  std::cout << "\n=== Results: " << g_pass << " PASS, "
            << g_fail << " FAIL ===\n";
  return (g_fail > 0) ? 1 : 0;
}
