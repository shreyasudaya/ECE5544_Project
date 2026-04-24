#define N 1024

void stencil(double A[N][N], double B[N][N]) {
  for (int i = 1; i < N-1; i++) {
    for (int j = 1; j < N-1; j++) {
      B[i][j] = (A[i][j] +
                 A[i-1][j] +
                 A[i+1][j] +
                 A[i][j-1] +
                 A[i][j+1]) * 0.2;
    }
  }
}