#define N 1024

void mixed(double A[N][N], double B[N][N]) {
  for (int i = 1; i < N-2; i++) {
    for (int j = 2; j < N-1; j++) {
      B[i][j] = A[i][j] +
                A[i][j-1] +
                A[i-1][j+1];
    }
  }
}