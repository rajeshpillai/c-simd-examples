// simd_csv.c
// SIMD (AVX2) comma counter + scalar CSV parser with same CLI options as nosimd version
// Build (x86-64, AVX2 enabled): gcc -O3 -std=c11 -mavx2 simd_csv.c -o simd_csv
// Build (portable; still runs, will auto-fallback if no AVX2 at runtime):
//   gcc -O3 -std=c11 simd_csv.c -o simd_csv
//
// Usage:
//   Count commas only:           ./simd_csv file.csv --count-commas
//   Parse & stats only:          ./simd_csv file.csv
//   Parse & print first N rows:  ./simd_csv file.csv --print --limit 5

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
  #define PLATFORM_X86 1
  #include <immintrin.h>
#else
  #define PLATFORM_X86 0
#endif

// -----------------------------
// Small dynamic string vector
// -----------------------------
typedef struct {
    char **items;
    size_t len;
    size_t cap;
} StrVec;

static void sv_init(StrVec *v) {
    v->items = NULL; v->len = 0; v->cap = 0;
}
static void sv_push(StrVec *v, char *s) {
    if (v->len == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 8;
        char **nitems = (char**)realloc(v->items, ncap * sizeof(char*));
        if (!nitems) { perror("realloc"); exit(1); }
        v->items = nitems; v->cap = ncap;
    }
    v->items[v->len++] = s;
}
static void sv_free_with_items(StrVec *v) {
    for (size_t i = 0; i < v->len; ++i) free(v->items[i]);
    free(v->items);
    v->items = NULL; v->len = v->cap = 0;
}

// -----------------------------
// Small growable string buffer
// -----------------------------
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf *b) { b->buf = NULL; b->len = 0; b->cap = 0; }
static void sb_clear(StrBuf *b) { b->len = 0; }
static void sb_reserve(StrBuf *b, size_t need) {
    if (need <= b->cap) return;
    size_t ncap = b->cap ? b->cap * 2 : 64;
    while (ncap < need) ncap *= 2;
    char *nbuf = (char*)realloc(b->buf, ncap);
    if (!nbuf) { perror("realloc"); exit(1); }
    b->buf = nbuf; b->cap = ncap;
}
static void sb_pushc(StrBuf *b, char c) {
    sb_reserve(b, b->len + 2);
    b->buf[b->len++] = c;
    b->buf[b->len] = '\0';
}
static char* sb_take_cstr(StrBuf *b) {
    // ensure null-termination and hand over ownership
    sb_pushc(b, '\0');
    b->len--;
    char *out = b->buf;
    b->buf = NULL; b->len = b->cap = 0;
    return out;
}
static void sb_free(StrBuf *b) {
    free(b->buf);
    b->buf = NULL; b->len = b->cap = 0;
}

// -----------------------------
// Scalar CSV line parser (RFC-4180-ish)
// -----------------------------
static bool parse_csv_line(const char *line, StrVec *out) {
    StrBuf field; sb_init(&field);
    bool in_quotes = false;
    bool field_started = false;
    bool ok = true;

    size_t i = 0;
    for (;;) {
        char c = line[i];
        if (c == '\0' || c == '\n') {
            if (in_quotes) ok = false; // unmatched quote
            char *f = sb_take_cstr(&field);
            sv_push(out, f);
            break;
        }
        if (!in_quotes) {
            if (c == ',') {
                char *f = sb_take_cstr(&field);
                sv_push(out, f);
                field_started = false;
                i++;
            } else if (c == '\r') {
                i++; // ignore CR
            } else if (c == '"') {
                if (!field_started && field.len == 0) {
                    in_quotes = true;
                    field_started = true;
                    i++;
                } else {
                    // stray quote treated as literal
                    sb_pushc(&field, '"');
                    field_started = true;
                    i++;
                }
            } else {
                sb_pushc(&field, c);
                field_started = true;
                i++;
            }
        } else {
            // inside quotes
            if (c == '"') {
                char next = line[i+1];
                if (next == '"') {
                    sb_pushc(&field, '"'); // escaped quote
                    i += 2;
                } else {
                    in_quotes = false; // closing quote
                    i++;
                }
            } else {
                sb_pushc(&field, c);
                i++;
            }
        }
    }
    sb_free(&field);
    return ok;
}

// -----------------------------
// Scalar comma counter (fallback)
// -----------------------------
static int count_commas_scalar(FILE *f, size_t *bytes_out, size_t *commas_out) {
    const size_t BUF = 1 << 16;
    char *buf = (char*)malloc(BUF);
    if (!buf) { perror("malloc"); return -1; }

    size_t total = 0, commas = 0;
    while (!feof(f)) {
        size_t n = fread(buf, 1, BUF, f);
        total += n;
        for (size_t i = 0; i < n; ++i)
            if (buf[i] == ',') commas++;
    }
    free(buf);
    *bytes_out = total;
    *commas_out = commas;
    return 0;
}

// -----------------------------
// AVX2 comma counter (hot path)
// -----------------------------
static bool cpu_supports_avx2(void) {
#if PLATFORM_X86
  #if defined(__GNUC__) || defined(__clang__)
    // Built without -mavx2? Runtime check still valid on GCC/Clang.
    return __builtin_cpu_supports("avx2");
  #else
    // On MSVC or others, you might implement CPUID. Assume yes if compiled with AVX2.
    #ifdef __AVX2__
      return true;
    #else
      return false;
    #endif
  #endif
#else
    return false;
#endif
}

static int count_commas_avx2(FILE *f, size_t *bytes_out, size_t *commas_out) {
#if !PLATFORM_X86
    // Not x86: fall back
    return count_commas_scalar(f, bytes_out, commas_out);
#else
    const size_t BUF = 1 << 16;
    char *buf = (char*)malloc(BUF);
    if (!buf) { perror("malloc"); return -1; }

    size_t total = 0, commas = 0;
    const __m256i vcomma = _mm256_set1_epi8(',');

    for (;;) {
        size_t n = fread(buf, 1, BUF, f);
        if (n == 0) break;
        total += n;

        size_t i = 0;
        // 32-byte blocks
        for (; i + 32 <= n; i += 32) {
            __m256i data = _mm256_loadu_si256((const __m256i*)(buf + i));
            __m256i eq = _mm256_cmpeq_epi8(data, vcomma);
            uint32_t mask = (uint32_t)_mm256_movemask_epi8(eq);
            commas += __builtin_popcount(mask);
        }
        // tail
        for (; i < n; ++i) {
            if (buf[i] == ',') commas++;
        }
    }
    free(buf);
    *bytes_out = total;
    *commas_out = commas;
    return 0;
#endif
}

// -----------------------------
// Pretty print parsed fields (TSV)
// -----------------------------
static void print_fields(const StrVec *v) {
    for (size_t i = 0; i < v->len; ++i) {
        const char *s = v->items[i] ? v->items[i] : "";
        for (const char *p = s; *p; ++p) {
            if (*p == '\t') fputs("\\t", stdout);
            else if (*p == '\n') fputs("\\n", stdout);
            else putchar(*p);
        }
        if (i + 1 < v->len) putchar('\t');
    }
    putchar('\n');
}

// -----------------------------
// Main
// -----------------------------
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  %s file.csv [--print] [--limit N]\n"
            "  %s file.csv --count-commas\n",
            argv[0], argv[0]);
        return 1;
    }
    const char *path = argv[1];
    bool mode_count_commas = false;
    bool do_print = false;
    long print_limit = -1;

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--count-commas") == 0) mode_count_commas = true;
        else if (strcmp(argv[i], "--print") == 0) do_print = true;
        else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            print_limit = strtol(argv[++i], NULL, 10);
        }
    }

    if (mode_count_commas) {
        FILE *fc = fopen(path, "rb");
        if (!fc) { perror("fopen"); return 1; }
        size_t bytes=0, commas=0;

        int rc;
        if (cpu_supports_avx2()) {
            rc = count_commas_avx2(fc, &bytes, &commas);
        } else {
            // fallback scalar if no AVX2
            rc = count_commas_scalar(fc, &bytes, &commas);
        }
        fclose(fc);
        if (rc != 0) return 1;

        printf("Processed %zu bytes, found %zu commas%s\n",
               bytes, commas,
               cpu_supports_avx2() ? " (AVX2)" : " (scalar)");
        return 0;
    }

    // Parse mode (scalar, robust)
    FILE *f = fopen(path, "r");
    if (!f) { perror("fopen"); return 1; }

    char *line = NULL;
    size_t cap = 0;
    ssize_t nread;

    size_t rows = 0;
    size_t total_fields = 0;
    size_t max_fields_in_row = 0;
    size_t malformed = 0;

    while ((nread = getline(&line, &cap, f)) != -1) {
        StrVec fields; sv_init(&fields);
        bool ok = parse_csv_line(line, &fields);

        rows++;
        total_fields += fields.len;
        if (fields.len > max_fields_in_row) max_fields_in_row = fields.len;
        if (!ok) malformed++;

        if (do_print) {
            if (print_limit < 0 || (long)rows <= print_limit) {
                print_fields(&fields);
            }
        }

        sv_free_with_items(&fields);
    }
    free(line);
    fclose(f);

    double avg_fields = rows ? (double)total_fields / (double)rows : 0.0;
    printf("Rows: %zu\n", rows);
    printf("Avg fields/row: %.2f\n", avg_fields);
    printf("Max fields in a row: %zu\n", max_fields_in_row);
    printf("Malformed rows (unmatched quotes): %zu\n", malformed);

    return 0;
}

