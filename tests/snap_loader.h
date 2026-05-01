#pragma once

// ================================================================
// SNAP edge-list loader
//
// SNAP graphs (https://snap.stanford.edu/data/) are distributed as
// plain-text edge lists:
//   - Lines starting with '#' are comments (ignored).
//   - Each remaining line is "u<TAB>v" (0-indexed or 1-indexed,
//     both are handled; we renumber to 0-indexed).
//   - Edges are directed; the graph is made undirected by
//     symmetrizing via graph_utils::symmetrize.
//
// Usage:
//   auto [G, n, m, Delta] = load_snap_graph("path/to/edges.txt");
//
// Returns:
//   G      – parlay::sequence<parlay::sequence<vertex>> adjacency list
//   n      – number of vertices (renumbered 0..n-1)
//   m      – number of undirected edges
//   Delta  – maximum degree
// ================================================================

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>

#include <parlay/primitives.h>
#include <parlay/sequence.h>

#include "../dynamic_graph_color.h"   // for vertex, edge, edges, graph
#include "helper/graph_utils.h"

using utils = graph_utils<vertex>;

// ----------------------------------------------------------------
// load_snap_graph(filename)
//
// Reads a SNAP-format edge list, renumbers vertices to [0,n),
// symmetrizes, and returns (G, n, m, Delta).
// ----------------------------------------------------------------
static std::tuple<graph, long, long, int>
load_snap_graph(const std::string& filename) {
  std::ifstream fin(filename);
  if (!fin.is_open()) {
    std::cerr << "ERROR: cannot open \"" << filename << "\"\n";
    return {};
  }

  // Pass 1: read raw (u,v) pairs, build renaming map.
  std::vector<std::pair<long,long>> raw_edges;
  std::unordered_map<long,long> id_map;
  long next_id = 0;

  std::string line;
  while (std::getline(fin, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream iss(line);
    long u, v;
    if (!(iss >> u >> v)) continue;
    if (u == v) continue; // skip self-loops

    auto remap = [&](long x) -> long {
      auto it = id_map.find(x);
      if (it == id_map.end()) { id_map[x] = next_id++; return next_id - 1; }
      return it->second;
    };
    raw_edges.push_back({remap(u), remap(v)});
  }
  fin.close();

  long n = next_id;
  if (n == 0 || raw_edges.empty()) {
    std::cerr << "ERROR: empty graph in \"" << filename << "\"\n";
    return {};
  }

  // Convert to parlay::sequence<edge>.
  edges E = parlay::tabulate((long)raw_edges.size(), [&](long i) -> edge {
    return {(vertex)raw_edges[i].first, (vertex)raw_edges[i].second};
  });

  // Symmetrize and deduplicate.
  graph G = utils::symmetrize(E, n);

  // Compute m (undirected) and Delta.
  long m_half = (long)parlay::reduce(
    parlay::map(G, [](const auto& ns) { return (long)ns.size(); })) / 2;

  int Delta = (int)parlay::reduce(
    parlay::map(G, [](const auto& ns) { return ns.size(); }),
    parlay::maximum<size_t>());

  return {std::move(G), n, m_half, Delta};
}

// ----------------------------------------------------------------
// snap_to_edges(filename)
//
// Variant that returns the raw (undirected, each once) edge list
// rather than an adjacency list.  Useful for feeding directly into
// add_edge_batch.
// ----------------------------------------------------------------
static edges snap_to_edges(const std::string& filename, long& n_out) {
  std::ifstream fin(filename);
  if (!fin.is_open()) {
    std::cerr << "ERROR: cannot open \"" << filename << "\"\n";
    n_out = 0;
    return {};
  }

  std::vector<std::pair<long,long>> raw_edges;
  std::unordered_map<long,long> id_map;
  long next_id = 0;

  std::string line;
  while (std::getline(fin, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream iss(line);
    long u, v;
    if (!(iss >> u >> v)) continue;
    if (u == v) continue;

    auto remap = [&](long x) -> long {
      auto it = id_map.find(x);
      if (it == id_map.end()) { id_map[x] = next_id++; return next_id - 1; }
      return it->second;
    };
    long ru = remap(u), rv = remap(v);
    if (ru < rv) raw_edges.push_back({ru, rv}); // keep only u<v once
  }
  fin.close();

  n_out = next_id;
  return parlay::tabulate((long)raw_edges.size(), [&](long i) -> edge {
    return {(vertex)raw_edges[i].first, (vertex)raw_edges[i].second};
  });
}
