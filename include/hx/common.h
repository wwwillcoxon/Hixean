#ifndef HX_COMMON_H
#define HX_COMMON_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct HxArenaBlock HxArenaBlock;

typedef struct {
    HxArenaBlock *head;
    size_t total;
    size_t peak;
    uint64_t next_tag;
} HxArena;

void hx_arena_init(HxArena *a);
void *hx_arena_alloc(HxArena *a, size_t n);
void *hx_arena_alloc_tag(HxArena *a, size_t n, uint64_t tag);
void *hx_arena_calloc(HxArena *a, size_t n);
char *hx_arena_strndup(HxArena *a, const char *s, size_t n);
char *hx_arena_strdup(HxArena *a, const char *s);
char *hx_arena_sprintf(HxArena *a, const char *fmt, ...);
char *hx_arena_vsprintf(HxArena *a, const char *fmt, va_list ap);

#define HX_VEC_TYPE(T)                                                         \
    struct {                                                                   \
        T *data;                                                               \
        int len;                                                               \
        int cap;                                                               \
    }

#define HX_VEC_ANON(T)                                                          \
    struct {                                                                   \
        T *data;                                                               \
        int len;                                                               \
        int cap;                                                               \
    }

#define HX_VEC_DECL(NAME, T)                                                  \
    typedef struct {                                                           \
        T *data;                                                               \
        int len;                                                               \
        int cap;                                                               \
    } NAME

#define HX_VEC_T(NAME, T) typedef struct { T *data; int len; int cap; } NAME
#define HX_VEC(VAR, T) typedef struct { T *data; int len; int cap; } VAR##_t; VAR##_t VAR = {0, 0, 0}

#define hx_vec_grow(ptr, len, cap, elem)                                       \
    do {                                                                       \
        if ((len) == (cap)) {                                                  \
            int nx = (cap) ? (cap) * 2 : 8;                                    \
            ptr = (__typeof__(ptr))(uintptr_t)hx_arena_realloc_tmp(             \
                      (void *)(uintptr_t)ptr, (size_t)nx * (elem));              \
            cap = nx;                                                          \
        }                                                                      \
    } while (0)

void *hx_arena_realloc_tmp(void *p, size_t n);

#define HX_VEC_PUSH(v, item)                                                   \
    do {                                                                       \
        __typeof__((v).data) hx_p_ = (v).data;                                 \
        hx_vec_grow(hx_p_, (v).len, (v).cap, sizeof(*(v).data));               \
        (v).data = (__typeof__((v).data))hx_p_;                                \
        (v).data[(v).len++] = (item);                                          \
    } while (0)

#define HX_VEC_POP(v) ((v).data[--(v).len])

typedef struct HxIntern HxIntern;

typedef const char *HxSym;

HxIntern *hx_intern_new(HxArena *a);
HxSym hx_intern(HxIntern *t, const char *s, size_t n);
HxSym hx_intern_cstr(HxIntern *t, const char *s);
HxSym hx_intern_fold_ascii(HxIntern *t, const char *s, size_t n);
const char *hx_sym_str(HxSym s);
size_t hx_intern_count(HxIntern *t);

char hx_ascii_upper(char c);
char hx_ascii_lower(char c);
int hx_ascii_casecmp(const char *a, const char *b);

typedef struct {
    HxArena *arena;
    char *data;
    size_t len;
    size_t cap;
} HxBuf;

void hx_buf_reserve(HxBuf *b, size_t n);
void hx_buf_put(HxBuf *b, const char *s, size_t n);
void hx_buf_str(HxBuf *b, const char *s);
void hx_buf_printf(HxBuf *b, const char *fmt, ...);

typedef struct {
    uint64_t h;
} HxHash;

void hx_fnv_init(HxHash *x);
void hx_fnv_bytes(HxHash *x, const void *data, size_t n);
void hx_fnv_str(HxHash *x, const char *s);
void hx_fnv_u64(HxHash *x, uint64_t v);
void hx_fnv_hex(HxHash *x, char *out, int n);

typedef struct {
    uint32_t start;
    uint32_t len;
} HxSpan;

typedef struct {
    const char *file;
    uint32_t line;
    uint32_t col;
} HxPos;

HxPos hx_pos_of(const char *file, const char *src, const char *off);

char *hx_read_file(HxArena *a, const char *path, size_t *out_len);
char *hx_path_join(HxArena *a, const char *dir, const char *rel);
char *hx_path_dirname(HxArena *a, const char *path);
char *hx_path_stem(HxArena *a, const char *path);
char *hx_path_basename(HxArena *a, const char *path);
int hx_file_exists(const char *path);
int hx_write_file(const char *path, const char *data, size_t len);
void hx_mkdir_p(const char *path);

extern const char *HX_VERSION;

#endif