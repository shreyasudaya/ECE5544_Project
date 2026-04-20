#define N 2048

void independent(double A[N][N], double B[N][N]) {
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < N; j++) {
      B[i][j] = A[i][j] * 2.0 + 1.0;
    }
  }
}