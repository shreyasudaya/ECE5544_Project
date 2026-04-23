# Polyhedral Loop Transformations for Cache Locality

## Overview

This project implements a small polyhedral optimization pass for affine loop nests in LLVM. The goal is to improve cache locality by applying a fixed set of legal loop transformations instead of building a full polyhedral optimizer such as Pluto.

The pass targets static-control, perfectly nested loops and focuses on two transformations:

- loop interchange
- loop tiling

The implementation treats these transformations as affine schedule changes and applies them only when dependence constraints are preserved.

## Objective

The main objective is to generalize the earlier loop optimization ideas from LICM and LCM into the polyhedral setting.

For each candidate loop nest, the pass should:

1. identify affine loop bounds and affine memory accesses
2. build a compact polyhedral representation using:
   - an iteration domain
   - access functions
   - dependence relations
3. test whether a proposed affine schedule transformation is legal
4. rewrite the loop nest in IR when the transformation is valid

This project does not attempt full automatic schedule synthesis. Instead, it evaluates a small, fixed family of profitable schedule changes for locality.

## Scope

### Supported nests

- perfectly nested loops
- affine loop bounds
- affine array subscripts
- static control flow
- dense array kernels such as matrix multiply, stencil, transpose, and convolution-style code

### Unsupported cases

- non-affine bounds or subscripts
- irregular control flow inside the nest
- pointer aliasing that cannot be resolved conservatively
- general while-loops or data-dependent loop limits
- arbitrary fusion, skewing, or Pluto-style schedule search

## Polyhedral Representation

Each loop nest is modeled with three core pieces of information.

### 1. Iteration domain

The iteration domain represents all dynamic instances of the statements in the loop nest. For a nest such as:

```c
for (int i = 0; i < N; i++) {
  for (int j = 0; j < M; j++) {
    S(i, j);
  }
}
```

the iteration domain is:

```text
D_S = { (i, j) | 0 <= i < N and 0 <= j < M }
```

### 2. Access functions

Each memory reference is represented as an affine map from iteration points to array elements. For example:

```c
A[i][j]
A[i][j - 1]
B[j][i]
```

becomes:

```text
f_A(i, j)     = (i, j)
f_left(i, j)  = (i, j - 1)
f_B(i, j)     = (j, i)
```

### 3. Dependence relations

A transformation is legal only if it respects the original program dependences. For any source iteration `x` and sink iteration `y`, we require the transformed schedule to satisfy:

```text
theta(x) < theta(y)
```

for all true dependences.

In practice, this project can use either:

- `isl` for dependence and legality checks, or
- a lightweight custom solver for the restricted affine cases in the test suite

## Transformations

### Loop interchange

Loop interchange swaps the execution order of two nested loops. This is useful when the original order causes poor spatial locality, such as column-major traversal over row-major arrays.

Example:

```c
for (int j = 0; j < N; j++) {
  for (int i = 0; i < N; i++) {
    B[i][j] = A[i][j] + 3.14;
  }
}
```

can be rewritten as:

```c
for (int i = 0; i < N; i++) {
  for (int j = 0; j < N; j++) {
    B[i][j] = A[i][j] + 3.14;
  }
}
```

when dependence analysis confirms that the swap is legal.

### Loop tiling

Loop tiling improves temporal and spatial locality by executing the iteration space in blocks that fit better in cache.

Example:

```c
for (int i = 0; i < N; i++) {
  for (int j = 0; j < N; j++) {
    B[i][j] = A[i][j] * 2.0;
  }
}
```

can be tiled into:

```c
for (int ii = 0; ii < N; ii += T) {
  for (int jj = 0; jj < N; jj += T) {
    for (int i = ii; i < min(ii + T, N); i++) {
      for (int j = jj; j < min(jj + T, N); j++) {
        B[i][j] = A[i][j] * 2.0;
      }
    }
  }
}
```

The tile size `T` is exposed as a tuning parameter for the evaluation.

## Legality Checking

The pass should reject any transformation that violates dependences.

### Interchange legality

Interchange is legal only if no carried dependence is reversed by swapping loop order. For example:

- `recurrence`-style loops with dependencies on `A[i-1][j]` or `A[i][j-1]` may restrict or forbid interchange
- pure elementwise kernels are typically safe to interchange

### Tiling legality

Tiling preserves the original lexicographic order within each transformed schedule. For affine nests, legality is checked by verifying that the tiled schedule still satisfies all dependence constraints.

### Conservative fallback

If the pass cannot prove legality, it must leave the loop nest unchanged.

## Implementation Plan

The LLVM pass can be organized into the following stages.

### 1. Detect candidate loop nests

- use `LoopInfo` to find loop nests
- keep only perfectly nested affine loops
- identify the induction variables, bounds, and step sizes

### 2. Extract memory accesses

- inspect loads and stores inside the innermost body
- recover affine access expressions relative to loop IVs
- group accesses by base array object

### 3. Build a restricted polyhedral model

- construct iteration domains from loop bounds
- represent accesses as affine maps
- compute dependence directions or dependence polyhedra

### 4. Evaluate fixed transformations

- try loop interchange on two-deep or three-deep nests
- try tiling for selected dimensions and tile sizes
- reject any candidate that fails legality

### 5. Rewrite IR

- generate transformed loop structure
- remap induction variables
- preserve the original statement body semantics

## Suggested Repository Structure

- `polyhedralpass.cpp`: LLVM pass implementation
- `polyhedralpass.h`: pass declarations and helper data structures
- `tests/`: affine kernels used for legality and locality experiments

The current test set already contains several useful cases:

- `test1.c`: matrix multiplication
- `test2.c`: 2D stencil
- `test3.c`: transpose-like access pattern
- `test4.c`: recurrence with loop-carried dependences
- `test5.c`: independent elementwise kernel
- `test6.c`: poor locality due to column-first traversal
- `test7.c`: mixed affine offsets
- `test8.c`: pre-blocked reference implementation
- `test9.c`: simple dependence through neighboring elements
- `test10.c`: iterative stencil / heat update

## Evaluation

### Baselines

- untransformed loop nest
- LICM-optimized version from the earlier homework

### Benchmarks

Use dense kernels inspired by Polybench, including:

- matrix multiplication
- LU-style or triangular update kernels
- 2D convolution
- stencil or heat diffusion kernels

The included tests can serve as functional microbenchmarks, while full Polybench kernels are better for performance measurements.

### Metrics

Measure:

- L1 data cache misses
- L2 data cache misses
- total runtime
- speedup relative to baseline

On Linux, hardware counter collection can be done with `perf`, for example:

```bash
perf stat -e cache-references,cache-misses,L1-dcache-load-misses ./benchmark
```

### Sensitivity study

For tiled kernels, sweep several tile sizes and report:

- cache miss rate vs. tile size
- runtime vs. tile size
- best-performing tile size per benchmark

This demonstrates the locality/performance tradeoff and shows that larger tiles are not always better.

## Expected Outcomes

A successful submission should demonstrate:

- correct identification of affine, perfectly nested loops
- conservative legality checking based on dependence preservation
- working loop interchange and loop tiling transformations
- measurable locality improvements on at least some dense kernels
- a clear discussion of cases where the pass declines to transform

## Limitations

This project intentionally stays small and educational. It is not a full polyhedral compiler. In particular, it does not include:

- full schedule optimization
- fusion or skewing
- automatic cost-model-driven search over all schedules
- sophisticated alias analysis beyond conservative checks

## Deliverables

The final project should include:

- the LLVM pass implementation
- a short explanation of the legality analysis
- transformed examples or IR before/after snapshots
- performance data for the chosen kernels
- a brief discussion of tile-size sensitivity and observed cache behavior
