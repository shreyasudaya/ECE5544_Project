# Polyhedral Loop Transformations for Cache Locality

## Overview

This project implements a small LLVM loop pass for affine, perfectly nested loop nests and evaluates it on dense kernels with cache-sensitive access patterns.

The pass focuses on a fixed family of locality transformations:

- loop interchange for row/column order mismatches
- loop tiling for rectangular and triangular iteration spaces

The implementation is intentionally conservative. It recognizes affine loop nests, extracts affine-style memory subscripts, rejects same-base loop-carried dependences it cannot safely preserve, and only rewrites kernels when a legal transformed schedule is available.

## What The Pass Does

For each candidate function, the pass:

1. finds perfectly nested loops with canonical induction variables
2. extracts affine-like bounds and array subscripts from the innermost body
3. performs a conservative legality check
4. chooses either interchange or tiling based on the access pattern
5. rewrites the original function to call a matching transformed helper

The helper lookup convention is:

```text
__poly_ref_<original_function_name>
```

This keeps the project small and deterministic while still giving a real IR rewrite and meaningful runtime baselines for supported kernels.

## Supported Cases

- perfectly nested `for`-style loops
- affine or affine-like integer bounds and GEP indices
- dense kernels with static control flow
- functions with a legal transformed helper in the same module

## Conservative Rejections

The pass leaves the loop nest unchanged when it detects or cannot rule out:

- same-array read/write dependences with shifted subscripts
- non-elementwise same-base accesses
- nests shallower than two loops
- unsupported or missing helper-backed transformed variants

That means cases such as in-place stencils, recurrences, and neighbor-copy loops are intentionally rejected unless the pass can prove they are safe.

## Repository Layout

- `polyhedralpass.cpp`: LLVM pass implementation
- `benchmarks/`: executable benchmark kernels with `raw` code plus `__poly_ref_*` transformed helpers
- `tests/polyhedral-pass/`: small functional kernels used for IR-level inspection
- `scripts/lli-compare.sh`: instruction-count comparison for `raw`, `licm`, and `poly`
- `scripts/perf-profile.sh`: runtime and cache-miss comparison for `raw`, `licm`, and `poly`
- `scripts/tile-sweep.sh`: tile-size sweep for helper-backed tiled kernels

## Benchmarks

The benchmark set includes:

- matrix multiplication
- 2D convolution
- 2D stencil
- transpose-like access
- triangular upper-region scaling

These are built as standalone executables so they can be profiled directly with `perf`.

## Baselines

Every benchmark is built in three variants:

- `raw`: original kernel, no LICM or polyhedral pass
- `licm`: original kernel after `mem2reg`, `loop-simplify`, and `licm`
- `poly`: original kernel after `mem2reg`, `loop-simplify`, `licm`, and `polyhedral-pass`

This gives the two required baselines:

- untransformed code
- LICM-only code

and a third comparison point for the polyhedral transformation itself.

## Metrics

The analysis scripts report:

- interpreted instruction count
- elapsed runtime
- L1 data cache load misses
- total cache misses
- speedup of `poly` relative to `raw`
- speedup of `poly` relative to `licm`

`cache-misses` is used as the portable second-level cache proxy in the provided scripts because exact L2 event names vary across machines.

## Tile-Size Sensitivity Study

Tiled benchmarks use the compile-time parameter:

```text
TILE_SIZE
```

The sweep script rebuilds the benchmark suite for several tile sizes, records runtime and cache misses, and reports the best-performing tile per benchmark.

## How To Run

### Build the Docker image

```bash
docker-compose build
```

### Start the container

```bash
docker-compose run compiler-env
```

### Build the pass

```bash
make
```

### Build the IR inspection tests

```bash
make tests
```

This generates:

- `build/tests/.../*-m2r.ll`
- `build/tests/.../*-opt.ll`

### Build the benchmark baselines

```bash
make benchmarks
```

This produces executables and bitcode under:

```text
build/benchmarks/tile-32/
```

### Compare instruction counts

```bash
make analyze
```

### Run runtime and cache profiling

```bash
make perf
```

### Sweep tile sizes

```bash
make sweep
```

You can also choose a tile size manually:

```bash
make benchmarks TILE_SIZE=64
make analyze TILE_SIZE=64
make perf TILE_SIZE=64
```

## Expected Outcomes

A successful run should demonstrate:

- correct identification of affine, perfectly nested loops
- conservative legality checking based on dependence preservation
- loop interchange on transpose-like kernels
- loop tiling on stencil-, convolution-, triangular-, and matrix-multiply-style kernels
- measurable locality improvements on at least some dense kernels
- explicit rejection of unsafe same-base dependence patterns

## Limitations

This is still a small educational project, not a full polyhedral optimizer. In particular, it does not include:

- full schedule synthesis
- arbitrary IR-level loop reconstruction from dependence polyhedra
- fusion, skewing, or Pluto-style search
- aggressive alias reasoning
- automatic transformed helper generation

For now, the pass only rewrites functions that have a matching `__poly_ref_*` transformed helper in the same module. Unsupported functions are analyzed and reported, but left unchanged.
