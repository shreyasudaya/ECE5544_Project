#include "common.h"

#define N 1536

typedef double matrix[N][N];
typedef double vector[N];

void __poly_ref_triangular_scale(double A[N][N], double B[N][N], double D[N]) {
  for (int ii = 0; ii < N; ii += TILE_SIZE) {
    for (int jj = 0; jj < N; jj += TILE_SIZE) {
      for (int i = ii; i < MIN2(ii + TILE_SIZE, N); ++i) {
        const int JStart = MAX2(i, jj);
        for (int j = JStart; j < MIN2(jj + TILE_SIZE, N); ++j) {
          B[i][j] = A[i][j] * D[j];
        }
      }
    }
  }
}

void triangular_scale(double A[N][N], double B[N][N], double D[N]) {
  for (int i = 0; i < N; ++i) {
    for (int j = i; j < N; ++j) {
      B[i][j] = A[i][j] * D[j];
    }
  }
}

int main(void) {
  matrix *A = checked_malloc(sizeof(matrix));
  matrix *B = checked_malloc(sizeof(matrix));
  vector *D = checked_malloc(sizeof(vector));

  for (int i = 0; i < N; ++i) {
    (*D)[i] = 1.0 + (double)((i * 7 + 19) % 31) / 31.0;
    for (int j = 0; j < N; ++j) {
      (*A)[i][j] = (double)((i * 17 + j * 5 + 23) % 137) / 137.0;
      (*B)[i][j] = 0.0;
    }
  }

  for (int RepeatIndex = 0; RepeatIndex < REPEAT; ++RepeatIndex) {
    triangular_scale(*A, *B, *D);
  }

  printf("triangular_scale checksum=%0.6f tile=%d\n",
         checksum_buffer(&(*B)[0][0], (size_t)N * N), TILE_SIZE);

  free(A);
  free(B);
  free(D);
  return 0;
}
