#define N 2048

void column_access(double A[N][N], double B[N][N]) {
  for (int j = 0; j < N; j++) {
    for (int i = 0; i < N; i++) {
      B[i][j] = A[i][j] + 3.14;
    }
  }
}