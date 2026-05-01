#define N 1024

void transpose(double A[N][N], double B[N][N]) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            B[j][i] = A[i][j];
}