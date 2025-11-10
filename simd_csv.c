// simd_csv.c
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void simd_count_commas(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) { perror("open"); return; }

    const int BUF_SIZE = 1 << 16;
    char *buf = aligned_alloc(32, BUF_SIZE);
    size_t total = 0, commas = 0;

    while (!feof(f)) {
        size_t n = fread(buf, 1, BUF_SIZE, f);
        total += n;
        size_t i = 0;
        for (; i + 32 <= n; i += 32) {
            __m256i data = _mm256_loadu_si256((__m256i*)(buf + i));
            __m256i comma = _mm256_set1_epi8(',');
            __m256i eq = _mm256_cmpeq_epi8(data, comma);
            int mask = _mm256_movemask_epi8(eq);
            commas += __builtin_popcount(mask);
        }
        // tail
        for (; i < n; ++i)
            if (buf[i] == ',') commas++;
    }

    fclose(f);
    free(buf);
    printf("Processed %zu bytes, found %zu commas\n", total, commas);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s file.csv\n", argv[0]);
        return 1;
    }
    simd_count_commas(argv[1]);
    return 0;
}

