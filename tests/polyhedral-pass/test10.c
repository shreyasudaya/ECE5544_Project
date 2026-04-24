#define N 1024

void heat(double A[N][N]) {
  for (int t = 0; t < 50; t++) {
    for (int i = 1; i < N-1; i++) {
      for (int j = 1; j < N-1; j++) {
        A[i][j] = (A[i][j] +
                   A[i-1][j] +
                   A[i+1][j] +
                   A[i][j-1] +
                   A[i][j+1]) * 0.25;
      }
    }
  }
}