#define N 1024

void false_dep(double A[N][N]) {
  for (int i = 1; i < N; i++) {
    for (int j = 1; j < N; j++) {
      double tmp = A[i][j-1];
      A[i][j] = tmp;
    }
  }
}