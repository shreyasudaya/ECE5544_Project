#define N 512
#define T 10

void stencil(double A[N][N], double B[N][N]) {
    for (int t = 0; t < T; t++) {
        for (int i = 1; i < N-1; i++) {
            for (int j = 1; j < N-1; j++) {
                B[i][j] = 0.25 * (A[i-1][j] + A[i+1][j]
                                + A[i][j-1] + A[i][j+1]);
            }
        }

        // swap
        for (int i = 1; i < N-1; i++)
            for (int j = 1; j < N-1; j++)
                A[i][j] = B[i][j];
    }
}