#pragma once

// ================================================================
// Parallel Batch-Dynamic (Delta+1) Graph Coloring
// Based on: Ghaffari & Koo, arXiv:2512.09218, Sections 3–4.
//
// Maintains a proper (Delta+1)-coloring under batch edge insertions
// and deletions via Algorithm 5, 6, 7 from the paper.
//
// Key design choices vs. the paper:
//   - LowerEqualUsed, NB>=, NB> are NOT stored persistently.
//     They are computed on-the-fly from adj[] + level[] each round.
//     This trades O(deg(v)) per sample against no incremental
//     hash-table maintenance, keeping the code simple.
//   - Adjacency lists are parlay::sequence<vertex>; deletions are
//     implemented by filtering the list (O(m) per batch).
//   - in_S is a flat uint8_t array for O(1) membership checks.
// ================================================================

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>
#include <random>
#include <utility>

#include <parlay/primitives.h>
#include <parlay/sequence.h>
#include <parlay/random.h>
#include <parlay/parallel.h>

using vertex = int;
using edge   = std::pair<vertex, vertex>;
using edges  = parlay::sequence<edge>;
using graph  = parlay::sequence<parlay::sequence<vertex>>;

// ----------------------------------------------------------------
// Sort a sequence of vertices and remove duplicates in parallel.
// Vertices must be non-negative integers (uses integer_sort).
// ----------------------------------------------------------------
static parlay::sequence<vertex>
sort_dedup_vertices(parlay::sequence<vertex> S) {
  if (S.empty()) return S;
  parlay::integer_sort_inplace(S, [](vertex v) { return (size_t)v; });
  // keep only the first occurrence of each value
  auto is_first = parlay::tabulate(S.size(), [&](size_t i) -> bool {
    return (i == 0 || S[i] != S[i - 1]);
  });
  return parlay::pack(S, is_first);
}

// ----------------------------------------------------------------
// Sample from the geometric distribution used for level assignment.
//   P[l(v) = k] = 2^{-k}          for k in [1, max_level-1]
//   P[l(v) = max_level] = 2^{-(max_level-1)}   (doubled last bucket)
// Returns a level in [1, max_level].
// ----------------------------------------------------------------
static int sample_geometric(parlay::random_generator& rng, int max_level) {
  for (int k = 1; k < max_level; k++) {
    if (rng() & 1ULL) return k;
  }
  return max_level;
}

// ================================================================
// DynamicGraphColoring
// ================================================================
struct DynamicGraphColoring {

  long n;         // number of vertices
  int  Delta;     // degree upper bound; colors are in {0,...,Delta}
  int  max_level; // ceil(log2(Delta)) + 1

  graph                  adj;       // current adjacency lists
  parlay::sequence<int>  level;     // l(v): geometric level (fixed at init)
  parlay::sequence<int>  color;     // c(v): current color, -1 = uncolored
  parlay::sequence<long> timestamp; // t(v): last recoloring time
  std::atomic<long>      global_time;

  // Diagnostics updated by the most recent recolor_batch call.
  // last_recolor_rounds: number of while-loop iterations executed.
  // Should be O(log n) w.h.p. per Lemma 4.5 of the paper.
  long last_recolor_rounds = 0;

  // --------------------------------------------------------------
  // Construct with n vertices and degree bound Delta.
  // No edges initially; call add_edge_batch to insert edges.
  // Each vertex receives a random level and an initial random color.
  // --------------------------------------------------------------
  DynamicGraphColoring(long n, int Delta)
    : n(n), Delta(Delta),
      adj(n), level(n), color(n), timestamp(n, 0L),
      global_time(1L) {

    // Paper §3.1: levels 1 .. ceil(log Delta)+1 with geometric 2^{-k}
    // on the first ceil(log Delta) levels and doubled mass on the last.
    // We use log base 2 (consistent with 2^{-k} in the paper).
    // max_level = ceil(log2(Delta)) + 1; keep at least 2 for tiny Delta.
    auto ceil_log2 = [](int x) -> int {
      if (x <= 1) return 0;
      int r = 0;
      for (int t = x - 1; t > 0; t >>= 1) r++;
      return r;
    };
    max_level = std::max(2, ceil_log2(Delta) + 1);

    parlay::random_generator gen(/*seed=*/54321UL);
    parlay::parallel_for(0, n, [&](long v) {
      auto rng = gen[(size_t)v];
      level[v] = sample_geometric(rng, max_level);
      // initial color: uniform random in [0, Delta]
      std::uniform_int_distribution<int> dis(0, Delta);
      color[v] = dis(rng);
    });
  }

  // --------------------------------------------------------------
  // SampleFromPalette(v)  —  Algorithm 1 of the paper.
  //
  // Precondition : color[v] == -1 (vertex is currently uncolored).
  // Postcondition: color[v] is set to a color drawn from palette Pv.
  //
  // Palette Pv = { c in [0,Delta] :
  //   (a) no neighbor w with level[w] <= level[v]  has color c
  //   (b) at most one neighbor w with level[w] > level[v] has color c }
  //
  // Strategy (on-the-fly, no persistent LowerEqualUsed structure):
  //   1. Scan adj[v] to build the "used" set (colors of
  //      lower/equal-level colored neighbors) = complement of Uv.
  //   2. Build the free-color list (= Uv) explicitly.
  //   3. Repeatedly sample c uniformly from Uv.  By Obs. 3.7,
  //      |Pv| >= |Uv|/2, so each trial is in Pv with prob >= 1/2.
  //      Reject if more than one higher-level neighbor uses c.
  // --------------------------------------------------------------
  void sample_from_palette(vertex v, parlay::random_generator& rng) {
    int lv = level[v];
    const auto& nbrs = adj[v];

    // Step 1: collect colors used by lower/equal-level colored neighbors.
    // Use a flat bitset sized Delta+1 to avoid heap allocation per call.
    std::vector<bool> used(Delta + 1, false);
    for (vertex w : nbrs) {
      if (level[w] <= lv) {
        int cw = color[w];
        if (cw >= 0) used[cw] = true;
      }
    }

    // Step 2: build explicit list of free colors (= Uv).
    // Preallocate to avoid repeated growth; reserve is O(1) amortized.
    std::vector<int> free_colors;
    free_colors.reserve(Delta + 1);
    for (int c = 0; c <= Delta; c++) {
      if (!used[c]) free_colors.push_back(c);
    }

    // Step 3: rejection-sample from Pv.
    // Count hi-level usages per free color in one pass (avoids a second
    // O(degree) scan per rejection); store counts in the same `used` space.
    // We repurpose `used` as a count array capped at 2 (we only need <=1).
    std::fill(used.begin(), used.end(), false);
    std::vector<int> hi_count(Delta + 1, 0);
    for (vertex w : nbrs) {
      if (level[w] > lv) {
        int cw = color[w];
        if (cw >= 0 && hi_count[cw] < 2) hi_count[cw]++;
      }
    }

    // |Pv| >= |Uv|/2 by Observation 3.7, so expected 2 trials.
    while (true) {
      int c = free_colors[rng() % free_colors.size()];
      if (hi_count[c] <= 1) {
        color[v] = c;
        return;
      }
    }
  }

  // --------------------------------------------------------------
  // RecolorBatch(S)  —  Algorithm 7 (parallel iterative version).
  //
  // Given a set S of vertices to (re-)color, runs O(log n) rounds
  // w.h.p. until S is empty.
  //
  // Invariant entering each round:
  //   Every vertex in V\S is colored, and V\S is properly colored
  //   with respect to itself (no two adjacent V\S vertices share a
  //   color).
  // --------------------------------------------------------------
  void recolor_batch(parlay::sequence<vertex> S) {
    if (S.empty()) return;

    // O(n) membership array: in_S[v] = 1 iff v is currently in S.
    parlay::sequence<uint8_t> in_S(n, 0);
    parlay::parallel_for(0, (long)S.size(), [&](long i) {
      in_S[S[i]] = 1;
    });

    parlay::random_generator gen((size_t)global_time.load());
    long round = 0;

    while (!S.empty()) {
      long sz = (long)S.size();

      // ── Phase 1: uncolor all vertices in S (lines 1-4) ──────────
      // (LowerEqualUsed maintained on-the-fly: simply mark color=-1)
      parlay::parallel_for(0, sz, [&](long i) {
        color[S[i]] = -1;
      });

      // ── Phase 2: sample from palette in parallel (lines 6-7) ────
      // grain_size=1 gives maximum parallelism; keep at 1 since each
      // call is O(Delta) — heavy enough to amortise task overhead.
      parlay::parallel_for(0, sz, [&](long i) {
        vertex v = S[i];
        auto rng = gen[(size_t)(round * n + v)];
        sample_from_palette(v, rng);
      }, /*grain_size=*/1);

      // ── Phase 3: detect S-S conflicts (lines 8-12) ───────────────
      // For each v in S, scan NB>=(v) ∩ S for a color match.
      // Emit v and every conflicting partner w into S_conflict.
      auto conflict_lists = parlay::tabulate(sz,
        [&](long i) -> parlay::sequence<vertex> {
          vertex v = S[i];
          int cv = color[v]; // freshly sampled (>= 0)
          int lv = level[v];
          parlay::sequence<vertex> out;
          bool flagged_self = false;
          for (vertex w : adj[v]) {
            // NB>=(v): neighbors with level >= level[v]
            if (in_S[w] && level[w] >= lv && color[w] == cv) {
              if (!flagged_self) { out.push_back(v); flagged_self = true; }
              out.push_back(w);
            }
          }
          return out;
        });
      auto S_conflict = sort_dedup_vertices(parlay::flatten(conflict_lists));

      // ── Phase 4: uncolor S_conflict; derive S_ok = S \ S_conflict ─
      parlay::parallel_for(0, (long)S_conflict.size(), [&](long i) {
        color[S_conflict[i]] = -1;
      });
      // S_conflict is sorted (from integer_sort inside sort_dedup).
      // Use binary search to filter S_ok efficiently.
      auto S_ok = parlay::filter(S, [&](vertex v) {
        return !std::binary_search(S_conflict.begin(), S_conflict.end(), v);
      });

      // ── Phase 5: find V\S conflict vertices for S_ok (lines 16-23) ─
      // For each v in S_ok, find the (at most one, by palette) neighbor
      // w in V\S with level[w] > level[v] and color[w] == color[v].
      // NOTE: in_S still reflects {S_conflict ∪ S_ok} = original S here,
      //       so !in_S[w] correctly identifies vertices outside S.
      auto conflict_w_raw = parlay::map(S_ok, [&](vertex v) -> vertex {
        int cv = color[v];
        int lv = level[v];
        for (vertex w : adj[v]) {
          // w must be in V\S (not in S), at strictly higher level,
          // and colored with the same color as v.
          if (!in_S[w] && level[w] > lv && color[w] == cv) {
            return w; // at most one such w by palette property
          }
        }
        return -1;
      });
      auto conflict_w_raw2 = parlay::filter(conflict_w_raw,
                                            [](vertex w) { return w != -1; });
      // A V\S vertex can be the conflict for multiple S_ok vertices;
      // deduplicate so it enters S only once.
      auto conflict_w = sort_dedup_vertices(conflict_w_raw2);

      // ── Phase 6: finalize S_ok — assign timestamps, update in_S ──
      // Timestamps needed by add_edge_batch tie-breaking.
      long t_base = global_time.fetch_add((long)S_ok.size());
      parlay::parallel_for(0, (long)S_ok.size(), [&](long i) {
        timestamp[S_ok[i]] = t_base + i;
      });
      // S_ok vertices leave S.
      parlay::parallel_for(0, (long)S_ok.size(), [&](long i) {
        in_S[S_ok[i]] = 0;
      });
      // New V\S conflict vertices join S.
      parlay::parallel_for(0, (long)conflict_w.size(), [&](long i) {
        in_S[conflict_w[i]] = 1;
      });

      // ── Phase 7: build next S = S_conflict ∪ conflict_w ─────────
      // S_conflict vertices already have in_S = 1.
      // conflict_w vertices just had in_S set to 1.
      S = parlay::append(S_conflict, conflict_w);
      round++;
    }
    last_recolor_rounds = round; // record for diagnostics / benchmarks
  }

  // --------------------------------------------------------------
  // AddEdgeBatch  —  Algorithm 5 of the paper.
  //
  // Insert a batch of undirected edges.  For each new edge (u,v)
  // where c(u)==c(v), recolor the endpoint with the larger timestamp
  // (i.e., the more recently colored one).
  // --------------------------------------------------------------
  void add_edge_batch(const edges& batch) {
    if (batch.empty()) return;

    // ── Step 1: update adjacency lists ───────────────────────────
    // Create both directed copies of each undirected edge, then
    // group by source vertex to find new neighbors per vertex.
    auto directed = parlay::flatten(
      parlay::map(batch, [](edge e) -> parlay::sequence<edge> {
        return {{e.first, e.second}, {e.second, e.first}};
      }));
    // group_by_index(pairs, n): groups by first element,
    // returns sequence<sequence<second>> of size n.
    auto new_nbrs = parlay::group_by_index(directed, n);
    parlay::parallel_for(0, n, [&](long v) {
      if (!new_nbrs[v].empty())
        adj[v] = parlay::append(adj[v], new_nbrs[v]);
    });

    // ── Step 2: collect conflicting endpoints into S ──────────────
    // For (u,v) with c(u)==c(v): recolor whichever has the larger
    // timestamp (ties broken by larger vertex id).  Algorithm 5,
    // lines 4-6: "take the endpoint whose timestamp is not less".
    auto candidates = parlay::map(batch, [&](edge e) -> vertex {
      auto [u, v] = e;
      if (color[u] >= 0 && color[u] == color[v]) {
        // Return the vertex with the larger (or equal) timestamp.
        if (timestamp[u] >= timestamp[v]) return u;
        return v;
      }
      return -1;
    });
    auto S_raw = parlay::filter(candidates, [](vertex v) { return v != -1; });
    // A vertex may appear in multiple conflicting edges; keep it once.
    auto S = sort_dedup_vertices(S_raw);

    recolor_batch(std::move(S));
  }

  // --------------------------------------------------------------
  // DeleteEdgeBatch  —  Algorithm 6 of the paper.
  //
  // Delete a batch of undirected edges.  For each vertex v whose
  // d_<=(v) (lower/equal-level degree) decreased by k, add v to the
  // recoloring set S with probability k / (Delta+1 - d_<=(v)_new).
  // --------------------------------------------------------------
  void delete_edge_batch(const edges& batch) {
    if (batch.empty()) return;

    // ── Step 1: collect affected vertices ────────────────────────
    auto affected_raw = parlay::flatten(
      parlay::map(batch, [](edge e) -> parlay::sequence<vertex> {
        return {e.first, e.second};
      }));
    auto affected = sort_dedup_vertices(affected_raw);
    long m_aff = (long)affected.size();

    // ── Step 2: compute d_<=(v)_old before deletion ───────────────
    auto d_le_old = parlay::map(affected, [&](vertex v) -> int {
      int lv = level[v], cnt = 0;
      for (vertex w : adj[v])
        if (level[w] <= lv) cnt++;
      return cnt;
    });

    // ── Step 3: remove deleted edges from adjacency lists ─────────
    // Normalize each deleted edge as (min,max) for canonical lookup.
    auto del_keys = parlay::sort(
      parlay::map(batch, [](edge e) {
        return std::make_pair(std::min(e.first, e.second),
                              std::max(e.first, e.second));
      }));
    parlay::parallel_for(0, n, [&](long v) {
      adj[v] = parlay::filter(adj[v], [&](vertex w) {
        auto key = std::make_pair(std::min((vertex)v, w),
                                  std::max((vertex)v, w));
        return !std::binary_search(del_keys.begin(), del_keys.end(), key);
      });
    });

    // ── Step 4: compute d_<=(v)_new after deletion ────────────────
    auto d_le_new = parlay::map(affected, [&](vertex v) -> int {
      int lv = level[v], cnt = 0;
      for (vertex w : adj[v])
        if (level[w] <= lv) cnt++;
      return cnt;
    });

    // ── Step 5: probabilistically build S (Algorithm 6, lines 3-4) ─
    // v enters S with probability (d_old - d_new) / (Delta+1 - d_new).
    parlay::random_generator gen((size_t)(global_time.load() + 777UL));
    auto in_S_flags = parlay::tabulate(m_aff, [&](long i) -> bool {
      int old_d = d_le_old[i], new_d = d_le_new[i];
      if (old_d <= new_d) return false; // d_<= did not decrease
      int k     = old_d - new_d;
      int denom = Delta + 1 - new_d;
      if (denom <= 0) return true;      // degenerate: always recolor
      // Accept with probability k / denom
      auto rng = gen[(size_t)i];
      return (int)(rng() % (unsigned long long)denom) < k;
    });
    auto S = parlay::pack(affected, in_S_flags);

    recolor_batch(std::move(S));
  }

  // Convenience accessor
  const parlay::sequence<int>& get_coloring() const { return color; }
};
