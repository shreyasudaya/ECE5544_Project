#include <stdio.h>
#include <stdlib.h>

#define N 512

// The Kernel: This is what your Polyhedral Pass will transform
void matmul(double A[N][N], double B[N][N], double C[N][N]) {
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            C[i][j] = 0.0;
            for (int k = 0; k < N; k++) {
                C[i][j] += A[i][k] * B[k][j];
            }
        }
    }
}

int main() {
    // Allocate on heap to avoid stack overflow at larger N
    typedef double matrix[N][N];
    matrix *A = malloc(sizeof(matrix));
    matrix *B = malloc(sizeof(matrix));
    matrix *C = malloc(sizeof(matrix));

    if (!A || !B || !C) return 1;

    // Initialize matrices
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            (*A)[i][j] = (double)(i + j);
            (*B)[i][j] = (double)(i - j);
            (*C)[i][j] = 0.0;
        }
    }

    // Actual workload
    matmul(*A, *B, *C);

    // Prevent Dead Code Elimination: print a result
    printf("Done. Sample result: %f\n", (*C)[N-1][N-1]);

    free(A);
    free(B);
    free(C);
    return 0;
}