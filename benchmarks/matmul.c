#include "common.h"

#define N 256

typedef double matrix[N][N];

void __poly_ref_matmul(double A[N][N], double B[N][N], double C[N][N]) {
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      C[i][j] = 0.0;
    }
  }

  for (int ii = 0; ii < N; ii += TILE_SIZE) {
    for (int jj = 0; jj < N; jj += TILE_SIZE) {
      for (int kk = 0; kk < N; kk += TILE_SIZE) {
        for (int i = ii; i < MIN2(ii + TILE_SIZE, N); ++i) {
          for (int j = jj; j < MIN2(jj + TILE_SIZE, N); ++j) {
            double Sum = C[i][j];
            for (int k = kk; k < MIN2(kk + TILE_SIZE, N); ++k) {
              Sum += A[i][k] * B[k][j];
            }
            C[i][j] = Sum;
          }
        }
      }
    }
  }
}

void matmul(double A[N][N], double B[N][N], double C[N][N]) {
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      double Sum = 0.0;
      for (int k = 0; k < N; ++k) {
        Sum += A[i][k] * B[k][j];
      }
      C[i][j] = Sum;
    }
  }
}

int main(void) {
  matrix *A = checked_malloc(sizeof(matrix));
  matrix *B = checked_malloc(sizeof(matrix));
  matrix *C = checked_malloc(sizeof(matrix));

  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      (*A)[i][j] = (double)((i + 3 * j) % 97) / 97.0;
      (*B)[i][j] = (double)((2 * i - j + 101) % 89) / 89.0;
      (*C)[i][j] = 0.0;
    }
  }

  for (int RepeatIndex = 0; RepeatIndex < REPEAT; ++RepeatIndex) {
    matmul(*A, *B, *C);
  }

  printf("matmul checksum=%0.6f tile=%d\n",
         checksum_buffer(&(*C)[0][0], (size_t)N * N), TILE_SIZE);

  free(A);
  free(B);
  free(C);
  return 0;
}
