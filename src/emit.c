#include "hx/emit.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    HxArena *arena;
    char *data;
    size_t len;
    size_t cap;
} HxBuf;

typedef struct {
    HxArena *arena;
    HxUnit *unit;
    HxDiagBag *diags;
    HxProfile profile;
    HxBuf out;
    int tmp;
    int uses_string;
    int uses_arena;
    int uses_exit;
    int uses_result;
    int uses_vec;
    int ret_is_result;
    int epilogue;
    const char *ret_field;
    int arena_depth;
    const char *arena_stack[16];
    int epilogue_stack[16];
    int epilogue_depth;
    HxStmtVec *match_cases;
} HxEmit;

static void hx_buf_reserve(HxBuf *b, size_t n) {
    if (b->len + n + 1 <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->len + n + 1) nc *= 2;
    b->data = (char *)realloc(b->data, nc);
    if (!b->data) {
        fprintf(stderr, "hx: out of memory emitting\n");
        exit(70);
    }
    b->cap = nc;
}

static void hx_buf_put(HxBuf *b, const char *s, size_t n) {
    hx_buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

static void hx_buf_str(HxBuf *b, const char *s) { hx_buf_put(b, s, strlen(s)); }

static void hx_buf_printf(HxBuf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n > 0) {
        hx_buf_reserve(b, (size_t)n);
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
        b->len += (size_t)n;
    }
    va_end(ap);
}

static const char *hx_c_ty(HxEmit *e, HxTy *t) {
    if (!t) return "int32_t";
    switch (t->kind) {
        case TY_VOID: return "void";
        case TY_BOOL: return "hx_bool";
        case TY_INT: return "int32_t";
        case TY_I64: return "int64_t";
        case TY_DURATION: return "int64_t";
        case TY_FLOAT: return "double";
        case TY_STRING: return "hx_str";
        case TY_PTR:
        case TY_REF: {
            const char *inner = hx_c_ty(e, t->inner);
            return hx_arena_sprintf(e->arena, "%s*", inner);
        }
        case TY_ARRAY: return "hx_span";
        case TY_VEC2: return "hx_vec2";
        case TY_VEC3: return "hx_vec3";
        case TY_VEC4: return "hx_vec4";
        case TY_MAT4: return "hx_mat4";
        case TY_QUAT: return "hx_quat";
        case TY_NAMED:
            if (hx_ty_is_result(t)) return "hx_result";
            if (t->decl) return hx_arena_sprintf(e->arena, "hx_T_%s", hx_sym_str(t->decl->name));
            return "int32_t";
        default: return "int32_t";
    }
}

static const char *HX_RT_FREESTANDING_MEM =
    "void *memcpy(void *d, const void *s, unsigned long n) {\n"
    "  unsigned char *dd = (unsigned char *)d;\n"
    "  const unsigned char *ss = (const unsigned char *)s;\n"
    "  unsigned long i;\n"
    "  for (i = 0; i < n; i++) dd[i] = ss[i];\n"
    "  return d;\n"
    "}\n"
    "void *memset(void *d, int c, unsigned long n) {\n"
    "  unsigned char *dd = (unsigned char *)d;\n"
    "  unsigned long i;\n"
    "  for (i = 0; i < n; i++) dd[i] = (unsigned char)c;\n"
    "  return d;\n"
    "}\n";

static const char *HX_RT_TYPES =
    "typedef struct { char *p; int64_t n; } hx_str;\n"
    "typedef struct { void *data; int64_t len; } hx_span;\n";

static const char *HX_RT_FREESTANDING =
    "typedef int hx_bool;\n"
    "static long hx_sys_write(int fd, const void *p, int64_t n) {\n"
    "  long r;\n"
    "  __asm__ volatile(\"syscall\" : \"=a\"(r) : \"a\"(1L), \"D\"((long)fd), \"S\"(p), \"d\"(n)\n"
    "                   : \"rcx\", \"r11\", \"memory\");\n"
    "  return r;\n"
    "}\n"
    "static void hx_exit(int code) {\n"
    "  __asm__ volatile(\"syscall\" : : \"a\"(60L), \"D\"((long)code) : \"rcx\", \"r11\", \"memory\");\n"
    "  __builtin_unreachable();\n"
    "}\n"
    "static long hx_sys_mmap(int64_t n) {\n"
    "  long r;\n"
    "  __asm__ volatile(\"syscall\" : \"=a\"(r)\n"
    "                   : \"a\"(9L), \"D\"(0L), \"S\"(n), \"d\"((long)0x22), \"r\"((long)0x32),\n"
    "                     \"r\"((long)-1L)\n"
    "                   : \"rcx\", \"r11\", \"memory\");\n"
    "  return r;\n"
    "}\n"
    "static long hx_sys_munmap(void *p) {\n"
    "  long r;\n"
    "  __asm__ volatile(\"syscall\" : \"=a\"(r)\n"
    "                   : \"a\"(11L), \"D\"((long)p), \"S\"((long)0), \"d\"((long)0x1000)\n"
    "                   : \"rcx\", \"r11\", \"memory\");\n"
    "  return r;\n"
    "}\n";

static const char *HX_RT_LIBC =
    "#include <unistd.h>\n"
    "typedef int hx_bool;\n"
    "static long hx_sys_write(int fd, const void *p, int64_t n) {\n"
    "  long r;\n"
    "  do { r = (long)write(fd, p, (size_t)n); } while (r < 0);\n"
    "  return r;\n"
    "}\n"
    "static void hx_exit(int code) { _exit(code); }\n"
    "#include <stdlib.h>\n";

static const char *HX_RT_CORE =
    "static void hx_write(int fd, const void *p, int64_t n) {\n"
    "  const char *s = (const char *)p;\n"
    "  while (n > 0) {\n"
    "    long r = hx_sys_write(fd, s, n);\n"
    "    if (r <= 0) return;\n"
    "    s += r;\n"
    "    n -= r;\n"
    "  }\n"
    "}\n"
    "static void hx_out(const char *p, int64_t n) { hx_write(1, p, n); }\n"
    "static void hx_u64_to_dec(uint64_t v, char *buf, int *len) {\n"
    "  char tmp[24];\n"
    "  int t = 0;\n"
    "  if (v == 0) tmp[t++] = '0';\n"
    "  while (v) { tmp[t++] = (char)('0' + (v % 10u)); v /= 10u; }\n"
    "  while (t) buf[(*len)++] = tmp[--t];\n"
    "}\n"
    "static void hx_print_i64(int64_t v) {\n"
    "  char buf[24];\n"
    "  int n = 0;\n"
    "  uint64_t u;\n"
    "  if (v < 0) { buf[n++] = '-'; u = (uint64_t)(-(v + 1)) + 1u; }\n"
    "  else u = (uint64_t)v;\n"
    "  hx_u64_to_dec(u, buf, &n);\n"
    "  hx_out(buf, n);\n"
    "}\n"
    "static void hx_print_bool(hx_bool v) { hx_out(v ? \"true\" : \"false\", v ? 4 : 5); }\n"
    "static void hx_print_str(hx_str s) { hx_write(1, s.p, s.n); }\n"
    "static void hx_print_f64(double d) {\n"
    "  char buf[48];\n"
    "  int n = 0, i;\n"
    "  uint64_t ip;\n"
    "  double frac;\n"
    "  if (d != d) { hx_out(\"nan\", 3); return; }\n"
    "  if (d < 0) { buf[n++] = '-'; d = -d; }\n"
    "  ip = (uint64_t)d;\n"
    "  frac = d - (double)ip;\n"
    "  hx_u64_to_dec(ip, buf, &n);\n"
    "  if (frac > 0) {\n"
    "    buf[n++] = '.';\n"
    "    for (i = 0; i < 9; i++) {\n"
    "      int dd;\n"
    "      frac *= 10.0;\n"
    "      dd = (int)frac;\n"
    "      if (dd > 9) dd = 9;\n"
    "      buf[n++] = (char)('0' + dd);\n"
    "      frac -= (double)dd;\n"
    "      if (frac <= 0) break;\n"
    "    }\n"
    "  }\n"
    "  hx_out(buf, n);\n"
    "}\n"
    "static void hx_print_duration(int64_t ns) {\n"
    "  int64_t unit_ns;\n"
    "  const char *unit;\n"
    "  if (ns != 0 && ns % 3600000000000LL == 0) { unit_ns = 3600000000000LL; unit = \"h\"; }\n"
    "  else if (ns != 0 && ns % 60000000000LL == 0) { unit_ns = 60000000000LL; unit = \"m\"; }\n"
    "  else if (ns != 0 && ns % 1000000000LL == 0) { unit_ns = 1000000000LL; unit = \"s\"; }\n"
    "  else if (ns != 0 && ns % 1000000LL == 0) { unit_ns = 1000000LL; unit = \"ms\"; }\n"
    "  else { unit_ns = 1LL; unit = \"ns\"; }\n"
    "  hx_print_i64(ns / unit_ns);\n"
    "  {\n"
    "    int64_t k = 0;\n"
    "    while (unit[k]) k++;\n"
    "    hx_out(unit, k);\n"
    "  }\n"
    "}\n"
    "static hx_bool hx_str_eq(hx_str a, hx_str b) {\n"
    "  int64_t i;\n"
    "  if (a.n != b.n) return 0;\n"
    "  for (i = 0; i < a.n; i++) if (a.p[i] != b.p[i]) return 0;\n"
    "  return 1;\n"
    "}\n"
    "static hx_bool hx_str_eq_c(hx_str a, const char *b) {\n"
    "  int64_t i, n = 0;\n"
    "  while (b[n]) n++;\n"
    "  if (a.n != n) return 0;\n"
    "  for (i = 0; i < n; i++) if (a.p[i] != b[i]) return 0;\n"
    "  return 1;\n"
    "}\n"
    "static void hx_panic(const char *msg, int64_t n) {\n"
    "  hx_write(2, \"hx: error: \", 12);\n"
    "  hx_write(2, msg, n);\n"
    "  hx_write(2, \"\\n\", 1);\n"
    "  hx_exit(70);\n"
    "}\n"
    "static hx_str hx_lit(const char *p, int64_t n) { hx_str s; s.p = (char *)p; s.n = n; return s; }\n"
    "static hx_span hx_span_make(void *p, int64_t n) { hx_span s; s.data = p; s.len = n; return s; }\n";

static const char *HX_RT_RESULT =
    "typedef struct { int32_t tag; int64_t i; double f; hx_str s; } hx_result;\n"
    "static hx_result hx_ok_i(int64_t v) { hx_result r; r.tag = 0; r.i = v; r.f = (double)v;"
    " r.s = hx_lit(\"\", 0); return r; }\n"
    "static hx_result hx_ok_f(double v) { hx_result r; r.tag = 0; r.i = (int64_t)v; r.f = v;"
    " r.s = hx_lit(\"\", 0); return r; }\n"
    "static hx_result hx_ok_s(hx_str v) { hx_result r; r.tag = 0; r.i = 0; r.f = 0.0;"
    " r.s = v; return r; }\n"
    "static hx_result hx_err_s(hx_str v) { hx_result r; r.tag = 1; r.i = 0; r.f = 0.0;"
    " r.s = v; return r; }\n"
    "static hx_bool hx_is_ok(hx_result v) { return v.tag == 0; }\n"
    "static int64_t hx_pat_eq_i(int64_t a, int64_t b) { return a == b; }\n"
    "static int64_t hx_pat_eq_d(double a, double b) { return a == b; }\n"
    "static int64_t hx_pat_eq_s(hx_str a, hx_str b) { return hx_str_eq(a, b); }\n"
    "static void hx_propagate_top(hx_result v) {\n"
    "  hx_write(2, \"hx: error no controlado: \", 25);\n"
    "  hx_write(2, v.s.p, v.s.n);\n"
    "  hx_write(2, \"\\n\", 1);\n"
    "  hx_exit(70);\n"
    "}\n";

static const char *HX_RT_CHECKED =
    "static int32_t hx_add_i32(int32_t a, int32_t b) {\n"
    "  int32_t r;\n"
    "  if (__builtin_add_overflow(a, b, &r))\n"
    "    hx_panic(\"desbordamiento de Entero en +\", sizeof(\"desbordamiento de Entero en +\") - 1);\n"
    "  return r;\n"
    "}\n"
    "static int32_t hx_sub_i32(int32_t a, int32_t b) {\n"
    "  int32_t r;\n"
    "  if (__builtin_sub_overflow(a, b, &r))\n"
    "    hx_panic(\"desbordamiento de Entero en -\", sizeof(\"desbordamiento de Entero en -\") - 1);\n"
    "  return r;\n"
    "}\n"
    "static int64_t hx_add_i64(int64_t a, int64_t b) {\n"
    "  int64_t r;\n"
    "  if (__builtin_add_overflow(a, b, &r))\n"
    "    hx_panic(\"desbordamiento de I64 en +\", sizeof(\"desbordamiento de I64 en +\") - 1);\n"
    "  return r;\n"
    "}\n"
    "static int64_t hx_sub_i64(int64_t a, int64_t b) {\n"
    "  int64_t r;\n"
    "  if (__builtin_sub_overflow(a, b, &r))\n"
    "    hx_panic(\"desbordamiento de I64 en -\", sizeof(\"desbordamiento de I64 en -\") - 1);\n"
    "  return r;\n"
    "}\n"
    "static int32_t hx_add_sat_i32(int32_t a, int32_t b) {\n"
    "  int64_t r = (int64_t)a + (int64_t)b;\n"
    "  if (r > 2147483647LL) return 2147483647;\n"
    "  if (r < -2147483647LL - 1) return -2147483647 - 1;\n"
    "  return (int32_t)r;\n"
    "}\n"
    "static int32_t hx_sub_sat_i32(int32_t a, int32_t b) {\n"
    "  int64_t r = (int64_t)a - (int64_t)b;\n"
    "  if (r > 2147483647LL) return 2147483647;\n"
    "  if (r < -2147483647LL - 1) return -2147483647 - 1;\n"
    "  return (int32_t)r;\n"
    "}\n"
    "static int64_t hx_add_sat_i64(int64_t a, int64_t b) {\n"
    "  if (b > 0 && a > 9223372036854775807LL - b) return 9223372036854775807LL;\n"
    "  if (b < 0 && a < -9223372036854775807LL - 1 - b) return -9223372036854775807LL - 1;\n"
    "  return a + b;\n"
    "}\n"
    "static int64_t hx_sub_sat_i64(int64_t a, int64_t b) {\n"
    "  if (b < 0 && a > 9223372036854775807LL + b) return 9223372036854775807LL;\n"
    "  if (b > 0 && a < -9223372036854775807LL - 1 + b) return -9223372036854775807LL - 1;\n"
    "  return a - b;\n"
    "}\n";

static const char *HX_RT_STRFUNS =
    "static int64_t hx_len_str(hx_str s) { return s.n; }\n"
    "static hx_bool hx_is_empty_str(hx_str s) { return s.n == 0; }\n"
    "static hx_str hx_str_at(hx_str s, int64_t i) { hx_str r; r.p = s.p + i; r.n = 1; return r; }\n"
    "static hx_str hx_str_slice(hx_str s, int64_t a, int64_t b) {\n"
    "  hx_str r;\n"
    "  if (a < 0) a = 0;\n"
    "  if (b > s.n) b = s.n;\n"
    "  if (b < a) b = a;\n"
    "  r.p = s.p + a;\n"
    "  r.n = b - a;\n"
    "  return r;\n"
    "}\n"
    "static hx_bool hx_is_upper(int c) { return c >= 'A' && c <= 'Z'; }\n"
    "static hx_bool hx_is_lower(int c) { return c >= 'a' && c <= 'z'; }\n"
    "static hx_bool hx_is_digit(int c) { return c >= '0' && c <= '9'; }\n"
    "static hx_bool hx_is_alpha(int c) { return hx_is_upper(c) || hx_is_lower(c); }\n"
    "static hx_bool hx_is_alnum(int c) { return hx_is_alpha(c) || hx_is_digit(c); }\n"
    "static hx_bool hx_is_space_c(int c) {\n"
    "  return c == ' ' || c == '\\t' || c == '\\n' || c == '\\r' || c == '\\v' || c == '\\f';\n"
    "}\n";

static const char *HX_RT_STRMORE =
    "static hx_str hx_upper_str(hx_str s) {\n"
    "  hx_str r = hx_concat(s, hx_lit(\"\", 0));\n"
    "  int64_t i;\n"
    "  for (i = 0; i < s.n; i++) {\n"
    "    char c = s.p[i];\n"
    "    if (hx_is_lower((unsigned char)c)) r.p[i] = (char)(c - 32);\n"
    "  }\n"
    "  return r;\n"
    "}\n"
    "static hx_str hx_lower_str(hx_str s) {\n"
    "  hx_str r = hx_concat(s, hx_lit(\"\", 0));\n"
    "  int64_t i;\n"
    "  for (i = 0; i < s.n; i++) {\n"
    "    char c = s.p[i];\n"
    "    if (hx_is_upper((unsigned char)c)) r.p[i] = (char)(c + 32);\n"
    "  }\n"
    "  return r;\n"
    "}\n"
    "static hx_str hx_trim_str(hx_str s) {\n"
    "  hx_str r;\n"
    "  int64_t a = 0, b = s.n;\n"
    "  while (a < b && hx_is_space_c((unsigned char)s.p[a])) a++;\n"
    "  while (b > a && hx_is_space_c((unsigned char)s.p[b - 1])) b--;\n"
    "  r.p = s.p + a;\n"
    "  r.n = b - a;\n"
    "  return r;\n"
    "}\n"
    "static hx_str hx_repeat_str(hx_str s, int64_t k) {\n"
    "  hx_str r;\n"
    "  int64_t i;\n"
    "  if (k < 0) k = 0;\n"
    "  if (k * s.n > (int64_t)sizeof(hx_cbuf))\n"
    "    hx_panic(\"cadena intermedia mayor que el buffer de concatenacion\",\n"
    "            sizeof(\"cadena intermedia mayor que el buffer de concatenacion\") - 1);\n"
    "  for (i = 0; i < k; i++) memcpy(hx_cbuf + i * s.n, s.p, (size_t)s.n);\n"
    "  r.p = hx_cbuf;\n"
    "  r.n = s.n * k;\n"
    "  return r;\n"
    "}\n";

static const char *HX_RT_ARENA =
    "typedef struct hx_arena_blk { struct hx_arena_blk *next; int64_t used; int64_t cap; }\n"
    "    hx_arena_blk;\n"
    "typedef struct { hx_arena_blk *head; } hx_arena;\n"
    "static const int64_t HX_ARENA_GRAIN = 65536;\n"
    "static void *hx_arena_alloc(hx_arena *a, int64_t n) {\n"
    "  hx_arena_blk *b = a->head;\n"
    "  void *p;\n"
    "  if (n < 0) hx_panic(\"reserva de tamano negativo\", 26);\n"
    "  if (n == 0) n = 1;\n"
    "  if (!b || b->cap - b->used < n) {\n"
    "    int64_t cap = HX_ARENA_GRAIN;\n"
    "    while (cap < n) cap *= 2;\n"
    "    b = (hx_arena_blk *)hx_raw_alloc(cap);\n"
    "    if (!b) hx_panic(\"sin memoria para la arena\", 25);\n"
    "    b->next = a->head;\n"
    "    b->used = 0;\n"
    "    b->cap = cap;\n"
    "    a->head = b;\n"
    "  }\n"
    "  p = (char *)b + sizeof(hx_arena_blk) + b->used;\n"
    "  b->used += (n + 15) & ~(int64_t)15;\n"
    "  return p;\n"
    "}\n"
    "static void hx_arena_init(hx_arena *a) { a->head = 0; }\n"
    "static void hx_arena_free(hx_arena *a) {\n"
    "  hx_arena_blk *b = a->head;\n"
    "  while (b) {\n"
    "    hx_arena_blk *next = b->next;\n"
    "    hx_raw_free(b);\n"
    "    b = next;\n"
    "  }\n"
    "  a->head = 0;\n"
    "}\n";

static const char *HX_RT_RAWALLOC_FREESTANDING =
    "#if defined(__linux__) && defined(__x86_64__)\n"
    "static void *hx_raw_alloc(int64_t n) {\n"
    "  register long r10 __asm__(\"r10\") = 0x22;\n"
    "  register long r8 __asm__(\"r8\") = 0xffffffffL;\n"
    "  register long r9 __asm__(\"r9\") = 0;\n"
    "  long r;\n"
    "  __asm__ volatile(\"syscall\" : \"=a\"(r)\n"
    "                   : \"a\"(9L), \"D\"(0L), \"S\"(n), \"d\"(3L), \"r\"(r10), \"r\"(r8),\n"
    "                     \"r\"(r9)\n"
    "                   : \"rcx\", \"r11\", \"memory\");\n"
    "  if (r < 0 && r > -4096) return 0;\n"
    "  return (void *)r;\n"
    "}\n"
    "static void hx_raw_free(void *p) {\n"
    "  register long r8 __asm__(\"r8\") = 0;\n"
    "  register long r9 __asm__(\"r9\") = 0;\n"
    "  long r;\n"
    "  __asm__ volatile(\"syscall\" : \"=a\"(r)\n"
    "                   : \"a\"(11L), \"D\"((long)p), \"S\"((long)0), \"d\"(0x1000L),\n"
    "                     \"r\"(r8), \"r\"(r9)\n"
    "                   : \"rcx\", \"r11\", \"memory\");\n"
    "  (void)r;\n"
    "}\n"
    "#else\n"
    "static char hx_static_heap[1048576];\n"
    "static int64_t hx_static_heap_used;\n"
    "static void *hx_raw_alloc(int64_t n) {\n"
    "  char *p;\n"
    "  n = (n + 15) & ~(int64_t)15;\n"
    "  if (hx_static_heap_used + n > (int64_t)sizeof(hx_static_heap)) return 0;\n"
    "  p = hx_static_heap + hx_static_heap_used;\n"
    "  hx_static_heap_used += n;\n"
    "  return p;\n"
    "}\n"
    "static void hx_raw_free(void *p) { (void)p; }\n"
    "#endif\n";

static const char *HX_RT_RAWALLOC_LIBC =
    "static void *hx_raw_alloc(int64_t n) { return malloc((size_t)n); }\n"
    "static void hx_raw_free(void *p) { free(p); }\n"
    "static void *hx_heap_alloc(int64_t n) { return malloc((size_t)n); }\n"
    "static void hx_heap_free(void *p) { free(p); }\n";

static const char *HX_RT_VEC_PRE =
    "static long long hx_pow10(int k) {\n"
    "  long long r = 1;\n"
    "  int i;\n"
    "  for (i = 0; i < k; i++) r *= 10;\n"
    "  return r;\n"
    "}\n"
    "static void hx_print_f32(float v) {\n"
    "  double d = (double)v;\n"
    "  long long scaled;\n"
    "  char buf[32];\n"
    "  int n = 0, i;\n"
    "  if (v != v) { hx_out(\"nan\", 3); return; }\n"
    "  if (d == (double)(long long)d) { hx_print_i64((long long)d); return; }\n"
    "  scaled = (long long)(d * 1000000.0);\n"
    "  if (scaled == 0) { hx_out(\"0\", 1); return; }\n"
    "  if (scaled < 0) { hx_out(\"-\", 1); scaled = -scaled; }\n"
    "  hx_u64_to_dec((uint64_t)(scaled / 1000000), buf, &n);\n"
    "  hx_out(buf, n);\n"
    "  n = 0;\n"
    "  hx_out(\".\", 1);\n"
    "  for (i = 5; i >= 0; i--) {\n"
    "    int dig = (int)((scaled / (long long)hx_pow10(i)) % 10);\n"
    "    if (dig) break;\n"
    "  }\n"
    "  for (; i >= 0; i--) {\n"
    "    int dig = (int)((scaled / (long long)hx_pow10(i)) % 10);\n"
    "    buf[n++] = (char)(\'0\' + dig);\n"
    "  }\n"
    "  while (n > 0 && buf[n - 1] == \'0\') n--;\n"
    "  hx_out(buf, n);\n"
    "}\n";

static const char *HX_RT_VEC =
    "typedef struct { float x, y; } hx_vec2;\n"
    "typedef struct { float x, y, z; } hx_vec3;\n"
    "typedef struct { float x, y, z, w; } hx_vec4;\n"
    "typedef hx_vec4 hx_quat;\n"
    "typedef struct { hx_vec4 r0, r1, r2, r3; } hx_mat4;\n"
    "static hx_vec2 hx_v2(float x, float y) { hx_vec2 v; v.x = x; v.y = y; return v; }\n"
    "static hx_vec3 hx_v3(float x, float y, float z) {\n"
    "  hx_vec3 v; v.x = x; v.y = y; v.z = z; return v;\n"
    "}\n"
    "static hx_vec4 hx_v4(float x, float y, float z, float w) {\n"
    "  hx_vec4 v; v.x = x; v.y = y; v.z = z; v.w = w; return v;\n"
    "}\n"
    "static hx_vec2 hx_v2s(float s) { return hx_v2(s, s); }\n"
    "static hx_vec3 hx_v3s(float s) { return hx_v3(s, s, s); }\n"
    "static hx_vec4 hx_v4s(float s) { return hx_v4(s, s, s, s); }\n"
    "static float hx_dot(hx_vec3 a, hx_vec3 b) {\n"
    "  return a.x * b.x + a.y * b.y + a.z * b.z;\n"
    "}\n"
    "static hx_vec3 hx_cross(hx_vec3 a, hx_vec3 b) {\n"
    "  return hx_v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);\n"
    "}\n"
    "static float hx_len3(hx_vec3 v) { return hx_dot(v, v); }\n"
    "static double hx_sqrt_d(double x) {\n"
    "  double r = x, prev = 0.0;\n"
    "  int i;\n"
    "  if (x <= 0.0) return 0.0;\n"
    "  for (i = 0; i < 40; i++) {\n"
    "    prev = r;\n"
    "    r = 0.5 * (r + x / r);\n"
    "    if (r - prev < 1e-12 && prev - r < 1e-12) break;\n"
    "  }\n"
    "  return r;\n"
    "}\n"
    "static hx_vec3 hx_normalized(hx_vec3 v) {\n"
    "  float l = hx_len3(v);\n"
    "  if (l <= 0.0f) return v;\n"
    "  l = 1.0f / (float)hx_sqrt_d((double)l);\n"
    "  return hx_v3(v.x * l, v.y * l, v.z * l);\n"
    "}\n"
    "static hx_vec3 hx_add3(hx_vec3 a, hx_vec3 b) {\n"
    "  return hx_v3(a.x + b.x, a.y + b.y, a.z + b.z);\n"
    "}\n"
    "static hx_vec3 hx_sub3(hx_vec3 a, hx_vec3 b) {\n"
    "  return hx_v3(a.x - b.x, a.y - b.y, a.z - b.z);\n"
    "}\n"
    "static hx_vec3 hx_scale3(hx_vec3 a, float s) {\n"
    "  return hx_v3(a.x * s, a.y * s, a.z * s);\n"
    "}\n"
    "static void hx_print_vec2(hx_vec2 v) {\n"
    "  hx_out(\"(\", 1); hx_print_f32(v.x); hx_out(\", \", 2);\n"
    "  hx_print_f32(v.y); hx_out(\")\", 1);\n"
    "}\n"
    "static void hx_print_vec3(hx_vec3 v) {\n"
    "  hx_out(\"(\", 1); hx_print_f32(v.x); hx_out(\", \", 2);\n"
    "  hx_print_f32(v.y); hx_out(\", \", 2);\n"
    "  hx_print_f32(v.z); hx_out(\")\", 1);\n"
    "}\n"
    "static void hx_print_vec4(hx_vec4 v) {\n"
    "  hx_out(\"(\", 1); hx_print_f32(v.x); hx_out(\", \", 2);\n"
    "  hx_print_f32(v.y); hx_out(\", \", 2);\n"
    "  hx_print_f32(v.z); hx_out(\", \", 2);\n"
    "  hx_print_f32(v.w); hx_out(\")\", 1);\n"
    "}\n";

static const char *HX_RT_ZERO =
    "static int32_t hx_zero_int32(void) { return 0; }\n"
    "static int64_t hx_zero_int64(void) { return 0; }\n"
    "static double hx_zero_f64(void) { return 0.0; }\n"
    "static hx_str hx_zero_str(void) { return hx_lit(\"\", 0); }\n"
    "static hx_span hx_zero_span(void) { return hx_span_make(NULL, 0); }\n";

static const char *HX_RT_STRING =
    "static char hx_cbuf[65536];\n"
    "static int64_t hx_clen;\n"
    "static hx_str hx_concat(hx_str a, hx_str b) {\n"
    "  hx_str s;\n"
    "  if (hx_clen + a.n + b.n > (int64_t)sizeof(hx_cbuf))\n"
    "    hx_panic(\"cadena intermedia mayor que el buffer de concatenacion\", 52);\n"
    "  memcpy(hx_cbuf + hx_clen, a.p, (size_t)a.n);\n"
    "  memcpy(hx_cbuf + hx_clen + a.n, b.p, (size_t)b.n);\n"
    "  hx_clen += a.n + b.n;\n"
    "  s.p = hx_cbuf + hx_clen - (a.n + b.n);\n"
    "  s.n = a.n + b.n;\n"
    "  return s;\n"
    "}\n"
    "static hx_str hx_str_of_i64(int64_t v) {\n"
    "  char buf[24];\n"
    "  int n = 0;\n"
    "  uint64_t u;\n"
    "  hx_str s;\n"
    "  if (v < 0) { buf[n++] = '-'; u = (uint64_t)(-(v + 1)) + 1u; }\n"
    "  else u = (uint64_t)v;\n"
    "  hx_u64_to_dec(u, buf, &n);\n"
    "  s.p = buf;\n"
    "  s.n = n;\n"
    "  return s;\n"
    "}\n"
    "static hx_str hx_str_of_f64(double d) {\n"
    "  static char buf[64];\n"
    "  int n = 0, i;\n"
    "  uint64_t ip;\n"
    "  double frac;\n"
    "  hx_str s;\n"
    "  if (d < 0) { buf[n++] = '-'; d = -d; }\n"
    "  ip = (uint64_t)d;\n"
    "  frac = d - (double)ip;\n"
    "  hx_u64_to_dec(ip, buf, &n);\n"
    "  if (frac > 0) {\n"
    "    buf[n++] = '.';\n"
    "    for (i = 0; i < 9; i++) {\n"
    "      int dd;\n"
    "      frac *= 10.0;\n"
    "      dd = (int)frac;\n"
    "      if (dd > 9) dd = 9;\n"
    "      buf[n++] = (char)('0' + dd);\n"
    "      frac -= (double)dd;\n"
    "      if (frac <= 0) break;\n"
    "    }\n"
    "  }\n"
    "  s.p = buf;\n"
    "  s.n = n;\n"
    "  return s;\n"
    "}\n"
    "static hx_str hx_str_of_bool(hx_bool v) { return hx_lit(v ? \"true\" : \"false\", v ? 4 : 5); }\n";

typedef struct {
    const char *name;
    const char *cname;
} HxIntrinName;

static const HxIntrinName hx_intrin_names[] = {
    {"Len", "len_str"},       {"IsEmpty", "is_empty_str"}, {"Upper", "upper_str"},
    {"Lower", "lower_str"},   {"Trim", "trim_str"},        {"Slice", "str_slice"},
    {"At", "str_at"},         {"Repeat", "repeat_str"},    {NULL, NULL},
};

static const char *hx_intrin_cname(const char *name) {
    for (int i = 0; hx_intrin_names[i].name; i++)
        if (!hx_ascii_casecmp(hx_intrin_names[i].name, name)) return hx_intrin_names[i].cname;
    return "unknown_intrinsic";
}

static const char *hx_zero_fn(HxEmit *e, HxTy *t) {
    switch (t ? t->kind : TY_INT) {
        case TY_VOID: return "int32";
        case TY_BOOL: return "int32";
        case TY_INT: return "int32";
        case TY_I64:
        case TY_DURATION: return "int64";
        case TY_FLOAT: return "f64";
        case TY_STRING: return "str";
        case TY_ARRAY: return "span";
        case TY_NAMED:
            if (t->decl)
                return hx_arena_sprintf(e->arena, "rec_%s", hx_sym_str(t->decl->name));
            return "int32";
        default: return "int32";
    }
}

static void hx_expr_str(HxEmit *e, HxExpr *x, int prec, HxBuf *b);
static void hx_stmt_emit(HxEmit *e, HxStmt *s, int ind);
static void hx_body(HxEmit *e, HxStmtVec *body, int ind);
static void hx_scan_stmt(HxEmit *e, HxStmt *s);
static void hx_scan_body(HxEmit *e, HxStmtVec *body);

static void hx_scan_expr(HxEmit *e, HxExpr *x) {
    if (!x) return;
    if (x->kind == EX_STR) e->uses_string = 1;
    if (x->kind == EX_VEC || x->vec_component || (x->ty && (x->ty->kind == TY_VEC2 || x->ty->kind == TY_VEC3 || x->ty->kind == TY_VEC4 || x->ty->kind == TY_MAT4 || x->ty->kind == TY_QUAT)))
        e->uses_vec = 1;
    if (x->kind == EX_TRY || x->is_ok_ctor || x->is_err_ctor) e->uses_result = 1;
    if (x->ty && hx_ty_is_result(x->ty)) e->uses_result = 1;
    switch (x->kind) {
        case EX_CALL:
            hx_scan_expr(e, x->call.callee);
            for (int i = 0; i < x->call.args.len; i++) hx_scan_expr(e, x->call.args.data[i].value);
            break;
        case EX_BIN:
            hx_scan_expr(e, x->bin.lhs);
            hx_scan_expr(e, x->bin.rhs);
            break;
        case EX_UN: hx_scan_expr(e, x->un.operand); break;
        case EX_INDEX:
            hx_scan_expr(e, x->index.base);
            hx_scan_expr(e, x->index.start);
            hx_scan_expr(e, x->index.end);
            break;
        case EX_TRY: hx_scan_expr(e, x->try.inner); break;
        case EX_VEC:
            for (int i = 0; i < x->vec.len; i++) hx_scan_expr(e, x->vec.items[i]);
            break;
        default: break;
    }
    for (int i = 0; i < x->n_segs; i++) hx_scan_expr(e, x->segs[i].hole);
}

static void hx_scan_body(HxEmit *e, HxStmtVec *body) {
    for (int i = 0; i < body->len; i++) hx_scan_stmt(e, &body->data[i]);
}

static void hx_scan_stmt(HxEmit *e, HxStmt *s) {
    HxStmtVec *body = NULL;
    switch (s->kind) {
        case ST_EXPR: hx_scan_expr(e, s->expr); break;
        case ST_ASSIGN:
            hx_scan_expr(e, s->assign.target);
            hx_scan_expr(e, s->assign.value);
            break;
        case ST_DIM:
            hx_scan_expr(e, s->dim.init);
            if (s->dim.ty && s->dim.ty->kind == TY_ARRAY && s->dim.ty->size > 0)
                e->uses_arena = 1;
            break;
        case ST_CONST: hx_scan_expr(e, s->konst.value); break;
        case ST_PRINT:
            for (int k = 0; k < s->print.items.len; k++)
                hx_scan_expr(e, s->print.items.data[k].expr);
            break;
        case ST_IF:
            hx_scan_expr(e, s->if_.cond);
            hx_scan_body(e, &s->if_.then);
            for (int k = 0; k < s->if_.n_elifs; k++) {
                hx_scan_expr(e, s->if_.elifs[k].cond);
                hx_scan_body(e, &s->if_.elifs[k].then);
            }
            hx_scan_body(e, &s->if_.else_);
            break;
        case ST_WHILE:
            hx_scan_expr(e, s->while_.cond);
            hx_scan_body(e, &s->while_.body);
            break;
        case ST_FOR:
            hx_scan_expr(e, s->for_.start);
            hx_scan_expr(e, s->for_.end);
            hx_scan_expr(e, s->for_.step);
            hx_scan_body(e, &s->for_.body);
            break;
        case ST_RETURN: hx_scan_expr(e, s->ret.value); break;
        case ST_EXIT:
            e->uses_exit = 1;
            hx_scan_expr(e, s->exit_.code);
            break;
        case ST_BLOCK: body = &s->block.stmts; break;
        case ST_ARENA: body = &s->arena.body; break;
        case ST_DEFER: hx_scan_body(e, &s->inner); break;
        default: break;
    }
    if (body) hx_scan_body(e, body);
}

static void hx_put_c_string(HxBuf *b, const char *raw, int len) {
    hx_buf_put(b, "\"", 1);
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)raw[i];
        switch (c) {
            case '\n': hx_buf_put(b, "\\n", 2); break;
            case '\t': hx_buf_put(b, "\\t", 2); break;
            case '\r': hx_buf_put(b, "\\r", 2); break;
            case '"': hx_buf_put(b, "\\\"", 2); break;
            case '\\': hx_buf_put(b, "\\\\", 2); break;
            case '\0': hx_buf_put(b, "\\0", 2); break;
            default:
                if (c < 0x20 || c == 0x7f) hx_buf_printf(b, "\\x%02x", c);
                else hx_buf_put(b, (const char *)&c, 1);
        }
    }
    hx_buf_put(b, "\"", 1);
}

static const char *hx_kind(const char *kind, const char *op, const char *sfx) {
    static char buf[64];
    snprintf(buf, sizeof(buf), "hx_%s_%s%s", op, kind, sfx);
    return buf;
}

static void hx_expr_base(HxEmit *e, HxExpr *x, int pre, HxBuf *b);

static int hx_vec_len(HxTy *t) {
    if (!t) return 0;
    switch (t->kind) {
        case TY_VEC2: return 2;
        case TY_VEC3: return 3;
        case TY_VEC4:
        case TY_QUAT: return 4;
        default: return 0;
    }
}

static int hx_vec_comp_index(char c) {
    if (c == 'x' || c == 'r') return 0;
    if (c == 'y' || c == 'g') return 1;
    if (c == 'z' || c == 'b') return 2;
    return 3;
}

static void hx_emit_vec_access(HxEmit *e, HxExpr *x, HxBuf *b) {
    static const char comp[] = "xyzw";
    e->uses_vec = 1;
    int pre = x->prefix_len > 0 ? x->prefix_len : 1;
    const char *name = x->method ? hx_sym_str(x->method) : "x";
    int n = (int)strlen(name);
    if (n == 1) {
        hx_buf_str(b, "(");
        hx_expr_base(e, x, pre, b);
        hx_buf_printf(b, ").%c", comp[hx_vec_comp_index(name[0])]);
        return;
    }
    hx_buf_printf(b, "%s(", n == 2 ? "hx_v2" : n == 3 ? "hx_v3" : "hx_v4");
    for (int i = 0; i < n; i++) {
        if (i) hx_buf_str(b, ", ");
        hx_buf_str(b, "(");
        hx_expr_base(e, x, pre, b);
        hx_buf_printf(b, ").%c", comp[hx_vec_comp_index(name[i])]);
    }
    hx_buf_str(b, ")");
}

static int hx_prec_of(HxExpr *x) {
    if (!x) return 100;
    switch (x->kind) {
        case EX_BIN: {
            HxBinOp op = x->bin.op;
            if (op == OP_AND) return 2;
            if (op == OP_OR || op == OP_XOR) return 1;
            if (op == OP_EQ || op == OP_NE || op == OP_LT || op == OP_LE || op == OP_GT ||
                op == OP_GE)
                return 3;
            if (op == OP_ADD || op == OP_SUB || op == OP_CONCAT || op == OP_ADDW ||
                op == OP_SUBW || op == OP_ADDS || op == OP_SUBS)
                return 4;
            return 5;
        }
        case EX_UN: return x->un.op == UOP_NOT ? 1 : 6;
        default: return 7;
    }
}

static const char *hx_print_fn(HxEmit *e, HxTy *t) {
    if (!t) return "hx_print_i64";
    switch (t->kind) {
        case TY_VEC2: e->uses_vec = 1; return "hx_print_vec2";
        case TY_VEC3: e->uses_vec = 1; return "hx_print_vec3";
        case TY_VEC4:
        case TY_QUAT: e->uses_vec = 1; return "hx_print_vec4";
        case TY_MAT4: e->uses_vec = 1; return "hx_print_vec4";
        case TY_FLOAT: return "hx_print_f64";
        case TY_BOOL: return "hx_print_bool";
        case TY_STRING: return "hx_print_str";
        case TY_DURATION: return "hx_print_duration";
        default: return "hx_print_i64";
    }
}

static const char *hx_str_of_fn(HxTy *t) {
    if (!t) return "hx_str_of_i64";
    switch (t->kind) {
        case TY_FLOAT: return "hx_str_of_f64";
        case TY_BOOL: return "hx_str_of_bool";
        default: return "hx_str_of_i64";
    }
}

static void hx_str_seg_expr(HxEmit *e, HxStrSeg *sg, HxBuf *b) {
    if (!sg->hole) {
        hx_buf_str(b, "hx_lit(");
        hx_put_c_string(b, sg->text, sg->len);
        hx_buf_printf(b, ", %d)", sg->len);
        return;
    }
    HxExpr *h = sg->hole;
    if (h->kind == EX_STR && !h->has_holes) {
        hx_buf_str(b, "hx_lit(");
        hx_put_c_string(b, h->str.raw, h->str.len);
        hx_buf_printf(b, ", %d)", h->str.len);
        return;
    }
    if (h->ty && h->ty->kind == TY_STRING) {
        hx_expr_str(e, h, 0, b);
        return;
    }
    HxBuf inner = {e->arena, NULL, 0, 0};
    hx_expr_str(e, h, 0, &inner);
    hx_buf_printf(b, "%s(%s)", hx_str_of_fn(h->ty), inner.data ? inner.data : "0");
    free(inner.data);
}

static void hx_expr_base(HxEmit *e, HxExpr *x, int pre, HxBuf *b) {
    if (x->deref) hx_buf_printf(b, "(*hx_v_%s)", hx_sym_str(x->path.parts.data[0].name));
    else hx_buf_printf(b, "hx_v_%s", hx_sym_str(x->path.parts.data[0].name));
    for (int i = 1; i < pre; i++) hx_buf_printf(b, ".%s", hx_sym_str(x->path.parts.data[i].name));
}

static void hx_expr_str(HxEmit *e, HxExpr *x, int prec, HxBuf *b) {
    int myprec = hx_prec_of(x);
    int paren = myprec < prec;
    if (paren) hx_buf_put(b, "(", 1);
    switch (x->kind) {
        case EX_INT:
            hx_buf_printf(b, "INT32_C(%lld)", (long long)x->ival);
            break;
        case EX_FLOAT:
            if (x->fval == (double)(int64_t)x->fval && x->fval < 1e15 && x->fval > -1e15)
                hx_buf_printf(b, "%.1f", x->fval);
            else
                hx_buf_printf(b, "%.17g", x->fval);
            break;
        case EX_DURATION:
            hx_buf_printf(b, "INT64_C(%lld)", (long long)x->ival);
            break;
        case EX_BOOL:
        case EX_NIL:
            hx_buf_str(b, x->ival ? "1" : "0");
            break;
        case EX_STR: {
            e->uses_string = 1;
            if (!x->has_holes) {
                hx_buf_str(b, "hx_lit(");
                hx_put_c_string(b, x->str.raw, x->str.len);
                hx_buf_printf(b, ", %d)", x->str.len);
                break;
            }
            HxBuf acc = {e->arena, NULL, 0, 0};
            hx_str_seg_expr(e, &x->segs[0], &acc);
            for (int k = 1; k < x->n_segs; k++) {
                HxBuf piece = {e->arena, NULL, 0, 0};
                hx_str_seg_expr(e, &x->segs[k], &piece);
                HxBuf joined = {e->arena, NULL, 0, 0};
                hx_buf_printf(&joined, "hx_concat(%s, %s)", acc.data ? acc.data : "hx_lit(\"\", 0)",
                              piece.data ? piece.data : "hx_lit(\"\", 0)");
                free(acc.data);
                free(piece.data);
                acc = joined;
            }
            hx_buf_put(b, acc.data ? acc.data : "hx_lit(\"\", 0)", acc.len);
            free(acc.data);
            break;
        }
        case EX_PATH: {
            if (x->vec_component > 0) {
                hx_emit_vec_access(e, x, b);
                break;
            }
            int pre = x->prefix_len > 0 ? x->prefix_len : (x->path.parts.len > 1 ? 1 : 1);
            if (x->is_intrin) {
                hx_buf_printf(b, "hx_%s(hx_v_%s", hx_intrin_cname(hx_sym_str(x->method)),
                              hx_sym_str(x->path.parts.data[0].name));
                for (int i = 1; i < pre; i++)
                    hx_buf_printf(b, ".%s", hx_sym_str(x->path.parts.data[i].name));
                hx_buf_str(b, ")");
                break;
            }
            if (x->deref) hx_buf_printf(b, "(*hx_v_%s)", hx_sym_str(x->path.parts.data[0].name));
            else hx_buf_printf(b, "hx_v_%s", hx_sym_str(x->path.parts.data[0].name));
            for (int i = 1; i < pre; i++)
                hx_buf_printf(b, ".%s", hx_sym_str(x->path.parts.data[i].name));
            break;
        }
        case EX_CALL: {
            HxExpr *callee = x->call.callee;
            if (x->is_ok_ctor || x->is_err_ctor) {
                e->uses_result = 1;
                const char *fn = x->is_ok_ctor ? "hx_ok" : "hx_err";
                const char *sfx = "s";
                if (x->payload_ty && x->payload_ty->kind == TY_FLOAT) sfx = "f";
                else if (x->payload_ty &&
                         (x->payload_ty->kind == TY_STRING || x->payload_ty->kind == TY_NAMED))
                    sfx = "s";
                else sfx = "i";
                hx_buf_printf(b, "%s_%s(", fn, sfx);
                if (x->call.args.len) hx_expr_str(e, x->call.args.data[0].value, 0, b);
                hx_buf_str(b, ")");
                break;
            }
            if (x->is_intrin == 3) {
                e->uses_vec = 1;
                const char *nm = hx_sym_str(x->method);
                if (!hx_ascii_casecmp(nm, "DOT")) {
                    hx_buf_str(b, "hx_dot(");
                    hx_expr_str(e, x->call.args.data[0].value, 0, b);
                    hx_buf_str(b, ", ");
                    hx_expr_str(e, x->call.args.data[1].value, 0, b);
                    hx_buf_str(b, ")");
                    break;
                }
                if (!hx_ascii_casecmp(nm, "CROSS")) {
                    hx_buf_str(b, "hx_cross(");
                    hx_expr_str(e, x->call.args.data[0].value, 0, b);
                    hx_buf_str(b, ", ");
                    hx_expr_str(e, x->call.args.data[1].value, 0, b);
                    hx_buf_str(b, ")");
                    break;
                }
                if (!hx_ascii_casecmp(nm, "LEN")) {
                    hx_buf_str(b, "hx_len3(");
                    hx_expr_str(e, x->call.args.data[0].value, 0, b);
                    hx_buf_str(b, ")");
                    break;
                }
                hx_buf_str(b, "hx_normalized(");
                hx_expr_str(e, x->call.args.data[0].value, 0, b);
                hx_buf_str(b, ")");
                break;
            }
            if (x->is_intrin) {
                hx_buf_printf(b, "hx_%s(", hx_intrin_cname(hx_sym_str(x->method)));
                hx_expr_str(e, x->recv, 0, b);
                for (int i = 0; i < x->call.args.len; i++) {
                    hx_buf_str(b, ", ");
                    hx_expr_str(e, x->call.args.data[i].value, 0, b);
                }
                hx_buf_str(b, ")");
                break;
            }
            if (callee->kind == EX_PATH)
                hx_buf_printf(b, "hx_call_%s(",
                              hx_sym_str(callee->path.parts.data[callee->path.parts.len - 1].name));
            else {
                hx_expr_str(e, callee, 7, b);
                hx_buf_str(b, "(");
            }
            for (int i = 0; i < x->call.args.len; i++) {
                if (i) hx_buf_str(b, ", ");
                int wants_ref = 0;
                if (x->ret_arg_refs) wants_ref = x->ret_arg_refs[i];
                if (wants_ref && x->call.args.data[i].value->kind == EX_PATH &&
                    !x->call.args.data[i].value->deref) {
                    hx_buf_printf(b, "&hx_v_%s",
                                  hx_sym_str(x->call.args.data[i].value->path.parts.data[0].name));
                } else {
                    hx_expr_str(e, x->call.args.data[i].value, 0, b);
                }
            }
            hx_buf_str(b, ")");
            break;
        }
        case EX_BIN: {
            const char *o = hx_binop_spelling(x->bin.op);
            if (x->bin.op == OP_CONCAT) {
                e->uses_string = 1;
                hx_buf_str(b, "hx_concat(");
                hx_expr_str(e, x->bin.lhs, 0, b);
                hx_buf_str(b, ", ");
                hx_expr_str(e, x->bin.rhs, 0, b);
                hx_buf_str(b, ")");
                break;
            }
            const char *cop = o;
            if (x->bin.op == OP_ADDW) cop = "+";
            if (x->bin.op == OP_SUBW) cop = "-";
            if (x->bin.op == OP_NE) cop = "!=";
            if (x->bin.op == OP_EQ) cop = "==";
            if (x->bin.op == OP_AND) cop = "&&";
            if (x->bin.op == OP_OR) cop = "||";
            if (x->bin.op == OP_XOR) cop = "^";
            if (x->bin.op == OP_MOD) cop = "%";
            if (x->ty && hx_vec_len(x->ty)) {
                e->uses_vec = 1;
                if (x->bin.op == OP_ADD || x->bin.op == OP_SUB) {
                    hx_buf_printf(b, "hx_%s3(", x->bin.op == OP_ADD ? "add" : "sub");
                    hx_expr_str(e, x->bin.lhs, 0, b);
                    hx_buf_str(b, ", ");
                    hx_expr_str(e, x->bin.rhs, 0, b);
                    hx_buf_str(b, ")");
                    break;
                }
                if (x->bin.op == OP_MUL) {
                    hx_buf_str(b, "hx_scale3(");
                    hx_expr_str(e, x->bin.lhs, 0, b);
                    hx_buf_str(b, ", (float)(");
                    hx_expr_str(e, x->bin.rhs, 0, b);
                    hx_buf_str(b, "))");
                    break;
                }
            }
            if (x->bin.op == OP_ADD || x->bin.op == OP_SUB || x->bin.op == OP_ADDS ||
                x->bin.op == OP_SUBS) {
                const char *kind = x->ty && x->ty->kind == TY_FLOAT   ? "f64"
                                   : x->ty && x->ty->kind == TY_I64    ? "i64"
                                   : x->ty && x->ty->kind == TY_DURATION ? "i64"
                                                                        : "i32";
                const char *fn = NULL;
                if (x->bin.op == OP_ADD) fn = hx_kind(kind, "add", "");
                if (x->bin.op == OP_SUB) fn = hx_kind(kind, "sub", "");
                if (x->bin.op == OP_ADDS) fn = hx_kind(kind, "add_sat", "");
                if (x->bin.op == OP_SUBS) fn = hx_kind(kind, "sub_sat", "");
                if (fn && strcmp(kind, "f64")) {
                    hx_buf_printf(b, "%s(", fn);
                    hx_expr_str(e, x->bin.lhs, 0, b);
                    hx_buf_str(b, ", ");
                    hx_expr_str(e, x->bin.rhs, 0, b);
                    hx_buf_str(b, ")");
                    break;
                }
            }
            hx_expr_str(e, x->bin.lhs, myprec, b);
            hx_buf_printf(b, " %s ", cop);
            hx_expr_str(e, x->bin.rhs, myprec + 1, b);
            break;
        }
        case EX_UN:
            if (x->un.op == UOP_NOT) {
                hx_buf_str(b, "!");
                hx_expr_str(e, x->un.operand, 7, b);
            } else {
                hx_buf_str(b, "-");
                hx_expr_str(e, x->un.operand, 7, b);
            }
            break;
        case EX_INDEX: {
            HxTy *et = x->ty;
            hx_buf_str(b, "((");
            hx_buf_str(b, hx_c_ty(e, et));
            hx_buf_str(b, "*)(");
            hx_expr_str(e, x->index.base, 7, b);
            hx_buf_str(b, ").data)");
            if (x->index.start) {
                hx_buf_str(b, "[");
                hx_expr_str(e, x->index.start, 0, b);
                hx_buf_str(b, "]");
            }
            break;
        }
        case EX_MEMB: {
            hx_expr_str(e, x->member.base, 7, b);
            hx_buf_printf(b, ".%s", hx_sym_str(x->member.name));
            break;
        }
        case EX_TRY:
            hx_error(e->diags, x->span, "E0403",
                     "'?' sólo puede ser el valor completo de una asignación, DIM o RETURN");
            hx_buf_str(b, "0");
            break;
        case EX_VEC: {
            e->uses_vec = 1;
            const char *fn = x->ty && x->ty->kind == TY_VEC2   ? "hx_v2"
                             : x->ty && x->ty->kind == TY_VEC3 ? "hx_v3"
                             : x->ty && x->ty->kind == TY_QUAT ? "hx_v4"
                             : x->ty && x->ty->kind == TY_VEC4 ? "hx_v4"
                                                               : "hx_v4s";
            if (x->vec.len == 1) {
                hx_buf_printf(b, "%s(", fn);
                hx_expr_str(e, x->vec.items[0], 0, b);
                hx_buf_str(b, ")");
                break;
            }
            hx_buf_printf(b, "%s(", fn);
            for (int i = 0; i < x->vec.len; i++) {
                if (i) hx_buf_str(b, ", ");
                if (x->vec.items[i]->kind == EX_INT) hx_buf_str(b, "(float)");
                hx_expr_str(e, x->vec.items[i], 0, b);
            }
            hx_buf_str(b, ")");
            break;
        }
        default:
            hx_buf_str(b, "0");
            break;
    }
    if (paren) hx_buf_put(b, ")", 1);
}


static void hx_indent(HxBuf *b, int n);

static int hx_size_of(HxEmit *e, HxTy *t) {
    switch (t ? t->kind : TY_INT) {
        case TY_BOOL:
        case TY_INT: return 4;
        case TY_I64:
        case TY_DURATION:
        case TY_FLOAT: return 8;
        case TY_STRING: return 16;
        case TY_ARRAY: return 16;
        case TY_PTR:
        case TY_REF: return 8;
        case TY_NAMED: {
            if (hx_ty_is_result(t)) return 32;
            if (!t->decl) return 4;
            int total = 0;
            for (int i = 0; i < t->decl->fields.len; i++)
                total += hx_size_of(e, t->decl->fields.data[i].ty);
            return total ? total : 4;
        }
        default: return 4;
    }
}

static int hx_body_has_defer(HxStmtVec *body) {
    for (int i = 0; i < body->len; i++)
        if (body->data[i].kind == ST_DEFER) return 1;
    return 0;
}

static int hx_count_defers(HxStmtVec *body) {
    int n = 0;
    for (int i = 0; i < body->len; i++) {
        HxStmt *st = &body->data[i];
        if (st->kind == ST_DEFER) {
            n++;
            continue;
        }
        switch (st->kind) {
            case ST_IF:
                n += hx_count_defers(&st->if_.then);
                for (int k = 0; k < st->if_.n_elifs; k++)
                    n += hx_count_defers(&st->if_.elifs[k].then);
                n += hx_count_defers(&st->if_.else_);
                break;
            case ST_WHILE: n += hx_count_defers(&st->while_.body); break;
            case ST_FOR: n += hx_count_defers(&st->for_.body); break;
            case ST_ARENA: n += hx_count_defers(&st->arena.body); break;
            case ST_BLOCK: n += hx_count_defers(&st->block.stmts); break;
            case ST_MATCH:
                for (int k = 0; k < st->match.cases.len; k++)
                    n += hx_count_defers(&st->match.cases.data[k].body);
                n += hx_count_defers(&st->match.else_body);
                break;
            default: break;
        }
    }
    return n;
}

static int hx_push_epilogue(HxEmit *e) {
    int lab = e->tmp++;
    if (e->epilogue_depth < 16) e->epilogue_stack[e->epilogue_depth++] = lab;
    return lab;
}

static void hx_epilogue_end(HxEmit *e, int lab, HxStmtVec *body, int ind) {
    HxBuf *b = &e->out;
    hx_indent(b, ind);
    hx_buf_printf(b, "hx_epilogue_%d:\n", lab);
    for (int i = body->len - 1; i >= 0; i--) {
        if (body->data[i].kind != ST_DEFER) continue;
        for (int k = 0; k < body->data[i].inner.len; k++)
            hx_stmt_emit(e, &body->data[i].inner.data[k], ind);
    }
    if (e->epilogue_depth > 0) e->epilogue_depth--;
    hx_indent(b, ind);
    if (e->epilogue_depth > 0)
        hx_buf_printf(b, "if (hx_mode) goto hx_epilogue_%d;\n", e->epilogue_stack[e->epilogue_depth - 1]);
    else
        hx_buf_str(b, "\n");
}

static void hx_block(HxEmit *e, HxStmtVec *body, int ind) {
    int lab = hx_body_has_defer(body) ? hx_push_epilogue(e) : -1;
    hx_body(e, body, ind);
    if (lab >= 0) {
        hx_indent(&e->out, ind);
        hx_buf_str(&e->out, "hx_mode = 0;\n");
        hx_indent(&e->out, ind);
        hx_buf_printf(&e->out, "goto hx_epilogue_%d;\n", lab);
        hx_epilogue_end(e, lab, body, ind);
    }
}

static const char *hx_payload_field(HxTy *t) {
    if (!t) return "i";
    if (t->kind == TY_FLOAT) return "f";
    if (t->kind == TY_STRING) return "s";
    return "i";
}

static void hx_emit_pattern_test(HxEmit *e, HxPattern *pat, const char *subj, HxTy *subj_ty) {
    HxBuf *b = &e->out;
    switch (pat->kind) {
        case PAT_WILDCARD:
        case PAT_BIND: hx_buf_str(b, "1"); return;
        case PAT_LITERAL: {
            hx_buf_printf(b, "hx_pat_eq_%s(%s, ", hx_payload_field(subj_ty), subj);
            hx_expr_str(e, pat->lit, 0, b);
            hx_buf_str(b, ")");
            return;
        }
        case PAT_RANGE:
            hx_buf_printf(b, "(%s >= ", subj);
            hx_expr_str(e, pat->lo, 0, b);
            hx_buf_str(b, " && ");
            hx_buf_printf(b, "%s <= ", subj);
            hx_expr_str(e, pat->hi, 0, b);
            hx_buf_str(b, ")");
            return;
        case PAT_CONSTRUCTOR: {
            int is_or = !hx_ascii_casecmp(hx_sym_str(pat->ctor), "__or__");
            if (is_or) {
                for (int i = 0; i < pat->args.len; i++) {
                    if (i) hx_buf_str(b, " || ");
                    hx_emit_pattern_test(e, &pat->args.data[i], subj, subj_ty);
                }
                return;
            }
            if (!hx_ascii_casecmp(hx_sym_str(pat->ctor), "Ok")) {
                hx_buf_str(b, "hx_is_ok(");
                hx_buf_str(b, subj);
                hx_buf_str(b, ")");
                return;
            }
            if (!hx_ascii_casecmp(hx_sym_str(pat->ctor), "Err")) {
                hx_buf_printf(b, "!hx_is_ok(%s)", subj);
                return;
            }
            hx_buf_str(b, "1");
            return;
        }
    }
}

static void hx_emit_pattern_bind(HxEmit *e, HxPattern *pat, const char *subj, HxTy *subj_ty,
                                 int ind) {
    HxBuf *b = &e->out;
    switch (pat->kind) {
        case PAT_BIND:
            hx_indent(b, ind);
            hx_buf_printf(b, "%s hx_v_%s = %s;\n", hx_c_ty(e, subj_ty), hx_sym_str(pat->name),
                          subj);
            return;
        case PAT_CONSTRUCTOR: {
            const char *cn = hx_sym_str(pat->ctor);
            if (!hx_ascii_casecmp(cn, "__or__")) {
                for (int i = 0; i < pat->args.len; i++)
                    hx_emit_pattern_bind(e, &pat->args.data[i], subj, subj_ty, ind);
                return;
            }
            if (!hx_ascii_casecmp(cn, "Ok") && pat->args.len == 1) {
                HxTy *pt = subj_ty && subj_ty->elem ? subj_ty->elem : NULL;
                hx_indent(b, ind);
                hx_buf_printf(b, "%s hx_v_%s = %s.%s;\n", hx_c_ty(e, pt),
                              hx_sym_str(pat->args.data[0].name), subj,
                              hx_payload_field(pt));
                return;
            }
            if (!hx_ascii_casecmp(cn, "Err") && pat->args.len == 1) {
                hx_indent(b, ind);
                hx_buf_printf(b, "hx_str hx_v_%s = %s.s;\n", hx_sym_str(pat->args.data[0].name),
                              subj);
                return;
            }
            for (int i = 0; i < pat->args.len; i++)
                hx_emit_pattern_bind(e, &pat->args.data[i], subj, subj_ty, ind);
            return;
        }
        default: return;
    }
}

static void hx_emit_propagating(HxEmit *e, HxExpr *val, int ind, const char *dest,
                               int dest_is_call) {
    HxBuf *b = &e->out;
    e->uses_result = 1;
    int id = e->tmp++;
    hx_indent(b, ind);
    hx_buf_printf(b, "hx_result hx_t%d = ", id);
    hx_expr_str(e, val->try.inner, 0, b);
    hx_buf_str(b, ";\n");
    hx_indent(b, ind);
    int tgt = e->epilogue_depth > 0 ? e->epilogue_stack[e->epilogue_depth - 1] : e->epilogue;
    if (e->ret_is_result && tgt >= 0)
        hx_buf_printf(b,
                      "if (hx_t%d.tag) { hx_ret_r = hx_t%d; hx_mode = 1; goto hx_epilogue_%d; }\n",
                      id, id, tgt);
    else if (e->ret_is_result)
        hx_buf_printf(b, "if (hx_t%d.tag) return hx_t%d;\n", id, id);
    else hx_buf_printf(b, "if (hx_t%d.tag) hx_propagate_top(hx_t%d);\n", id, id);
    hx_indent(b, ind);
    hx_buf_str(b, dest);
    hx_buf_printf(b, "hx_t%d.%s", id, hx_payload_field(val->ty));
    hx_buf_str(b, dest_is_call ? ");\n" : ";\n");
}

static void hx_indent(HxBuf *b, int n) {
    for (int i = 0; i < n; i++) hx_buf_put(b, "  ", 2);
}


static void hx_emit_print(HxEmit *e, HxStmt *s, int ind) {
    HxBuf *b = &e->out;
    for (int i = 0; i < s->print.items.len; i++) {
        HxPrintItem *it = &s->print.items.data[i];
        if (it->sep == PS_SEP_SEMI) continue;
        if (it->sep == PS_SEP_COMMA) {
            hx_indent(b, ind);
            hx_buf_str(b, "hx_out(\"        \", 8);\n");
            continue;
        }
        HxExpr *x = it->expr;
        if (x->kind == EX_TRY && x->propagate) {
            char *dest = hx_arena_sprintf(e->arena, "%s(", hx_print_fn(e, x->ty));
            hx_emit_propagating(e, x, ind, dest, 1);
            continue;
        }
        hx_indent(b, ind);
        if (x->kind == EX_STR && x->has_holes) {
            e->uses_string = 1;
            hx_buf_str(b, "{\n");
            for (int k = 0; k < x->n_segs; k++) {
                HxStrSeg *sg = &x->segs[k];
                hx_indent(b, ind + 1);
                if (!sg->hole) {
                    hx_buf_str(b, "hx_out(");
                    hx_put_c_string(b, sg->text, sg->len);
                    hx_buf_printf(b, ", %d);\n", sg->len);
                    continue;
                }
                HxExpr *h = sg->hole;
                if (h->kind == EX_STR && !h->has_holes) {
                    hx_buf_str(b, "hx_out(");
                    hx_put_c_string(b, h->str.raw, h->str.len);
                    hx_buf_printf(b, ", %d);\n", h->str.len);
                    continue;
                }
                HxBuf inner = {e->arena, NULL, 0, 0};
                hx_expr_str(e, h, 0, &inner);
                hx_buf_printf(b, "%s(%s);\n", hx_print_fn(e, h->ty), inner.data ? inner.data : "0");
                free(inner.data);
            }
            hx_indent(b, ind);
            hx_buf_str(b, "}\n");
            continue;
        }
        if (x->kind == EX_STR) {
            e->uses_string = 1;
            hx_buf_str(b, "hx_out(");
            hx_put_c_string(b, x->str.raw, x->str.len);
            hx_buf_printf(b, ", %d);\n", x->str.len);
            continue;
        }
        hx_buf_printf(b, "%s(", hx_print_fn(e, x->ty));
        hx_expr_str(e, x, 0, b);
        hx_buf_str(b, ");\n");
    }
    if (s->print.items.len) {
        int last = s->print.items.data[s->print.items.len - 1].sep;
        if (last != PS_SEP_SEMI) {
            hx_indent(b, ind);
            hx_buf_str(b, "hx_out(\"\\n\", 1);\n");
        }
    }
}

static void hx_emit_assign(HxEmit *e, HxStmt *s, int ind) {
    HxBuf *b = &e->out;
    if (s->assign.value && s->assign.value->kind == EX_TRY && s->assign.value->propagate) {
        HxBuf *db = &e->out;
        hx_buf_put(db, "", 0);
        hx_buf_str(db, "");
        hx_buf_str(db, "");
        char *dest = hx_arena_sprintf(e->arena, "%s", "");
        if (s->assign.target->kind == EX_INDEX) {
            dest = hx_arena_strdup(e->arena, "");
            hx_expr_str(e, s->assign.target, 0, db);
            dest = hx_arena_sprintf(e->arena, "%s", db->data + db->len);
            dest = hx_arena_sprintf(e->arena, "%.*s = ", db->len, db->data);
            db->len = 0;
            if (db->data) db->data[0] = 0;
        } else {
            dest = hx_arena_sprintf(e->arena, "hx_v_%s = ", hx_sym_str(s->assign.target->path.parts.data[0].name));
        }
        hx_emit_propagating(e, s->assign.value, ind, dest, 0);
        return;
    }
    hx_indent(b, ind);
    if (s->assign.compound && s->assign.op == OP_ADD && s->assign.target->ty &&
        s->assign.target->ty->kind == TY_STRING) {
        e->uses_string = 1;
        hx_expr_str(e, s->assign.target, 0, b);
        hx_buf_str(b, " = hx_concat(");
        hx_expr_str(e, s->assign.target, 0, b);
        hx_buf_str(b, ", ");
        hx_expr_str(e, s->assign.value, 0, b);
        hx_buf_str(b, ");\n");
        return;
    }
    hx_expr_str(e, s->assign.target, 0, b);
    if (!s->assign.compound) hx_buf_str(b, " = ");
    else if (s->assign.op == OP_ADD) hx_buf_str(b, " += ");
    else hx_buf_printf(b, " %s= ", hx_binop_spelling(s->assign.op));
    hx_expr_str(e, s->assign.value, 0, b);
    hx_buf_str(b, ";\n");
}

static void hx_stmt_emit(HxEmit *e, HxStmt *s, int ind) {
    HxBuf *b = &e->out;
    switch (s->kind) {
        case ST_NOP: break;
        case ST_EXPR:
            hx_indent(b, ind);
            hx_expr_str(e, s->expr, 0, b);
            hx_buf_str(b, ";\n");
            break;
        case ST_DIM:
            if (s->dim.init && s->dim.init->kind == EX_TRY && s->dim.init->propagate) {
                char *dest = hx_arena_sprintf(e->arena, "%s hx_v_%s = ",
                                              hx_c_ty(e, s->dim.ty), hx_sym_str(s->dim.name));
                hx_emit_propagating(e, s->dim.init, ind, dest, 0);
                break;
            }
            if (s->dim.ty && s->dim.ty->kind == TY_ARRAY && !s->dim.init) {
                e->uses_arena = 1;
                const char *el = hx_c_ty(e, s->dim.ty->elem);
                int64_t cnt = s->dim.ty->size;
                hx_indent(b, ind);
                hx_buf_printf(b, "hx_span hx_v_%s = hx_span_make(hx_arena_alloc(",
                              hx_sym_str(s->dim.name));
                if (e->arena_depth) hx_buf_printf(b, "&%s", e->arena_stack[e->arena_depth - 1]);
                else hx_buf_str(b, "&hx_static_arena");
                hx_buf_printf(b, ", %lld), %lld);\n",
                              (long long)(cnt * (int64_t)hx_size_of(e, s->dim.ty->elem)),
                              (long long)cnt);
                (void)el;
                break;
            }
            hx_indent(b, ind);
            hx_buf_printf(b, "%s hx_v_%s = ", hx_c_ty(e, s->dim.ty), hx_sym_str(s->dim.name));
            if (s->dim.init) {
                if (s->dim.ty && s->dim.ty->kind == TY_FLOAT && s->dim.init->kind == EX_INT)
                    hx_buf_str(b, "(double)");
                hx_expr_str(e, s->dim.init, 0, b);
            } else if (s->dim.ty && s->dim.ty->kind == TY_STRING) {
                e->uses_string = 1;
                hx_buf_str(b, "hx_lit(\"\", 0)");
            } else if (s->dim.ty && s->dim.ty->kind == TY_ARRAY) {
                hx_buf_str(b, "hx_span_make(NULL, 0)");
            } else {
                hx_buf_printf(b, "hx_zero_%s()", hx_zero_fn(e, s->dim.ty));
            }
            hx_buf_str(b, ";\n");
            break;
        case ST_CONST:
            hx_indent(b, ind);
            hx_buf_printf(b, "const %s hx_v_%s = ", hx_c_ty(e, s->konst.ty),
                          hx_sym_str(s->konst.name));
            hx_expr_str(e, s->konst.value, 0, b);
            hx_buf_str(b, ";\n");
            break;
        case ST_ASSIGN: hx_emit_assign(e, s, ind); break;
        case ST_PRINT: hx_emit_print(e, s, ind); break;
        case ST_IF:
            hx_indent(b, ind);
            hx_buf_str(b, "if (");
            hx_expr_str(e, s->if_.cond, 0, b);
            hx_buf_str(b, ") {\n");
            hx_block(e, &s->if_.then, ind + 1);
            hx_indent(b, ind);
            hx_buf_str(b, "}");
            for (int i = 0; i < s->if_.n_elifs; i++) {
                hx_buf_str(b, " else if (");
                hx_expr_str(e, s->if_.elifs[i].cond, 0, b);
                hx_buf_str(b, ") {\n");
                hx_block(e, &s->if_.elifs[i].then, ind + 1);
                hx_indent(b, ind);
                hx_buf_str(b, "}");
            }
            if (s->if_.has_else) {
                hx_buf_str(b, " else {\n");
                hx_block(e, &s->if_.else_, ind + 1);
                hx_indent(b, ind);
                hx_buf_str(b, "}");
            }
            hx_buf_str(b, "\n");
            break;
        case ST_WHILE:
            hx_indent(b, ind);
            hx_buf_str(b, "while (1) {\n");
            hx_indent(b, ind + 1);
            hx_buf_str(b, "if (!(");
            hx_expr_str(e, s->while_.cond, 0, b);
            hx_buf_str(b, ")) break;\n");
            hx_block(e, &s->while_.body, ind + 1);
            hx_indent(b, ind);
            hx_buf_str(b, "}\n");
            break;
        case ST_FOR: {
            HxTy *vt = s->for_.start && s->for_.start->ty ? s->for_.start->ty
                                                          : hx_ty_builtin(e->arena, TY_INT);
            int neg = s->for_.step && s->for_.step->kind == EX_INT && s->for_.step->ival < 0;
            hx_indent(b, ind);
            hx_buf_printf(b, "for (%s hx_v_%s = ", hx_c_ty(e, vt), hx_sym_str(s->for_.var));
            if (vt->kind == TY_FLOAT && s->for_.start->kind == EX_INT) hx_buf_str(b, "(double)");
            hx_expr_str(e, s->for_.start, 0, b);
            hx_buf_printf(b, "; hx_v_%s %s ", hx_sym_str(s->for_.var), neg ? ">=" : "<=");
            hx_expr_str(e, s->for_.end, 0, b);
            hx_buf_printf(b, "; hx_v_%s += ", hx_sym_str(s->for_.var));
            if (s->for_.step) hx_expr_str(e, s->for_.step, 0, b);
            else hx_buf_str(b, "1");
            hx_buf_str(b, ") {\n");
            hx_block(e, &s->for_.body, ind + 1);
            hx_indent(b, ind);
            hx_buf_str(b, "}\n");
            break;
        }
        case ST_RETURN:
            if (s->ret.value && s->ret.value->kind == EX_TRY && s->ret.value->propagate) {
                hx_emit_propagating(e, s->ret.value, ind, "return ", 0);
                break;
            }
            hx_indent(b, ind);
            int target = e->epilogue_depth > 0 ? e->epilogue_stack[e->epilogue_depth - 1]
                                               : e->epilogue;
            if (target >= 0) {
                if (s->ret.value) {
                    hx_buf_printf(b, "hx_ret_%s = ", e->ret_field);
                    hx_expr_str(e, s->ret.value, 0, b);
                    hx_buf_printf(b, ";\n");
                    hx_indent(b, ind);
                    hx_buf_str(b, "hx_mode = 1;\n");
                    hx_indent(b, ind);
                    hx_buf_printf(b, "goto hx_epilogue_%d;\n", target);
                } else {
                    hx_indent(b, ind);
                    hx_buf_str(b, "hx_mode = 1;\n");
                    hx_indent(b, ind);
                    hx_buf_printf(b, "goto hx_epilogue_%d;\n", target);
                }
                break;
            }
            if (s->ret.value) {
                hx_buf_str(b, "return ");
                hx_expr_str(e, s->ret.value, 0, b);
                hx_buf_str(b, ";\n");
            } else {
                hx_buf_str(b, "return;\n");
            }
            break;
        case ST_BREAK:
            hx_indent(b, ind);
            hx_buf_str(b, "break;\n");
            break;
        case ST_CONTINUE:
            hx_indent(b, ind);
            hx_buf_str(b, "continue;\n");
            break;
        case ST_EXIT:
            e->uses_exit = 1;
            hx_indent(b, ind);
            hx_buf_str(b, "hx_exit(");
            if (s->exit_.code) hx_expr_str(e, s->exit_.code, 0, b);
            else hx_buf_str(b, "0");
            hx_buf_str(b, ");\n");
            break;
        case ST_MATCH: {
            e->uses_result = 1;
            int id = e->tmp++;
            char *subj = s->match.subject_name
                             ? hx_arena_strdup(e->arena, hx_sym_str(s->match.subject_name))
                             : hx_arena_sprintf(e->arena, "hx_m%d", id);
            hx_indent(b, ind);
            hx_buf_printf(b, "%s %s = ", hx_c_ty(e, s->match.subject->ty), subj);
            hx_expr_str(e, s->match.subject, 0, b);
            hx_buf_str(b, ";\n");
            for (int i = 0; i < s->match.cases.len; i++) {
                HxMatchCase *mc = &s->match.cases.data[i];
                hx_indent(b, ind);
                hx_buf_str(b, i == 0 ? "if (" : "else if (");
                hx_emit_pattern_test(e, mc->pattern, subj, s->match.subject->ty);
                if (mc->guard) {
                    hx_buf_str(b, " && (");
                    hx_expr_str(e, mc->guard, 0, b);
                    hx_buf_str(b, ")");
                }
                hx_buf_str(b, ") {\n");
                hx_emit_pattern_bind(e, mc->pattern, subj, s->match.subject->ty, ind + 1);
                hx_block(e, &mc->body, ind + 1);
                hx_indent(b, ind);
                hx_buf_str(b, "}\n");
            }
            if (s->match.has_else) {
                hx_indent(b, ind);
                hx_buf_str(b, s->match.cases.len ? "else {\n" : "{");
                hx_block(e, &s->match.else_body, ind + 1);
                hx_indent(b, ind);
                hx_buf_str(b, "}\n");
            }
            break;
        }
        case ST_BLOCK: {
            int lab = hx_body_has_defer(&s->block.stmts) ? hx_push_epilogue(e) : -1;
            hx_body(e, &s->block.stmts, ind);
            if (lab >= 0) hx_epilogue_end(e, lab, &s->block.stmts, ind);
            break;
        }
        case ST_ARENA: {
            e->uses_arena = 1;
            int id = e->tmp++;
            char *aname = hx_arena_sprintf(e->arena, "hx_a%d", id);
            if (e->arena_depth < 16) e->arena_stack[e->arena_depth++] = aname;
            hx_indent(b, ind);
            hx_buf_printf(b, "hx_arena %s;\n", aname);
            hx_indent(b, ind);
            hx_buf_printf(b, "hx_arena_init(&%s);\n", aname);
            int lab = hx_body_has_defer(&s->arena.body) ? hx_push_epilogue(e) : -1;
            hx_body(e, &s->arena.body, ind);
            if (lab >= 0) hx_epilogue_end(e, lab, &s->arena.body, ind);
            hx_indent(b, ind);
            hx_buf_printf(b, "hx_arena_free(&%s);\n", aname);
            if (e->arena_depth) e->arena_depth--;
            break;
        }
        case ST_DEFER: break;
    }
}

static void hx_body(HxEmit *e, HxStmtVec *body, int ind) {
    for (int i = 0; i < body->len; i++) hx_stmt_emit(e, &body->data[i], ind);
}

static void hx_emit_decls(HxEmit *e, HxModule *m) {
    HxBuf *b = &e->out;
    if (!m->types.len) return;
    for (int i = 0; i < m->types.len; i++)
        hx_buf_printf(b, "typedef struct hx_T_%s hx_T_%s;\n", hx_sym_str(m->types.data[i].name),
                      hx_sym_str(m->types.data[i].name));
    for (int i = 0; i < m->types.len; i++) {
        HxTypeDecl *t = &m->types.data[i];
        hx_buf_printf(b, "struct hx_T_%s {\n", hx_sym_str(t->name));
        for (int j = 0; j < t->fields.len; j++)
            hx_buf_printf(b, "  %s %s;\n", hx_c_ty(e, t->fields.data[j].ty),
                          hx_sym_str(t->fields.data[j].name));
        hx_buf_str(b, "};\n");
    }
}

static void hx_emit_protos(HxEmit *e, HxModule *m) {
    HxBuf *b = &e->out;
    for (int i = 0; i < m->funcs.len; i++) {
        HxFunc *f = &m->funcs.data[i];
        hx_buf_printf(b, "static %s hx_call_%s(", hx_c_ty(e, f->ret), hx_sym_str(f->name));
        if (!f->params.len) hx_buf_str(b, "void");
        for (int j = 0; j < f->params.len; j++) {
            if (j) hx_buf_str(b, ", ");
            hx_buf_printf(b, "%s hx_v_%s", hx_c_ty(e, f->params.data[j].ty),
                          hx_sym_str(f->params.data[j].name));
        }
        hx_buf_str(b, ");\n");
    }
}

static void hx_emit_func(HxEmit *e, HxFunc *f) {
    HxBuf *b = &e->out;
    hx_buf_printf(b, "static %s hx_call_%s(", hx_c_ty(e, f->ret), hx_sym_str(f->name));
    if (!f->params.len) hx_buf_str(b, "void");
    for (int j = 0; j < f->params.len; j++) {
        if (j) hx_buf_str(b, ", ");
        hx_buf_printf(b, "%s hx_v_%s", hx_c_ty(e, f->params.data[j].ty),
                      hx_sym_str(f->params.data[j].name));
    }
    hx_buf_str(b, ") {\n");
    e->ret_is_result = f->ret && hx_ty_is_result(f->ret);
    int ndefers = hx_count_defers(&f->body);
    e->epilogue = ndefers ? e->tmp++ : -1;
    e->epilogue_depth = 0;
    if (e->epilogue >= 0 && e->epilogue_depth < 16)
        e->epilogue_stack[e->epilogue_depth++] = e->epilogue;
    if (e->epilogue >= 0) {
        hx_buf_str(b, "  int hx_mode = 0;\n");
        e->ret_field = e->ret_is_result ? "r"
                       : !f->ret || f->ret->kind == TY_VOID  ? "i"
                       : f->ret->kind == TY_FLOAT             ? "f"
                       : f->ret->kind == TY_STRING            ? "s"
                                                           : "i";
        if (e->ret_is_result) {
            hx_buf_str(b, "  hx_result hx_ret_r;\n");
        } else if (!f->ret || f->ret->kind == TY_VOID) {
            hx_buf_str(b, "  int64_t hx_ret_i;\n");
        } else {
            hx_buf_printf(b, "  %s hx_ret_%s;\n",
                          f->ret->kind == TY_FLOAT ? "double"
                          : f->ret->kind == TY_STRING ? "hx_str"
                                                      : "int64_t",
                          e->ret_field);
        }
    }
    if (f->is_tail_loop) {
        HxStmt *last = &f->body.data[f->body.len - 1];
        hx_buf_str(b, "  for (;;) {\n");
        for (int i = 0; i + 1 < f->body.len; i++) hx_stmt_emit(e, &f->body.data[i], 2);
        for (int j = 0; j < f->params.len; j++) {
            hx_buf_printf(b, "    hx_v_%s = ", hx_sym_str(f->params.data[j].name));
            hx_expr_str(e, last->ret.value->call.args.data[j].value, 0, b);
            hx_buf_str(b, ";\n");
        }
        hx_buf_str(b, "  }\n");
    } else {
        hx_body(e, &f->body, 1);
    }
    if (e->epilogue >= 0) {
        hx_buf_str(b, "hx_mode = 0;\n");
        hx_buf_str(b, "goto ");
        hx_buf_printf(b, "hx_epilogue_%d;\n", e->epilogue);
        hx_buf_printf(b, "hx_epilogue_%d:\n", e->epilogue);
        for (int i = f->body.len - 1; i >= 0; i--) {
            if (f->body.data[i].kind != ST_DEFER) continue;
            for (int k = 0; k < f->body.data[i].inner.len; k++)
                hx_stmt_emit(e, &f->body.data[i].inner.data[k], 1);
        }
        hx_buf_str(b, "  ");
        if (!f->ret || f->ret->kind == TY_VOID) hx_buf_str(b, "return;\n");
        else hx_buf_printf(b, "return hx_ret_%s;\n", e->ret_field);
    }
    e->epilogue_depth = 0;
    e->epilogue = -1;
    e->ret_is_result = 0;
    hx_buf_str(b, "}\n\n");
}

int hx_emit_unit(HxArena *arena, HxUnit *unit, const char *out_path, HxProfile profile,
                 const char *out_bin, int keep_asm, const char *keep_asm_path) {
    HxEmit e;
    memset(&e, 0, sizeof(e));
    e.arena = arena;
    e.unit = unit;
    e.diags = unit->diags;
    e.profile = profile;
    e.out.arena = arena;

    for (int i = 0; i < unit->modules.len; i++) {
        HxModule *m = &unit->modules.data[i];
        hx_scan_body(&e, &m->top);
        for (int j = 0; j < m->funcs.len; j++) {
            hx_scan_body(&e, &m->funcs.data[j].body);
            if (hx_ty_is_result(m->funcs.data[j].ret)) e.uses_result = 1;
            for (int q = 0; q < m->funcs.data[j].params.len; q++)
                if (hx_ty_is_result(m->funcs.data[j].params.data[q].ty)) e.uses_result = 1;
        }
        for (int t = 0; t < m->types.len; t++) {
            for (int q = 0; q < m->types.data[t].fields.len; q++)
                if (hx_ty_is_result(m->types.data[t].fields.data[q].ty)) e.uses_result = 1;
        }
    }

    HxBuf *b = &e.out;
    hx_buf_printf(b, "/* generado por hxc %s -- no editar */\n", HX_VERSION);
    hx_buf_str(b, "#include <stdint.h>\n#include <stddef.h>\n#include <string.h>\n");
    hx_buf_str(b, profile == HX_PROFILE_FREESTANDING ? HX_RT_FREESTANDING : HX_RT_LIBC);
    if (profile == HX_PROFILE_FREESTANDING) {
        hx_buf_str(b, HX_RT_FREESTANDING_MEM);
        hx_buf_str(b, HX_RT_RAWALLOC_FREESTANDING);
    } else {
        hx_buf_str(b, HX_RT_RAWALLOC_LIBC);
    }
    hx_buf_str(b, HX_RT_TYPES);
    hx_buf_str(b, HX_RT_CORE);
    if (e.uses_vec) {
        hx_buf_str(b, HX_RT_VEC_PRE);
        hx_buf_str(b, HX_RT_VEC);
    }
    if (e.uses_string) hx_buf_str(b, HX_RT_STRING);
    hx_buf_str(b, HX_RT_ZERO);
    if (e.uses_result) {
        hx_buf_str(b, HX_RT_RESULT);
    }
    hx_buf_str(b, HX_RT_CHECKED);
    if (e.uses_arena) {
        hx_buf_str(b, HX_RT_ARENA);
        hx_buf_str(b, "static hx_arena hx_static_arena;\n");
        hx_buf_str(b, "static void hx_static_init(void) { hx_arena_init(&hx_static_arena); }\n");
    }
    if (e.uses_string) {
        hx_buf_str(b, HX_RT_STRFUNS);
        hx_buf_str(b, HX_RT_STRMORE);
    }

    for (int i = 0; i < unit->modules.len; i++) {
        hx_emit_decls(&e, &unit->modules.data[i]);
        HxModule *m = &unit->modules.data[i];
        for (int t = 0; t < m->types.len; t++)
            hx_buf_printf(&e.out,
                          "static hx_T_%s hx_zero_rec_%s(void) { hx_T_%s v; "
                          "__builtin_memset(&v, 0, sizeof(v)); return v; }\n",
                          hx_sym_str(m->types.data[t].name), hx_sym_str(m->types.data[t].name),
                          hx_sym_str(m->types.data[t].name));
    }
    for (int i = 0; i < unit->modules.len; i++) {
        HxModule *m = &unit->modules.data[i];
        for (int k = 0; k < m->consts.len; k++) {
            HxConst *kc = &m->consts.data[k];
            if (!kc->value) continue;
            hx_buf_printf(&e.out, "static const %s hx_v_%s = ", hx_c_ty(&e, kc->ty),
                          hx_sym_str(kc->name));
            hx_expr_str(&e, kc->value, 0, &e.out);
            hx_buf_str(&e.out, ";\n");
        }
    }
    for (int i = 0; i < unit->modules.len; i++) hx_emit_protos(&e, &unit->modules.data[i]);

    if (profile == HX_PROFILE_FREESTANDING) hx_buf_str(b, "void _start(void);\n");
    if (e.uses_arena) hx_buf_str(b, "static void hx_static_init(void);\n");
    hx_buf_str(b, "static int32_t hx_main(void);\n");
    hx_buf_str(b, "static int32_t hx_main(void) {\n");
    if (e.uses_arena) hx_buf_str(b, "  hx_static_init();\n");
    for (int i = 0; i < unit->modules.len; i++) {
        HxModule *m = &unit->modules.data[i];
        if (m->is_entry) hx_body(&e, &m->top, 1);
    }
    hx_buf_str(b, "  return 0;\n}\n\n");

    for (int i = 0; i < unit->modules.len; i++) {
        HxModule *m = &unit->modules.data[i];
        for (int j = 0; j < m->funcs.len; j++) hx_emit_func(&e, &m->funcs.data[j]);
    }

    if (profile == HX_PROFILE_FREESTANDING)
        hx_buf_str(b, "void _start(void) {\n  hx_exit((int)hx_main());\n  __builtin_unreachable();\n}\n");
    else
        hx_buf_str(b, "int main(void) { return (int)hx_main(); }\n");

    int rc = hx_write_file(out_path, b->data, b->len);
    if (rc != 0) {
        fprintf(stderr, "hx: no se pudo escribir %s\n", out_path);
        free(b->data);
        return 1;
    }
    if (keep_asm && keep_asm_path) {
        FILE *f = fopen(keep_asm_path, "wb");
        if (f) {
            fwrite(b->data, 1, b->len, f);
            fclose(f);
        }
    }
    free(b->data);
    (void)out_bin;
    return 0;
}