// nosimd_csv.c
// Non-SIMD CSV parser + non-SIMD comma counter
// Build: gcc -O3 -std=c11 nosimd_csv.c -o nosimd_csv
// Usage:
//   Count commas only:           ./nosimd_csv file.csv --count-commas
//   Parse & stats only:          ./nosimd_csv file.csv
//   Parse & print first N rows:  ./nosimd_csv file.csv --print --limit 5

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

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

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf *b) {
    b->buf = NULL; b->len = 0; b->cap = 0;
}
static void sb_clear(StrBuf *b) {
    b->len = 0;
}
static void sb_reserve(StrBuf *b, size_t need) {
    if (need <= b->cap) return;
    size_t ncap = b->cap ? b->cap * 2 : 64;
    while (ncap < need) ncap *= 2;
    char *nbuf = (char*)realloc(b->buf, ncap);
    if (!nbuf) { perror("realloc"); exit(1); }
    b->buf = nbuf; b->cap = ncap;
}
static void sb_pushc(StrBuf *b, char c) {
    sb_reserve(b, b->len + 1 + 1);
    b->buf[b->len++] = c;
    b->buf[b->len] = '\0';
}
static char* sb_take_cstr(StrBuf *b) {
    // Ensure null-terminated
    sb_pushc(b, '\0'); // adds an extra, then:
    b->len--;          // drop the extra count
    // hand off ownership
    char *out = b->buf;
    b->buf = NULL; b->len = b->cap = 0;
    return out;
}
static void sb_free(StrBuf *b) {
    free(b->buf);
    b->buf = NULL; b->len = b->cap = 0;
}

// Parse one CSV line -> fields in 'out'.
// Returns true on success; false if malformed quoting (still returns best-effort fields).
static bool parse_csv_line(const char *line, StrVec *out) {
    StrBuf field; sb_init(&field);
    bool in_quotes = false;
    bool field_started = false;
    bool ok = true;

    size_t i = 0;
    for (;;) {
        char c = line[i];
        if (c == '\0' || c == '\n') {
            if (in_quotes) { ok = false; /* unmatched quote */ }
            // finalize current field
            char *f = sb_take_cstr(&field);
            sv_push(out, f);
            break;
        }
        if (!in_quotes) {
            if (c == ',') {
                // end field
                char *f = sb_take_cstr(&field);
                sv_push(out, f);
                field_started = false;
            } else if (c == '\r') {
                // ignore CR (handle CRLF)
            } else if (c == '"') {
                if (!field_started && field.len == 0) {
                    in_quotes = true;
                    field_started = true;
                } else {
                    // stray quote inside unquoted field -> treat as literal
                    sb_pushc(&field, '"');
                    field_started = true;
                }
            } else {
                sb_pushc(&field, c);
                field_started = true;
            }
            i++;
        } else {
            // inside quotes
            if (c == '"') {
                // look ahead for escaped quote
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

// Non-SIMD comma counter (baseline).
static int count_commas_file(const char *filename, size_t *bytes_out, size_t *commas_out) {
    FILE *f = fopen(filename, "rb");
    if (!f) { perror("fopen"); return -1; }
    const size_t BUF = 1 << 16;
    char *buf = (char*)malloc(BUF);
    if (!buf) { perror("malloc"); fclose(f); return -1; }

    size_t total = 0, commas = 0;
    while (!feof(f)) {
        size_t n = fread(buf, 1, BUF, f);
        total += n;
        for (size_t i = 0; i < n; ++i) {
            if (buf[i] == ',') commas++;
        }
    }
    fclose(f); free(buf);
    *bytes_out = total; *commas_out = commas;
    return 0;
}

static void print_fields(const StrVec *v) {
    for (size_t i = 0; i < v->len; ++i) {
        const char *s = v->items[i] ? v->items[i] : "";
        // Print as TSV for readability; escape tabs/newlines minimally
        for (const char *p = s; *p; ++p) {
            if (*p == '\t') fputs("\\t", stdout);
            else if (*p == '\n') fputs("\\n", stdout);
            else putchar(*p);
        }
        if (i + 1 < v->len) putchar('\t');
    }
    putchar('\n');
}

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
        size_t bytes = 0, commas = 0;
        if (count_commas_file(path, &bytes, &commas) != 0) return 1;
        printf("Processed %zu bytes, found %zu commas\n", bytes, commas);
        return 0;
    }

    // Parse mode
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

