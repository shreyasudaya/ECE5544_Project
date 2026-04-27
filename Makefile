CXX ?= clang++
CLANG ?= clang
OPT ?= opt
LLVM_DIS ?= llvm-dis
LLI ?= lli
LLVM_CONFIG ?= llvm-config

TILE_SIZE ?= 32
REPEAT ?= 4

CXXFLAGS = -rdynamic $(shell $(LLVM_CONFIG) --cxxflags) -fPIC -g -std=c++20
LDFLAGS = $(shell $(LLVM_CONFIG) --ldflags | tr '\n' ' ') -Wl,--exclude-libs,ALL
C_EMIT_FLAGS = -O0 -Xclang -disable-O0-optnone -fno-discard-value-names -emit-llvm -c

BUILDDIR = build
DEPDIR = $(BUILDDIR)/.deps
TESTDIR = $(BUILDDIR)/tests
BENCHDIR = $(BUILDDIR)/benchmarks/tile-$(TILE_SIZE)
REPORTDIR = $(BUILDDIR)/reports

OPTIMIZER_SOURCES = polyhedralpass.cpp
OPTIMIZER_LIBS = $(OPTIMIZER_SOURCES:%.cpp=$(BUILDDIR)/%.so)

TEST_SRCS = $(wildcard tests/*/*.c)
TESTS = $(TEST_SRCS:tests/%.c=%)
TESTS_RAW_BC = $(TESTS:%=$(TESTDIR)/%.bc)
TESTS_SSA_BC = $(TESTS:%=$(TESTDIR)/%-m2r.bc)
TESTS_SSA_LL = $(TESTS:%=$(TESTDIR)/%-m2r.ll)
TESTS_OPT_BC = $(TESTS:%=$(TESTDIR)/%-opt.bc)
TESTS_OPT_LL = $(TESTS:%=$(TESTDIR)/%-opt.ll)

BENCH_SRCS = $(wildcard benchmarks/*.c)
BENCH_NAMES = $(basename $(notdir $(BENCH_SRCS)))
BENCH_RAW_BC = $(BENCH_NAMES:%=$(BENCHDIR)/%-raw.bc)
BENCH_LICM_BC = $(BENCH_NAMES:%=$(BENCHDIR)/%-licm.bc)
BENCH_POLY_BC = $(BENCH_NAMES:%=$(BENCHDIR)/%-poly.bc)
BENCH_RAW_EXE = $(BENCH_NAMES:%=$(BENCHDIR)/%-raw)
BENCH_LICM_EXE = $(BENCH_NAMES:%=$(BENCHDIR)/%-licm)
BENCH_POLY_EXE = $(BENCH_NAMES:%=$(BENCHDIR)/%-poly)

DEPFLAGS = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.d
DEPFILES = $(OPTIMIZER_SOURCES:%.cpp=$(DEPDIR)/%.d)

LICM_PIPELINE = mem2reg,loop-simplify,loop-mssa(licm)
POLY_PIPELINE = mem2reg,loop-simplify,loop-mssa(licm),polyhedral-pass

.PHONY: all clean tests benchmarks analyze perf sweep
.SECONDARY:

all: $(OPTIMIZER_LIBS)

tests: $(TESTS_SSA_LL) $(TESTS_OPT_LL)

benchmarks: $(BENCH_RAW_EXE) $(BENCH_LICM_EXE) $(BENCH_POLY_EXE)

analyze: benchmarks
	@mkdir -p $(REPORTDIR)
	bash ./scripts/lli-compare.sh $(BENCHDIR) | tee $(REPORTDIR)/lli-tile-$(TILE_SIZE).txt

perf: benchmarks
	@mkdir -p $(REPORTDIR)
	bash ./scripts/perf-profile.sh $(BENCHDIR) | tee $(REPORTDIR)/perf-tile-$(TILE_SIZE).txt

sweep: $(OPTIMIZER_LIBS)
	@mkdir -p $(REPORTDIR)
	bash ./scripts/tile-sweep.sh

clean:
	rm -rf $(BUILDDIR)

$(BUILDDIR)/%.o: %.cpp | $(DEPDIR) $(BUILDDIR)
	$(CXX) $(DEPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/%.so: $(BUILDDIR)/%.o
	$(CXX) -shared $^ -o $@ $(LDFLAGS)

$(TESTDIR)/%.bc: tests/%.c | $(TESTDIR)
	@mkdir -p $(dir $@)
	$(CLANG) $(C_EMIT_FLAGS) $< -o $@

$(TESTDIR)/%-m2r.bc: $(TESTDIR)/%.bc
	@mkdir -p $(dir $@)
	$(OPT) -passes='mem2reg,loop-simplify' $< -o $@

$(TESTDIR)/%-opt.bc: $(TESTDIR)/%-m2r.bc $(OPTIMIZER_LIBS)
	@mkdir -p $(dir $@)
	$(eval PASS := $(patsubst %/,%,$(dir $*)))
	$(OPT) $(OPTIMIZER_LIBS:%=-load-pass-plugin=%) -passes='$(PASS)' $< -o $@

$(TESTDIR)/%-m2r.ll: $(TESTDIR)/%-m2r.bc
	@mkdir -p $(dir $@)
	$(LLVM_DIS) $< -o $@

$(TESTDIR)/%-opt.ll: $(TESTDIR)/%-opt.bc
	@mkdir -p $(dir $@)
	$(LLVM_DIS) $< -o $@

$(BENCHDIR)/%-raw.bc: benchmarks/%.c benchmarks/common.h | $(BENCHDIR)
	$(CLANG) $(C_EMIT_FLAGS) -DTILE_SIZE=$(TILE_SIZE) -DREPEAT=$(REPEAT) $< -o $@

$(BENCHDIR)/%-licm.bc: $(BENCHDIR)/%-raw.bc
	$(OPT) -passes='$(LICM_PIPELINE)' $< -o $@

$(BENCHDIR)/%-poly.bc: $(BENCHDIR)/%-raw.bc $(OPTIMIZER_LIBS)
	$(OPT) $(OPTIMIZER_LIBS:%=-load-pass-plugin=%) -passes='$(POLY_PIPELINE)' $< -o $@

$(BENCHDIR)/%-raw: $(BENCHDIR)/%-raw.bc
	$(CLANG) -O0 $< -o $@

$(BENCHDIR)/%-licm: $(BENCHDIR)/%-licm.bc
	$(CLANG) -O0 $< -o $@

$(BENCHDIR)/%-poly: $(BENCHDIR)/%-poly.bc
	$(CLANG) -O0 $< -o $@

$(DEPDIR) $(BUILDDIR) $(TESTDIR) $(BENCHDIR) $(REPORTDIR):
	@mkdir -p $@

-include $(wildcard $(DEPFILES))
