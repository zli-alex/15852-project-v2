# ================================================================
# Build system for the parallel batch-dynamic (Delta+1)-coloring
# implementation (Ghaffari & Koo, arXiv:2512.09218).
#
# Designed for Linux/g++ on a multi-core cluster.
# Parlaylib uses its own work-stealing scheduler (no extra libs
# needed beyond pthreads).
#
# Usage:
#   make              – build optimised binary
#   make debug        – build with -g -O0 -fsanitize=address,undefined
#   make run          – build and run with default n=262144
#   make run N=65536  – build and run with n=65536
#   make clean        – remove build artefacts
# ================================================================

CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread
OPT      = -O2 -march=native

# Parlaylib headers
PARLAY_INC   = parlaylib/include

# Example helpers (graph_color.h, graph_utils.h, speculative_for.h, ...)
EXAMPLES_INC = parlaylib/examples

INCLUDES = -I$(PARLAY_INC) -I$(EXAMPLES_INC)

TARGET   = dynamic_graph_color
SRCS     = dynamic_graph_color.cpp
HDRS     = dynamic_graph_color.h

# ── Default: optimised build ──────────────────────────────────────
all: $(TARGET)

$(TARGET): $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) $(OPT) $(INCLUDES) -o $@ $(SRCS)

# ── Debug build with sanitisers ───────────────────────────────────
debug: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -g -O0 \
	  -fsanitize=address,undefined \
	  $(INCLUDES) -o $(TARGET)_debug $(SRCS)

# ── Run ──────────────────────────────────────────────────────────
N ?= 262144

run: $(TARGET)
	./$(TARGET) $(N)

run_debug: debug
	./$(TARGET)_debug $(N)

# ================================================================
# Test targets
# ================================================================
# tests/ include the helper/ directory from parlaylib/examples.
TESTS_INCLUDES = $(INCLUDES) -Itests

test_small: tests/test_small_graphs.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) $(OPT) $(TESTS_INCLUDES) -o $@ $<

test_snap: tests/test_snap.cpp tests/snap_loader.h $(HDRS)
	$(CXX) $(CXXFLAGS) $(OPT) $(TESTS_INCLUDES) -o $@ $<

test_stress: tests/test_stress.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) $(OPT) $(TESTS_INCLUDES) -o $@ $<

# Run all tests that don't need external data files.
test: test_small test_stress
	./test_small && ./test_stress

# ================================================================
# Benchmark targets
# ================================================================
bench_static: bench/bench_static.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) $(OPT) $(INCLUDES) -o $@ $<

bench_dynamic: bench/bench_dynamic.cpp $(HDRS)
	$(CXX) $(CXXFLAGS) $(OPT) $(INCLUDES) -o $@ $<

# Convenience: build all benchmarks.
bench: bench_static bench_dynamic

# ── Clean ────────────────────────────────────────────────────────
clean:
	rm -f $(TARGET) $(TARGET)_debug \
	      test_small test_snap test_stress \
	      bench_static bench_dynamic

.PHONY: all debug run run_debug clean \
        test_small test_snap test_stress test \
        bench_static bench_dynamic bench
