#define N 2048

void transpose_sum(double A[N][N], double B[N][N]) {
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < N; j++) {
      B[j][i] = A[i][j] + 1.0;
    }
  }
}