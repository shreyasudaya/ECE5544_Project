#include <stdio.h>

#define N 192

static double A[N][N];
static double B[N][N];

static void transpose_sum(double A[N][N], double B[N][N]) {
  for (int j = 0; j < N; ++j) {
    for (int i = 0; i < N; ++i) {
      B[i][j] = A[i][j] + 3.14;
    }
  }
}

int main(void) {
  for (int i = 0; i < N; ++i) {
    for (int j = 0; j < N; ++j) {
      A[i][j] = (double)((i - 2 * j) % 31);
      B[i][j] = 0.0;
    }
  }

  transpose_sum(A, B);
  printf("%.3f\n", B[N - 2][N - 2]);
  return 0;
}
