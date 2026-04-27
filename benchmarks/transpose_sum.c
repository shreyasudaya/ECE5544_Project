#include "common.h"

#define N 1536

typedef double matrix[N][N];

void __poly_ref_transpose_sum(double A[N][N], double B[N][N]) {
  for (int j = 0; j < N; ++j) {
    for (int i = 0; i < N; ++i) {
      B[j][i] = A[i][j] + 1.0;
    }
  }
}

void transpose_sum(double A[N][N], double B[N][N]) {
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      B[j][i] = A[i][j] + 1.0;
    }
  }
}

int main(void) {
  matrix *A = checked_malloc(sizeof(matrix));
  matrix *B = checked_malloc(sizeof(matrix));

  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      (*A)[i][j] = (double)((i * 13 + j * 3 + 17) % 127) / 127.0;
      (*B)[i][j] = 0.0;
    }
  }

  for (int RepeatIndex = 0; RepeatIndex < REPEAT; ++RepeatIndex) {
    transpose_sum(*A, *B);
  }

  printf("transpose_sum checksum=%0.6f tile=%d\n",
         checksum_buffer(&(*B)[0][0], (size_t)N * N), TILE_SIZE);

  free(A);
  free(B);
  return 0;
}
