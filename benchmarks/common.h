#ifndef POLY_BENCH_COMMON_H
#define POLY_BENCH_COMMON_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef TILE_SIZE
#define TILE_SIZE 32
#endif

#ifndef REPEAT
#define REPEAT 4
#endif

#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define MAX2(a, b) ((a) > (b) ? (a) : (b))

static void *checked_malloc(size_t Size) {
  void *Ptr = malloc(Size);
  if (Ptr == NULL) {
    fprintf(stderr, "allocation failed for %zu bytes\n", Size);
    exit(1);
  }
  return Ptr;
}

static double checksum_buffer(const double *Buffer, size_t Count) {
  double Accumulator = 0.0;
  for (size_t I = 0; I < Count; ++I) {
    Accumulator += Buffer[I] * (double)((I % 17) + 1);
  }
  return Accumulator;
}

#endif
