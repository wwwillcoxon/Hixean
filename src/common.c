#include "hx/common.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define HX_MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define HX_MKDIR(p) mkdir((p), 0777)
#endif

const char *HX_VERSION = "0.1.0-dev";

struct HxArenaBlock {
    HxArenaBlock *next;
    size_t used;
    size_t cap;
    unsigned char data[];
};

#define HX_BLOCK_MIN (256u * 1024u)

void hx_arena_init(HxArena *a) {
    memset(a, 0, sizeof(*a));
    a->next_tag = 1;
}

static HxArenaBlock *hx_arena_new_block(HxArena *a, size_t need) {
    size_t cap = HX_BLOCK_MIN;
    while (cap < need + 16) cap *= 2;
    HxArenaBlock *b = (HxArenaBlock *)malloc(sizeof(HxArenaBlock) + cap);
    if (!b) {
        fprintf(stderr, "hx: out of memory (block %zu)\n", cap);
        exit(70);
    }
    b->next = a->head;
    b->used = 0;
    b->cap = cap;
    a->head = b;
    a->total += cap;
    if (a->total > a->peak) a->peak = a->total;
    return b;
}

void *hx_arena_alloc(HxArena *a, size_t n) {
    n = (n + 15u) & ~(size_t)15u;
    if (n == 0) n = 16;
    HxArenaBlock *b = a->head;
    if (!b || b->cap - b->used < n) {
        b = hx_arena_new_block(a, n);
    }
    void *p = b->data + b->used;
    b->used += n;
    return p;
}

void *hx_arena_alloc_tag(HxArena *a, size_t n, uint64_t tag) {
    void *p = hx_arena_alloc(a, n);
    (void)tag;
    return p;
}

void *hx_arena_calloc(HxArena *a, size_t n) {
    void *p = hx_arena_alloc(a, n);
    memset(p, 0, n);
    return p;
}

void *hx_arena_realloc_tmp(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "hx: out of memory\n");
        exit(70);
    }
    return q;
}

char *hx_arena_strndup(HxArena *a, const char *s, size_t n) {
    char *p = (char *)hx_arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *hx_arena_strdup(HxArena *a, const char *s) {
    return hx_arena_strndup(a, s, strlen(s));
}

char *hx_arena_vsprintf(HxArena *a, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) return hx_arena_strdup(a, "");
    char *p = (char *)hx_arena_alloc(a, (size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    return p;
}

char *hx_arena_sprintf(HxArena *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *p = hx_arena_vsprintf(a, fmt, ap);
    va_end(ap);
    return p;
}

struct HxInternEntry {
    struct HxInternEntry *next;
    uint32_t hash;
    uint32_t len;
    char text[];
};

typedef struct HxInternEntry HxInternEntry;

#define HX_INTERN_BUCKETS 4096

struct HxIntern {
    HxInternEntry *buckets[HX_INTERN_BUCKETS];
    size_t count;
    HxArena *arena;
    char *slab;
    size_t slab_used;
    size_t slab_cap;
};

HxIntern *hx_intern_new(HxArena *a) {
    HxIntern *t = (HxIntern *)hx_arena_calloc(a, sizeof(HxIntern));
    t->arena = a;
    return t;
}

static uint32_t hx_hash_bytes(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

HxSym hx_intern(HxIntern *t, const char *s, size_t n) {
    uint32_t h = hx_hash_bytes(s, n);
    size_t b = h & (HX_INTERN_BUCKETS - 1);
    for (HxInternEntry *e = t->buckets[b]; e; e = e->next) {
        if (e->hash == h && e->len == n && memcmp(e->text, s, n) == 0) return e->text;
    }
    HxInternEntry *e = (HxInternEntry *)hx_arena_alloc(t->arena, sizeof(HxInternEntry) + n + 1);
    e->hash = h;
    e->len = (uint32_t)n;
    memcpy(e->text, s, n);
    e->text[n] = 0;
    e->next = t->buckets[b];
    t->buckets[b] = e;
    t->count++;
    return e->text;
}

HxSym hx_intern_cstr(HxIntern *t, const char *s) { return hx_intern(t, s, strlen(s)); }

char hx_ascii_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
char hx_ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int hx_ascii_casecmp(const char *a, const char *b) {
    for (;;) {
        char x = hx_ascii_lower(*a++);
        char y = hx_ascii_lower(*b++);
        if (x != y) return (int)(unsigned char)x - (int)(unsigned char)y;
        if (!x) return 0;
    }
}

HxSym hx_intern_fold_ascii(HxIntern *t, const char *s, size_t n) {
    char *tmp = (char *)hx_arena_alloc(t->arena, n + 1);
    for (size_t i = 0; i < n; i++) tmp[i] = hx_ascii_lower(s[i]);
    tmp[n] = 0;
    return hx_intern(t, tmp, n);
}

const char *hx_sym_str(HxSym s) { return s; }
size_t hx_intern_count(HxIntern *t) { return t->count; }

HxPos hx_pos_of(const char *file, const char *src, const char *off) {
    HxPos p = {file, 1, 1};
    uint32_t line = 1, col = 1;
    for (const char *q = src; q < off && *q; q++) {
        if (*q == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }
    p.line = line;
    p.col = col;
    return p;
}

char *hx_read_file(HxArena *a, const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)hx_arena_alloc(a, (size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    if (out_len) *out_len = got;
    return buf;
}

static int hx_is_sep(char c) { return c == '/' || c == '\\'; }

char *hx_path_join(HxArena *a, const char *dir, const char *rel) {
    if (!dir || !*dir) return hx_arena_strdup(a, rel);
    if (rel[0] == '/' || rel[0] == '\\') return hx_arena_strdup(a, rel);
    size_t dn = strlen(dir);
    while (dn > 0 && hx_is_sep(dir[dn - 1])) dn--;
    if (dn == 0) return hx_arena_strdup(a, rel);
    return hx_arena_sprintf(a, "%.*s/%s", (int)dn, dir, rel);
}

char *hx_path_dirname(HxArena *a, const char *path) {
    size_t n = strlen(path);
    size_t i = n;
    while (i > 0 && !hx_is_sep(path[i - 1])) i--;
    if (i == 0) return hx_arena_strdup(a, ".");
    while (i > 1 && hx_is_sep(path[i - 1])) i--;
    return hx_arena_strndup(a, path, i);
}

char *hx_path_stem(HxArena *a, const char *path) {
    const char *slash = NULL;
    for (const char *p = path; *p; p++)
        if (hx_is_sep(*p)) slash = p;
    const char *start = slash ? slash + 1 : path;
    const char *dot = NULL;
    for (const char *p = start; *p; p++)
        if (*p == '.') dot = p;
    return dot ? hx_arena_strndup(a, start, (size_t)(dot - start))
               : hx_arena_strdup(a, start);
}

char *hx_path_basename(HxArena *a, const char *path) {
    const char *slash = NULL;
    for (const char *p = path; *p; p++)
        if (hx_is_sep(*p)) slash = p;
    return hx_arena_strdup(a, slash ? slash + 1 : path);
}

int hx_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int hx_write_file(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len ? 0 : -1;
}

void hx_mkdir_p(const char *path) {
    char *tmp = (char *)malloc(strlen(path) + 1);
    strcpy(tmp, path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = 0;
            HX_MKDIR(tmp);
            *p = path[p - tmp];
        }
    }
    HX_MKDIR(tmp);
    free(tmp);
}