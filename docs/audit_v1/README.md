# Implementation audit vs. Ghaffari & Koo (arXiv:2512.09218)

**Scope:** [`dynamic_graph_color.h`](../../dynamic_graph_color.h) and the batch-dynamic procedures in Section 4 (Algorithms 5–7), plus shared definitions from Section 3.1–3.2 that those algorithms rely on.

**Reference:** Mohsen Ghaffari and Jaehyun Koo, *Dynamic Graph Coloring: Sequential, Parallel, and Distributed*, arXiv:2512.09218v1 (Dec 2025). Local copy: [`2512.09218v1-2.pdf`](../../2512.09218v1-2.pdf).

---

## 1. Notation and object mapping

| Paper | Implementation | Notes |
|--------|----------------|--------|
| Graph \(G=(V,E)\), \(|V|=n\) | `long n`, vertices `0 … n-1` | `vertex` typedef is `int`; `n` is `long`. |
| Maximum degree bound \(\Delta\) | `int Delta` | Passed in by caller; must upper-bound actual degree for the color universe \(\{0,\ldots,\Delta\}\). |
| Level \(l(v)\) | `level[v]` | Drawn once at construction; never updated (matches paper: levels are not dynamically maintained in Section 3–4). |
| Color \(c(v)\), \(\bot\) uncolored | `color[v]`, `-1` = uncolored | Paper uses \(\bot\); we use `-1`. |
| Color universe \(\{1,\ldots,\Delta+1\}\) | \(\{0,\ldots,\Delta\}\) | **Isomorphic indexing** (subtract 1). No algorithmic change. |
| Timestamp \(t(v)\) | `timestamp[v]` + `std::atomic<long> global_time` | Paper allows non-unique timestamps in parallel (Section 3.1); we assign strictly increasing values per finalized vertex in `recolor_batch` via `fetch_add`. |
| Adjacency / neighbors | `adj[v]` as `parlay::sequence<vertex>` | Undirected graph: each edge stored at both endpoints. |
| Sets \(S\), \(S_{\mathrm{nxt}}\) | `parlay::sequence<vertex>` + dedup | Paper uses set notation; we use sequences + `sort_dedup_vertices`. |
| “\(w \in S\)” test | `in_S[w]` (`uint8_t` array of size `n`) | **Engineering structure** not in paper; equivalent to a set membership bit. |
| `LowerEqualUsed(v)` | *Not a separate object* | Recomputed by scanning `adj[v]` and `level[]`, `color[]` (see §4). |
| `NB_\geq(v)\), `NB_>(v)\)` | *Not stored* | Derived by filtering neighbors with `level[w] >= level[v]` or `>`. |
| Batch \(B\) of edges | `edges` = `parlay::sequence<std::pair<vertex,vertex>>` | Undirected: each update is one pair \((u,v)\) with \(u,v\) in \(V\). |

---

## 2. What matches the paper (same logic)

### 2.1 Initialization (Section 3.1, Theorem 1.1 / 1.3)

- **Levels:** Geometric law: for \(k \in [1, \lceil \log \Delta \rceil]\), \(\Pr[l(v)=k]=2^{-k}\); top level has doubled mass. We implement this via repeated fair coin flips in `sample_geometric` until `max_level`, where `max_level = max(2, \lceil \log_2 \Delta \rceil + 1)\)` (see §3.8 for the distinction from an earlier off-by-one when \(\Delta\) was a power of two).

- **Initial colors:** Independent uniform on the \((\Delta+1)\)-element palette (paper’s \(\{1,\ldots,\Delta+1\}\); ours \(\{0,\ldots,\Delta\}\)).

- **Empty graph:** Constructor leaves `adj` empty; edges arrive only through `add_edge_batch`.

### 2.2 `SampleFromPalette` (Algorithm 1, Section 3.2)

**Mathematical definition of the palette \(P_v\)** (Definitions 3.4–3.5, Observations 3.6–3.7):

- Colors **not** used by any neighbor \(w\) with \(l(w) \le l(v)\) among **colored** neighbors (the color universe \(U_v\) relative to current coloring).
- Among remaining colors, reject if **more than one** neighbor with \(l(w) > l(v)\) uses that color (so accepted colors lie in \(P_v\)).

Our `sample_from_palette` builds the “used by \(\le\)-level neighbors” set from a neighbor scan, builds the list of colors in \(U_v\), then **rejection-samples** uniformly from \(U_v\) until the higher-neighbor count is at most 1. By Observation 3.7, a uniform draw from \(U_v\) lies in \(P_v\) with constant probability; expected iterations are \(O(1)\) per sample as in the paper’s while-loop analysis.

### 2.3 `RecolorBatch` (Algorithm 7, Section 4.1)

Per round, the implementation follows the same phases as the pseudocode:

1. **Uncolor \(S\)** — set \(c(v)=\bot\) for \(v\in S\); equivalent to paper’s deletion from neighbors’ `LowerEqualUsed` in aggregate, but we do not maintain those structures (see §4).

2. **Parallel palette samples** for all \(v\in S\) — matches “sample independently in parallel” (Section 4.1 / proof of Theorem 1.3).

3. **Intra-\(S\) conflicts** — paper inserts both endpoints into \(S_{\mathrm{nxt}}\) when \(w\in S\) and \(c(v)=c(w)\) in the scan over neighbors in \(NB_\geq(v)\). We enumerate neighbors with `in_S[w] && level[w] >= level[v] && color[w]==color[v]`, emit \(v\) and each such \(w\), then deduplicate. This is equivalent to building \(S_{\mathrm{nxt}}\) as a set of conflicting vertices.

4. **Uncolor conflicting vertices** and restrict to successful \(S\setminus S_{\mathrm{nxt}}\) — same as paper lines 13–15 (we form `S_ok` by filtering).

5. **`V\setminus S` conflicts** — for each \(v\in S\) that kept its color, find the unique neighbor \(w\notin S\) with \(l(w)>l(v)\) and \(c(w)=c(v)\) if it exists (paper lines 16–23; uniqueness follows from sampling from \(P_v\) as in Observation 4.1–4.3). We scan `adj[v]` with `!in_S[w] && level[w] > lv && color[w]==cv`.

6. **Insert conflict vertices into next \(S\)** — paper’s \(S_{\mathrm{nxt}}.\mathrm{Insert}(\mathrm{ConflictVertex})\); we append deduplicated `conflict_w` to `S_conflict`.

7. **Termination** — paper recurses on \(S_{\mathrm{nxt}}\); we set `S = append(S_conflict, conflict_w)` and loop until \(S\) is empty. Same mathematical process, iterative instead of recursive.

### 2.4 `AddEdgeBatch` (Algorithm 5)

- Update neighborhood structures **before** recoloring: we append new neighbors to `adj` (parallel `group_by_index` + `append`).

- For each inserted edge with \(c(u)=c(v)\), enqueue **one** endpoint for recoloring: the one with **not smaller** timestamp after the paper’s swap convention. We implement “pick \(u\) if \(t(u)\ge t(v)\), else \(v\)” which matches the swap logic **except** for the tie case \(t(u)=t(v)\) (see §3.1).

- Deduplicate \(S\) when one vertex appears in several bad edges — consistent with treating \(S\) as a set.

### 2.5 `DeleteEdgeBatch` (Algorithm 6)

- For each affected endpoint \(v\), compare \(d_\le^{\mathrm{old}}(v)\) and \(d_\le^{\mathrm{new}}(v)\) (Definition 3.1: neighbors with \(l(w)\le l(v)\)).

- If \(d_\le^{\mathrm{old}} > d_\le^{\mathrm{new}}\), add \(v\) to \(S\) with probability \(\dfrac{d_\le^{\mathrm{old}}-d_\le^{\mathrm{new}}}{\Delta+1-d_\le^{\mathrm{new}}(v)}\), matching Algorithm 6 line 4 (with \(k =\) decrease in \(d_\le\)).

- Then call `recolor_batch(S)` as in the paper.

---

## 3. Differences from the paper (and why)

### 3.1 Tie-breaking in `AddEdgeBatch` when \(t(u)=t(v)\)

**Paper (Algorithm 5, lines 4–7):** If \(c(u)=c(v)\) and \(t(v)<t(u)\), swap \((u,v)\); then always add the current second component \(v\) to \(S\).

So when **\(t(u)=t(v)\)** there is **no swap**, and the vertex added is the one named **`v` in the batch tuple** (depends on edge orientation in \(B\)).

**Implementation:** If \(t(u)\ge t(v)\), recolor **`u`**; else **`v`**. When timestamps are equal, we always recolor **`u`** (the first field of the pair).

**Why it’s a difference:** Not identical to the paper’s literal rule on ties, but still a **valid deterministic tie-break** for the same-color conflict rule. The analysis (Lemma 4.8, etc.) allows arbitrary consistent tie-breaking between simultaneous recolorings; the paper’s sequential phrasing is one convention.

**Possible alignment fix:** If strict textual fidelity matters, map each undirected edge to a canonical ordered pair (e.g. smaller vertex first) and then apply the paper’s “after swap, take `v`” rule on that ordering.

### 3.2 No persistent `LowerEqualUsed`, `NB_\geq`, `NB_>`

**Paper:** Each vertex maintains `LowerEqualUsed` (with complement sampling per Lemmas 3.3 / 4.12) and hash sets for \(NB_\geq\), \(NB_>\), updated in \(O(1)\) expected work per edge change (Section 3.1, Lemma 4.12, proof of Theorem 1.3).

**Implementation:** On every `sample_from_palette` and conflict scan, we **recompute** from `adj[v]`, `level[·]`, and `color[·]`.

**Why:** Simplifies code and avoids implementing the paper’s parallel hash-table machinery (GMV91-style batch structures). **Correctness** of the *combinatorial* steps (what is in \(U_v\), \(P_v\), who conflicts) is unchanged.

**Cost model impact:** Per-sample work becomes \(O(\deg(v))\) instead of \(O(|NB_>(v)|)\) in expectation as analyzed in the paper. **Depth** of each `RecolorBatch` round remains \(O(\log n)\) w.h.p. as in the paper’s abstract iteration structure; **total work** per batch can be higher in practice than the paper’s idealized \(O(1)\) per edge when \(\Delta\) is large.

### 3.3 `SampleFromPalette`: sampling procedure

**Paper:** `LowerEqualUsed(v).SampleEmpty()` (uniform from \(U_v\) via their data structure), then scan \(NB_>(v)\) to count higher-neighbor multiplicity.

**Implementation:** Build explicit list of colors in \(U_v\), draw uniformly from that list, count neighbors with \(l(w)>l(v)\) and that color.

**Why:** Equivalent *definition* of one trial; distribution of **accepted** colors matches “sample from \(P_v\)” up to the paper’s use of rejection inside Algorithm 1. We do not implement Lemma 3.3 / 4.11–4.12’s **dynamic-array + hash** representation of \(S\) and \(S^C\) for \(O(1)\) sampling when \(|S|\) is large.

### 3.4 `RecolorBatch`: recursion vs. iteration

**Paper:** Recursive `RecolorBatch(S_{\mathrm{nxt}})\)`.

**Implementation:** `while (!S.empty()) { ...; S = append(...); }`.

**Why:** Semantically identical; avoids stack depth and matches typical parallel C++ style.

### 3.5 `DeleteEdgeBatch`: edge removal in adjacency lists

**Paper:** Assumes neighborhood updates in \(O(1)\) expected per change via hashing (Section 2, Preliminaries).

**Implementation:** Sort deleted undirected edges as `(min,max)`, then **filter** each `adj[v]` in parallel with `binary_search` on the deleted list.

**Why:** Simple and correct. **Work** per batch is \(O\!\left(\sum_v \deg(v)\right)\) in the worst case over the batch, not \(O(|B|)\) as in the idealized proof.

### 3.6 Random number generation

**Paper:** Abstract “random bits” / oblivious adversary; parallel batch model uses independent randomness per vertex where needed.

**Implementation:** `parlay::random_generator` with deterministic seeding from `(global_time, round, v)`-style forks for palette samples; fixed seed `54321` for initial levels/colors; separate fork for delete-batch Bernoulli trials.

**Why:** Standard for reproducible engineering; not specified in the paper. Does not change the *type* of random process assumed in the analysis (independent geometric levels, independent samples per step).

### 3.7 Integer sort for deduplication

**Paper:** “Set” / hash insertions.

**Implementation:** `integer_sort_inplace` + pack-first-occurrence (`sort_dedup_vertices`).

**Why:** Purely an implementation detail; same resulting sets of vertices.

### 3.8 Level range `max_level` (Section 3.1) — corrected during this audit

**Paper:** \(l(v) \in \{1,\ldots,\lceil\log\Delta\rceil+1\}\) with the last level carrying the residual probability mass (doubled bucket).

**Earlier bug (pre-audit):** The constructor incremented `max_level` once per right-shift until `tmp==1`, which yielded **`floor(\log_2\Delta)+2`** levels for \(\Delta>1\). That matches \(\lceil\log_2\Delta\rceil+1\) **only when \(\Delta\) is not a power of two**; for \(\Delta=2^k\) it was **one level too many**.

**Current code:** `max_level = max(2, ceil_log2(Delta) + 1)` where `ceil_log2` is the standard “smallest \(r\) with \(2^r \ge x\)” for \(x\ge2\). This matches the paper’s \(\lceil\log_2\Delta\rceil+1\) up to the explicit **`max(2, …)`** guard for \(\Delta\in\{0,1\}\), where the paper’s indexing is degenerate anyway.

---

## 4. Design choices where the paper is silent or under-specified

1. **`S` representation:** Paper treats \(S\) as a mathematical set. We use a sequence plus explicit deduplication and an `in_S` bitvector for \(O(1)\) membership—needed for efficient parallel checks.

2. **Order of `append(S_conflict, conflict_w)`:** Paper’s \(S_{\mathrm{nxt}}\) accumulates intra-\(S\) conflicts first, then outside conflicts. We concatenate in that order; any order would preserve correctness of the next round as long as the multiset of tokens is correct (we deduplicate anyway).

3. **Timestamps after parallel recoloring:** Paper notes \(t(v)\) need not be unique in parallel. We still assign **strictly increasing** `global_time` slices to `S_ok` for deterministic `AddEdgeBatch` tie-breaking in the driver’s tests.

4. **Vertices as `int`:** Paper’s \(n\)-vertex graph is abstract; we assume vertex IDs fit in `int` and lie in `[0,n)\). For \(n > 2^{31}\) this would need a wider type.

5. **Duplicate edges in a batch:** Not discussed. If the same undirected edge appears twice in `add_edge_batch`, we append duplicate neighbors; correctness of coloring still holds, but degree counts and performance degrade. **Assumption:** batches are simple edge sets (or duplicates are rare).

6. **Self-loops / out-of-range vertices:** Not validated in the header; invalid input violates paper preconditions (Section 3.1: no loops, no duplicate edges).

---

## 5. Section coverage checklist

| Paper location | Covered in code? |
|----------------|------------------|
| §3.1 Levels, colors, timestamps, \(d_\le\) definition | Yes (`level`, `color`, `timestamp`, scans for \(d_\le\)) |
| §3.2 `AddEdge` / `DeleteEdge` (sequential) | **Partially** — we implement **batch** §4 versions only |
| Algorithm 1 `SampleFromPalette` | Yes (`sample_from_palette`) |
| Algorithm 5 `AddEdgeBatch` | Yes (`add_edge_batch`) |
| Algorithm 6 `DeleteEdgeBatch` | Yes (`delete_edge_batch`) |
| Algorithm 7 `RecolorBatch` | Yes (`recolor_batch`, iterative) |
| §4.2 Parallel hash tables (Lemma 4.11–4.12) | **No** — replaced by scans + `parlay` primitives |
| §4.3 Distributed / CONGEST | **Out of scope** — not implemented |

---

## 6. Summary

- **Algorithmically aligned:** Batch update rules (Algorithms 5–6), parallel `RecolorBatch` structure (Algorithm 7), palette definition, geometric levels, and delete-trigger probabilities match Section 4.1.
- **Main engineered deviations:** No incremental `LowerEqualUsed` / neighbor-hash structures; adjacency updates on delete by filtering; `AddEdgeBatch` tie-break when \(t(u)=t(v)\) differs from the paper’s “second endpoint after optional swap” unless edge ordering is chosen to match.
- **Intentional scope limit:** Sequential §3.2 `Recolor`/`AddEdge`/`DeleteEdge` entry points are not exposed as separate APIs; the driver uses batch APIs only. Distributed §4.3 is not implemented.

For questions or tightening alignment (especially **§3.1 tie-breaking** and **Lemma 4.12-style sampling**), this document is **audit version 1** — extend in a future `audit_v2` if the implementation changes.
