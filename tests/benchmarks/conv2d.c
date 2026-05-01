#define N 512
#define K 5

void conv2d(double A[N][N], double B[N][N], double KERNEL[K][K]) {
    for (int i = 2; i < N-2; i++)
        for (int j = 2; j < N-2; j++) {
            double sum = 0;
            for (int ki = 0; ki < K; ki++)
                for (int kj = 0; kj < K; kj++)
                    sum += A[i+ki-2][j+kj-2] * KERNEL[ki][kj];
            B[i][j] = sum;
        }
}