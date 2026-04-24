#define N 1024

void blocked_ref(double A[N][N], double B[N][N]) {
  for (int i = 0; i < N; i += 32) {
    for (int j = 0; j < N; j += 32) {
      for (int ii = i; ii < i + 32 && ii < N; ii++) {
        for (int jj = j; jj < j + 32 && jj < N; jj++) {
          B[ii][jj] = A[ii][jj] * 2;
        }
      }
    }
  }
}