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

# --- Pass/Plugin ---
# Ensure this matches your .cpp filename exactly!
OPTIMIZER_SOURCES = polyhedralpass.cpp
OPTIMIZER_LIBS    = $(OPTIMIZER_SOURCES:%.cpp=$(BUILDDIR)/%.so)

# --- Tests ---
TEST_SRCS    = $(wildcard tests/*.c)
TESTS        = $(TEST_SRCS:tests/%.c=%)
TESTS_PRE    = $(TESTS:%=$(TESTDIR)/%-m2r.ll)
TESTS_OUT    = $(TESTS:%=$(TESTDIR)/%-opt.ll)

# --- Dependency Management ---
DEPFLAGS     = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.d
DEPFILES     = $(OPTIMIZER_SOURCES:%.cpp=$(DEPDIR)/%.d)

.PHONY: all clean tests
.SECONDARY: # This ensures Make doesn't delete your intermediate .bc files

all: $(OPTIMIZER_LIBS)

tests: $(TESTS_PRE) $(TESTS_OUT)

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
	clang -O0 -Xclang -disable-O0-optnone -fno-discard-value-names -emit-llvm -c $< -o $@

# Step B: Raw Bitcode -> SSA Bitcode (Virtual Registers & PHI nodes)
# This uses the built-in mem2reg pass
$(TESTDIR)/%-m2r.bc: $(TESTDIR)/%.bc
	@mkdir -p $(dir $@)
	opt -passes=mem2reg,loop-simplify $< -o $@

# Step C: SSA Bitcode -> Optimized Bitcode (Running your Plugin)
$(TESTDIR)/%-opt.bc: $(TESTDIR)/%-m2r.bc $(OPTIMIZER_LIBS)
	@mkdir -p $(dir $@)
	$(eval PASS := $(patsubst %/,%,$(dir $*)))
	opt $(OPTIMIZER_LIBS:%=-load-pass-plugin=%) -passes='$(PASS)' $< -o $@

# Step D: Bitcode -> Human Readable IR (.ll files)
$(TESTDIR)/%-opt.ll: $(TESTDIR)/%-opt.bc
	@mkdir -p $(dir $@)
	llvm-dis $< -o $@

$(TESTDIR)/%-m2r.ll: $(TESTDIR)/%-m2r.bc
	@mkdir -p $(dir $@)
	llvm-dis $< -o $@

# --- Helpers ---
$(DEPDIR) $(BUILDDIR) $(TESTDIR):
	@mkdir -p $@

-include $(wildcard $(DEPFILES))