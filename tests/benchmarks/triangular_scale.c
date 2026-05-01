#define N 512

void triangular(double A[N][N]) {
    for (int i = 1; i < N; i++) {
        for (int j = 1; j <= i; j++) {
            A[i][j] = A[i][j] + A[i-1][j] + A[i][j-1];
        }
    }
}