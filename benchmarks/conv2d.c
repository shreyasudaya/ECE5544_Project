#include "common.h"

#define N 1024

typedef double matrix[N][N];

void __poly_ref_conv2d(double A[N][N], double B[N][N]) {
  for (int ii = 1; ii < N - 1; ii += TILE_SIZE) {
    for (int jj = 1; jj < N - 1; jj += TILE_SIZE) {
      for (int i = ii; i < MIN2(ii + TILE_SIZE, N - 1); ++i) {
        for (int j = jj; j < MIN2(jj + TILE_SIZE, N - 1); ++j) {
          B[i][j] = 0.0625 * A[i - 1][j - 1] + 0.125 * A[i - 1][j] +
                    0.0625 * A[i - 1][j + 1] + 0.125 * A[i][j - 1] +
                    0.25 * A[i][j] + 0.125 * A[i][j + 1] +
                    0.0625 * A[i + 1][j - 1] + 0.125 * A[i + 1][j] +
                    0.0625 * A[i + 1][j + 1];
        }
      }
    }
  }
}

void conv2d(double A[N][N], double B[N][N]) {
  for (int i = 1; i < N - 1; ++i) {
    for (int j = 1; j < N - 1; ++j) {
      B[i][j] = 0.0625 * A[i - 1][j - 1] + 0.125 * A[i - 1][j] +
                0.0625 * A[i - 1][j + 1] + 0.125 * A[i][j - 1] +
                0.25 * A[i][j] + 0.125 * A[i][j + 1] +
                0.0625 * A[i + 1][j - 1] + 0.125 * A[i + 1][j] +
                0.0625 * A[i + 1][j + 1];
    }
  }
}

int main(void) {
  matrix *A = checked_malloc(sizeof(matrix));
  matrix *B = checked_malloc(sizeof(matrix));

  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      (*A)[i][j] = (double)((i * 5 + j * 7 + 13) % 101) / 101.0;
      (*B)[i][j] = 0.0;
    }
  }

  for (int RepeatIndex = 0; RepeatIndex < REPEAT; ++RepeatIndex) {
    conv2d(*A, *B);
  }

  printf("conv2d checksum=%0.6f tile=%d\n",
         checksum_buffer(&(*B)[0][0], (size_t)N * N), TILE_SIZE);

  free(A);
  free(B);
  return 0;
}
