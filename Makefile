# --- Tools ---
CXX          = clang++
LLVM_CONFIG  = llvm-config

# --- Flags ---
# -g is included to help preserve some naming metadata
CXXFLAGS     = -rdynamic $(shell $(LLVM_CONFIG) --cxxflags) -fPIC -g -std=c++20
LDFLAGS      = $(shell $(LLVM_CONFIG) --ldflags | tr '\n' ' ') -Wl,--exclude-libs,ALL

# --- Directories ---
BUILDDIR     = build
DEPDIR       = $(BUILDDIR)/.deps
TESTDIR      = $(BUILDDIR)/tests
BENCHDIR     = $(BUILDDIR)/benchmarks

# --- Pass/Plugin ---
# Ensure this matches your .cpp filename exactly!
OPTIMIZER_SOURCES = polyhedralpass.cpp
OPTIMIZER_LIBS    = $(OPTIMIZER_SOURCES:%.cpp=$(BUILDDIR)/%.so)

# --- Tests ---
TEST_SRCS    = $(wildcard tests/polyhedral-pass/*.c)
TESTS        = $(TEST_SRCS:tests/%.c=%)
TESTS_PRE    = $(TESTS:%=$(TESTDIR)/%-m2r.ll)
TESTS_POLY   = $(TESTS:%=$(TESTDIR)/%-poly.ll)
TESTS_LICM   = $(TESTS:%=$(TESTDIR)/%-licm.ll)
TESTS_LCM    = $(TESTS:%=$(TESTDIR)/%-lcm.ll)

# --- Benchmarks ---
BENCH_SRCS   = $(wildcard tests/benchmarks/*.c)
BENCHES      = $(BENCH_SRCS:tests/benchmarks/%.c=%)
BENCH_RAW    = $(BENCHES:%=$(BENCHDIR)/%-raw.bc)
BENCH_POLY   = $(BENCHES:%=$(BENCHDIR)/%-poly.bc)
BENCH_LICM   = $(BENCHES:%=$(BENCHDIR)/%-licm.bc)
BENCH_LCM    = $(BENCHES:%=$(BENCHDIR)/%-lcm.bc)

# --- Dependency Management ---
DEPFLAGS     = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.d
DEPFILES     = $(OPTIMIZER_SOURCES:%.cpp=$(DEPDIR)/%.d)

.PHONY: clean tests benchmarks analyze perf
.SECONDARY: # This ensures Make doesn't delete your intermediate .bc files

$(OPTIMIZER_LIBS): # Default target - builds the pass plugin

tests: $(TESTS_PRE) $(TESTS_POLY) $(TESTS_LICM) $(TESTS_LCM)

benchmarks: $(BENCH_RAW) $(BENCH_POLY) $(BENCH_LICM) $(BENCH_LCM)

analyze: $(OPTIMIZER_LIBS) benchmarks
	@echo "\n========== Running Analysis =========="
	./scripts/lli-compare.sh

perf: $(OPTIMIZER_LIBS) benchmarks
	@echo "\n========== Running LLI Comparison =========="
	./scripts/lli-compare.sh
	@echo "\n========== Running Perf Analysis =========="
	./scripts/perf-profile.sh

clean:
	rm -rf $(BUILDDIR)

# --- 1. Compile the Plugin ---
$(BUILDDIR)/%.o: %.cpp | $(DEPDIR) $(BUILDDIR)
	$(CXX) $(DEPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/%.so: $(BUILDDIR)/%.o
	$(CXX) -shared $^ -o $@ $(LDFLAGS)

# --- 2. The Test Pipeline ---

# Step A: C -> Raw Bitcode (Allocas/Loads/Stores)
$(TESTDIR)/%.bc: tests/%.c
	@mkdir -p $(dir $@)
	clang -O0 -ffp-contract=off -Xclang -disable-O0-optnone -fno-discard-value-names -emit-llvm -c $< -o $@

# Step B: Raw Bitcode -> SSA Bitcode (Virtual Registers & PHI nodes)
# This uses the built-in mem2reg pass
$(TESTDIR)/%-m2r.bc: $(TESTDIR)/%.bc
	@mkdir -p $(dir $@)
	opt -passes=mem2reg,loop-simplify $< -o $@

# Step C: SSA Bitcode -> Optimized Bitcode (Running your Plugin)
$(TESTDIR)/%-poly.bc: $(TESTDIR)/%-m2r.bc $(OPTIMIZER_LIBS)
	@mkdir -p $(dir $@)
	opt $(OPTIMIZER_LIBS:%=-load-pass-plugin=%) -passes='function(polyhedral-pass)' $< -o $@

$(TESTDIR)/%-licm.bc: $(TESTDIR)/%-m2r.bc
	@mkdir -p $(dir $@)
	opt -passes='function(loop-mssa(licm))' $< -o $@

$(TESTDIR)/%-lcm.bc: $(TESTDIR)/%-m2r.bc
	@mkdir -p $(dir $@)
	opt -passes='function(gvn<pre>)' $< -o $@

# Step D: Bitcode -> Human Readable IR (.ll files)
$(TESTDIR)/%-poly.ll: $(TESTDIR)/%-poly.bc
	@mkdir -p $(dir $@)
	llvm-dis $< -o $@

$(TESTDIR)/%-licm.ll: $(TESTDIR)/%-licm.bc
	@mkdir -p $(dir $@)
	llvm-dis $< -o $@

$(TESTDIR)/%-lcm.ll: $(TESTDIR)/%-lcm.bc
	@mkdir -p $(dir $@)
	llvm-dis $< -o $@

$(TESTDIR)/%-m2r.ll: $(TESTDIR)/%-m2r.bc
	@mkdir -p $(dir $@)
	llvm-dis $< -o $@

# --- 3. Benchmark Pipeline ---
$(BENCHDIR)/%.bc: tests/benchmarks/%.c
	@mkdir -p $(dir $@)
	clang -O0 -ffp-contract=off -Xclang -disable-O0-optnone -fno-discard-value-names -emit-llvm -c $< -o $@

$(BENCHDIR)/%-raw.bc: $(BENCHDIR)/%.bc
	@mkdir -p $(dir $@)
	opt -passes=mem2reg,loop-simplify $< -o $@

$(BENCHDIR)/%-poly.bc: $(BENCHDIR)/%-raw.bc $(OPTIMIZER_LIBS)
	@mkdir -p $(dir $@)
	opt $(OPTIMIZER_LIBS:%=-load-pass-plugin=%) -passes='function(polyhedral-pass)' $< -o $@

$(BENCHDIR)/%-licm.bc: $(BENCHDIR)/%-raw.bc
	@mkdir -p $(dir $@)
	opt -passes='function(loop-mssa(licm))' $< -o $@

$(BENCHDIR)/%-lcm.bc: $(BENCHDIR)/%-raw.bc
	@mkdir -p $(dir $@)
	opt -passes='function(gvn<pre>)' $< -o $@

# --- Helpers ---
$(DEPDIR) $(BUILDDIR) $(TESTDIR) $(BENCHDIR):
	@mkdir -p $@

-include $(wildcard $(DEPFILES))
