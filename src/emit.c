#include "hx/emit.h"
#include "hx/mono.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    HxTy *from;
    HxTy *to;
} HxIterMap;

typedef struct {
    HxArena *arena;
    HxUnit *unit;
    HxDiagBag *diags;
    HxProfile profile;
    HxBuf out;
    int tmp;
    int uses_string;
    int uses_index;  /* a.At(i), que comprueba el rango */
    int uses_maybe;  /* MAYBE<T> */
    int uses_darr;   /* ARRAY[T] dinámico: Push necesita la arena */
    int uses_arena;
    int uses_iter;
    int uses_enum;
    int uses_net;
    int iter_depth;
    int iter_n; /* contador global de variables de iterador por funcion */
    /* tipos de elemento usados por iteradores, para generar sus ayudantes */
    HX_VEC_ANON(HxTy) iter_elems;
    /* tipos interiores de los MAYBE usados, para generar un typedef por tipo */
    HX_VEC_ANON(const char *) maybe_inners;
    /* pares (valor, MAYBE resultante) de cada m.Map(f), para el ayudante */
    HX_VEC_ANON(HxIterMap) maybe_maps;
    /* tipos de elemento de los ARRAY[T] a los que se les hace Push: cada uno
       necesita su ayudante, porque pasar el valor y tomar su direccion dentro
       de una expresion no se puede escribir en C */
    HX_VEC_ANON(const char *) darr_push_types;
    /* pares (origen, resultado) de los MAP que cambian el tipo */
    HX_VEC_ANON(HxIterMap) iter_maps;
    int uses_exit;
    int uses_result;
    int uses_vec;
    int uses_shift;
    int uses_tostring;
    int ret_is_result;
    int epilogue;
    const char *ret_field;
    int arena_depth;
    const char *arena_stack[16];
    int epilogue_stack[16];
    int epilogue_depth;
    HxStmtVec *match_cases;
} HxEmit;

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
        case TY_ARRAY: return t->size < 0 ? "hx_darr" : "hx_span";
        case TY_VEC2: return "hx_vec2";
        case TY_VEC3: return "hx_vec3";
        case TY_VEC4: return "hx_vec4";
        case TY_MAT4: return "hx_mat4";
        case TY_QUAT: return "hx_quat";
        case TY_MAYBE: {
            /* un struct distinto por tipo: si no, dos MAYBE de INNER distintos
               se confundirian en C */
            const char *inner = hx_c_ty(e, t->elem);
            int ya = 0;
            for (int i = 0; i < e->maybe_inners.len; i++)
                if (!strcmp(e->maybe_inners.data[i], inner)) ya = 1;
            if (!ya && e->maybe_inners.len < 32) {
                e->uses_maybe = 1;
                HX_VEC_PUSH(e->maybe_inners, inner);
            }
            return hx_arena_sprintf(e->arena, "hx_maybe_%s", inner);
        }
        case TY_NAMED:
            if (hx_ty_is_result(t)) return "hx_result";
            /* un ENUM es un INT con nombre: en C basta con int32_t */
            if (t->decl && !t->decl->is_enum)
                return hx_arena_sprintf(e->arena, "hx_T_%s", hx_sym_str(t->decl->name));
            return "int32_t";
        case TY_ITER: return "hx_iter";
        /* sin default: un kind nuevo emitiendo "int32_t" en silencio era justo
           el fallo que busco cerrar */
        case TY_UNKNOWN: return "int32_t";
    }
    return "int32_t";
}

/* El typedef de ITER va siempre: una cabecera de modulo puede declarar
   "extern hx_iter hx_call_X(...)" aunque el programa que la genera no itere
   nunca, y sin el typedef el C de mas abajo no compila. */
static const char *HX_RT_ITER_TYPE =
    "typedef struct { void *estado; int32_t (*paso)(void *estado, void *out); } hx_iter;\n";

static const char *HX_RT_ITER =
    "typedef struct { int32_t i, fin; } hx_iter_st_rango_i;\n"
    "static inline int32_t hx_iter_paso_rango_i(void *st, void *out) {\n"
    "  hx_iter_st_rango_i *s = (hx_iter_st_rango_i *)st;\n"
    "  if (s->i >= s->fin) return 0;\n"
    "  *(int32_t *)out = s->i;\n"
    "  s->i++;\n"
    "  return 1;\n"
    "}\n"
    "static inline hx_iter hx_iter_rango_i(hx_arena *a, int32_t desde, int32_t hasta) {\n"
    "  hx_iter_st_rango_i *s = (hx_iter_st_rango_i *)hx_arena_alloc(a, sizeof(*s));\n"
    "  s->i = desde; s->fin = hasta;\n"
    "  hx_iter it; it.estado = s; it.paso = hx_iter_paso_rango_i; return it;\n"
    "}\n"
    "typedef struct { double i, fin, paso_; } hx_iter_st_rango_f;\n"
    "static inline int32_t hx_iter_paso_rango_f(void *st, void *out) {\n"
    "  hx_iter_st_rango_f *s = (hx_iter_st_rango_f *)st;\n"
    "  if (s->i >= s->fin) return 0;\n"
    "  *(double *)out = s->i;\n"
    "  s->i += s->paso_;\n"
    "  return 1;\n"
    "}\n"
    "static inline hx_iter hx_iter_rango_f(hx_arena *a, double desde, double hasta, double paso) {\n"
    "  hx_iter_st_rango_f *s = (hx_iter_st_rango_f *)hx_arena_alloc(a, sizeof(*s));\n"
    "  s->i = desde; s->fin = hasta; s->paso_ = paso;\n"
    "  hx_iter it; it.estado = s; it.paso = hx_iter_paso_rango_f; return it;\n"
    "}\n";

/* La capacidad net habla con el kernel: en freestanding son syscalls directas
   de Linux x86_64 y en el perfil libc las llamadas de la biblioteca. */
static const char *HX_NET_PRE =
    "typedef struct { uint16_t puerto; uint32_t addr; } hx_inet;\n"
    "static inline hx_span hx_span_of(hx_str s) { hx_span sp; sp.data = s.p; sp.len = s.n; return sp; }\n"
    "static inline hx_inet hx_inet_de(int64_t puerto, uint32_t a1, uint32_t a2, uint32_t a3,\n"
    "                              uint32_t a4) {\n"
    "  hx_inet r; r.puerto = (uint16_t)puerto;\n"
    "  r.addr = (a1 << 24) | (a2 << 16) | (a3 << 8) | a4; return r;\n"
    "}\n";

static const char *HX_RT_NET_FREESTANDING =
    "#if defined(__linux__) && defined(__x86_64__)\n"
    "typedef struct { uint16_t f; uint16_t puerto; uint32_t addr; uint64_t pad; } hx_sockaddr;\n"
    "static inline uint16_t hx_htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }\n"
    "static inline uint32_t hx_htonl(uint32_t v) {\n"
    "  return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) | ((v >> 8) & 0xff00u) | ((v >> 24) & 0xffu);\n"
    "}\n"
    "static inline hx_sockaddr hx_sa(hx_inet a) {\n"
    "  hx_sockaddr s; s.f = 2; s.puerto = hx_htons(a.puerto); s.addr = hx_htonl(a.addr); s.pad = 0; return s;\n"
    "}\n"
    "static inline int64_t hx_sys3(long n, long a1, long a2, long a3) {\n"
    "  long r;\n"
    "  __asm__ volatile(\"syscall\" : \"=a\"(r) : \"a\"(n), \"D\"(a1), \"S\"(a2), \"d\"(a3)\n"
    "                   : \"rcx\", \"r11\", \"memory\");\n"
    "  return r;\n"
    "}\n"
    "static inline int64_t hx_sys6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {\n"
    "  long r;\n"
    "  register long r10 __asm__(\"r10\") = a4;\n"
    "  register long r8 __asm__(\"r8\") = a5;\n"
    "  register long r9 __asm__(\"r9\") = a6;\n"
    "  __asm__ volatile(\"syscall\" : \"=a\"(r) : \"a\"(n), \"D\"(a1), \"S\"(a2), \"d\"(a3),\n"
    "                   \"r\"(r10), \"r\"(r8), \"r\"(r9) : \"rcx\", \"r11\", \"memory\");\n"
    "  return r;\n"
    "}\n"
    "static inline int64_t hx_socket(int64_t tipo) { return hx_sys3(41L, 2L, tipo, 0L); }\n"
    "static inline int64_t hx_bind(int64_t fd, hx_inet a) {\n"
    "  hx_sockaddr sa = hx_sa(a); return hx_sys3(49L, fd, (long)&sa, 16L);\n"
    "}\n"
    "static inline int64_t hx_listen(int64_t fd, int64_t cola) { return hx_sys3(50L, fd, cola, 0L); }\n"
    "static inline int64_t hx_connect(int64_t fd, hx_inet a) {\n"
    "  hx_sockaddr sa = hx_sa(a); return hx_sys3(42L, fd, (long)&sa, 16L);\n"
    "}\n"
    "static inline int64_t hx_accept(int64_t fd, int64_t *quien) {\n"
    "  hx_sockaddr sa;\n"
    "  int64_t c = hx_sys3(43L, fd, (long)&sa, 16L);\n"
    "  if (quien && c >= 0) { quien[0] = (int64_t)hx_htons(sa.puerto); quien[1] = (int64_t)hx_htonl(sa.addr); }\n"
    "  return c;\n"
    "}\n"
    "static inline int64_t hx_sendto(int64_t fd, hx_span buf, hx_inet a) {\n"
    "  hx_sockaddr sa = hx_sa(a);\n"
    "  return hx_sys6(44L, fd, (long)buf.data, buf.len, 0L, (long)&sa, 16L);\n"
    "}\n"
    "static inline int64_t hx_recvfrom(int64_t fd, hx_span buf, hx_inet *de) {\n"
    "  hx_sockaddr sa;\n"
    "  int64_t n = hx_sys6(45L, fd, (long)buf.data, buf.len, 0L, (long)&sa, 16L);\n"
    "  if (de && n >= 0) { de->puerto = hx_htons(sa.puerto); de->addr = hx_htonl(sa.addr); }\n"
    "  return n;\n"
    "}\n"
    "static inline int64_t hx_close_fd(int64_t fd) { return hx_sys3(3L, fd, 0L, 0L); }\n"
    "static inline int64_t hx_net_error(void) { return 0; }\n"
    "static hx_str hx_net_recv(int64_t fd, hx_arena *a) {\n"
    "  char buf[1024];\n"
    "  int64_t n = hx_recvfrom(fd, ((hx_span){buf, 1024}), 0);\n"
    "  if (n <= 0) return hx_lit(\"\", 0);\n"
    "  char *p = (char *)hx_arena_alloc(a, n + 1);\n"
    "  __builtin_memcpy(p, buf, (size_t)n);\n"
    "  p[n] = 0;\n"
    "  hx_str s; s.p = p; s.n = n; return s;\n"
    "}\n"
    "static hx_str hx_net_recv_de(int64_t fd, hx_arena *a, int64_t *puerto, int64_t *ip) {\n"
    "  char buf[1024];\n"
    "  hx_inet de;\n"
    "  int64_t n = hx_recvfrom(fd, ((hx_span){buf, 1024}), &de);\n"
    "  if (n <= 0) return hx_lit(\"\", 0);\n"
    "  char *p = (char *)hx_arena_alloc(a, n + 1);\n"
    "  __builtin_memcpy(p, buf, (size_t)n);\n"
    "  p[n] = 0;\n"
    "  if (puerto) *puerto = (int64_t)de.puerto;\n"
    "  if (ip) *ip = (int64_t)de.addr;\n"
    "  hx_str s; s.p = p; s.n = n; return s;\n"
    "}\n";

/* los stubs van aparte: ISO C99 obliga a soportar literales de 4095 bytes */
static const char *HX_RT_NET_STUBS =
    "#else\n"
    "static inline int64_t hx_socket(int64_t t) { return -1; }\n"
    "static inline int64_t hx_bind(int64_t f, hx_inet a) { (void)f; (void)a; return -1; }\n"
    "static inline int64_t hx_listen(int64_t f, int64_t c) { (void)f; (void)c; return -1; }\n"
    "static inline int64_t hx_connect(int64_t f, hx_inet a) { (void)f; (void)a; return -1; }\n"
    "static inline int64_t hx_accept(int64_t f, int64_t *q) { (void)f; (void)q; return -1; }\n"
    "static inline int64_t hx_sendto(int64_t f, hx_span b, hx_inet a) { (void)f; (void)b; (void)a; return -1; }\n"
    "static inline int64_t hx_recvfrom(int64_t f, hx_span b, hx_inet *d) {\n"
    "  (void)f; (void)b; (void)d; return -1;\n"
    "}\n"
    "static inline int64_t hx_close_fd(int64_t f) { (void)f; return -1; }\n"
    "static inline int64_t hx_net_error(void) { return 0; }\n"
    "static hx_str hx_net_recv(int64_t f, hx_arena *a) { (void)f; (void)a; return hx_lit(\"\", 0); }\n"
    "static hx_str hx_net_recv_de(int64_t f, hx_arena *a, int64_t *p, int64_t *i) {\n"
    "  (void)f; (void)a; (void)p; (void)i; return hx_lit(\"\", 0);\n"
    "}\n"
    "#endif\n";

static const char *HX_RT_NET_LIBC =
    "#include <sys/socket.h>\n"
    "#include <netinet/in.h>\n"
    "#include <unistd.h>\n"
    "#include <errno.h>\n"
    "static inline uint16_t hx_htons(uint16_t v) { return htons(v); }\n"
    "static inline int64_t hx_socket(int64_t t) { return (int64_t)socket(AF_INET, (int)t, 0); }\n"
    "static inline int64_t hx_bind(int64_t fd, hx_inet a) {\n"
    "  struct sockaddr_in sa;\n"
    "  __builtin_memset(&sa, 0, sizeof(sa));\n"
    "  sa.sin_family = AF_INET; sa.sin_port = hx_htons(a.puerto); sa.sin_addr.s_addr = htonl(a.addr);\n"
    "  return (int64_t)bind((int)fd, (struct sockaddr *)&sa, sizeof(sa));\n"
    "}\n"
    "static inline int64_t hx_sendto(int64_t fd, hx_span buf, hx_inet a) {\n"
    "  struct sockaddr_in sa;\n"
    "  __builtin_memset(&sa, 0, sizeof(sa));\n"
    "  sa.sin_family = AF_INET; sa.sin_port = hx_htons(a.puerto); sa.sin_addr.s_addr = htonl(a.addr);\n"
    "  return (int64_t)sendto((int)fd, buf.data, (size_t)buf.len, 0, (struct sockaddr *)&sa, sizeof(sa));\n"
    "}\n"
    "static inline int64_t hx_recvfrom(int64_t fd, hx_span buf, hx_inet *de) {\n"
    "  struct sockaddr_in sa;\n"
    "  __builtin_memset(&sa, 0, sizeof(sa));\n"
    "  socklen_t sl = sizeof(sa);\n"
    "  int64_t n = (int64_t)recvfrom((int)fd, buf.data, (size_t)buf.len, 0, (struct sockaddr *)&sa, &sl);\n"
    "  if (de) { de->puerto = ntohs(sa.sin_port); de->addr = ntohl(sa.sin_addr.s_addr); }\n"
    "  return n;\n"
    "}\n"
    "static inline int64_t hx_connect(int64_t fd, hx_inet a) {\n"
    "  struct sockaddr_in sa;\n"
    "  __builtin_memset(&sa, 0, sizeof(sa));\n"
    "  sa.sin_family = AF_INET; sa.sin_port = hx_htons(a.puerto); sa.sin_addr.s_addr = htonl(a.addr);\n"
    "  return (int64_t)connect((int)fd, (struct sockaddr *)&sa, sizeof(sa));\n"
    "}\n"
    "static inline int64_t hx_listen(int64_t fd, int64_t cola) { return (int64_t)listen((int)fd, (int)cola); }\n"
    "static inline int64_t hx_accept(int64_t fd, int64_t *de) {\n"
    "  struct sockaddr_in sa;\n"
    "  __builtin_memset(&sa, 0, sizeof(sa));\n"
    "  socklen_t sl = sizeof(sa);\n"
    "  int64_t c = (int64_t)accept((int)fd, (struct sockaddr *)&sa, &sl);\n"
    "  if (de) { de[0] = (int64_t)ntohs(sa.sin_port); de[1] = (int64_t)ntohl(sa.sin_addr.s_addr); }\n"
    "  return c;\n"
    "}\n"
    "static inline int64_t hx_close_fd(int64_t fd) { return (int64_t)close((int)fd); }\n"
    "static inline int64_t hx_net_error(void) { return (int64_t)errno; }\n"
    "static hx_str hx_net_recv(int64_t fd, hx_arena *a) {\n"
    "  char buf[1024];\n"
    "  int64_t n = hx_recvfrom(fd, ((hx_span){buf, 1024}), 0);\n"
    "  if (n <= 0) return hx_lit(\"\", 0);\n"
    "  char *p = (char *)hx_arena_alloc(a, n + 1);\n"
    "  __builtin_memcpy(p, buf, (size_t)n);\n"
    "  p[n] = 0;\n"
    "  hx_str s; s.p = p; s.n = n; return s;\n"
    "}\n"
    "static hx_str hx_net_recv_de(int64_t fd, hx_arena *a, int64_t *puerto, int64_t *ip) {\n"
    "  char buf[1024];\n"
    "  hx_inet de;\n"
    "  int64_t n = hx_recvfrom(fd, ((hx_span){buf, 1024}), &de);\n"
    "  if (n <= 0) return hx_lit(\"\", 0);\n"
    "  char *p = (char *)hx_arena_alloc(a, n + 1);\n"
    "  __builtin_memcpy(p, buf, (size_t)n);\n"
    "  p[n] = 0;\n"
    "  if (puerto) *puerto = (int64_t)de.puerto;\n"
    "  if (ip) *ip = (int64_t)de.addr;\n"
    "  hx_str s; s.p = p; s.n = n; return s;\n"
    "}\n";

/* En Windows el perfil freestanding tambien emite sus memcpy y memset, y ahi ya
   los declara <string.h>: si se vuelven a declarar aqui, el prototipo no coincide
   (size_t contra unsigned long) y el C no compila. La solucion no es renombrar los
   nuestros, que entonces no serian los de libc, sino declarar el prototipo con
   el mismo tipo que usa el resto del runtime al llamarlos. */
static const char *HX_RT_MEM_DECL = "void *memcpy(void *, const void *, size_t);\n"
    "void *memset(void *, int, size_t);\n";

static const char *HX_RT_FREESTANDING_MEM =
    "void *memcpy(void *d, const void *s, size_t n) {\n"
    "  unsigned char *dd = (unsigned char *)d;\n"
    "  const unsigned char *ss = (const unsigned char *)s;\n"
    "  size_t i;\n"
    "  for (i = 0; i < n; i++) dd[i] = ss[i];\n"
    "  return d;\n"
    "}\n"
    "void *memset(void *d, int c, size_t n) {\n"
    "  unsigned char *dd = (unsigned char *)d;\n"
    "  size_t i;\n"
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
    "static inline void hx_write(int fd, const void *p, int64_t n) {\n"
    "  const char *s = (const char *)p;\n"
    "  while (n > 0) {\n"
    "    long r = hx_sys_write(fd, s, n);\n"
    "    if (r <= 0) return;\n"
    "    s += r;\n"
    "    n -= r;\n"
    "  }\n"
    "}\n"
    "static inline void hx_out(const char *p, int64_t n) { hx_write(1, p, n); }\n"
    "static inline void hx_u64_to_dec(uint64_t v, char *buf, int *len) {\n"
    "  char tmp[24];\n"
    "  int t = 0;\n"
    "  if (v == 0) tmp[t++] = '0';\n"
    "  while (v) { tmp[t++] = (char)('0' + (v % 10u)); v /= 10u; }\n"
    "  while (t) buf[(*len)++] = tmp[--t];\n"
    "}\n"
    "static inline void hx_print_i64(int64_t v) {\n"
    "  char buf[24];\n"
    "  int n = 0;\n"
    "  uint64_t u;\n"
    "  if (v < 0) { buf[n++] = '-'; u = (uint64_t)(-(v + 1)) + 1u; }\n"
    "  else u = (uint64_t)v;\n"
    "  hx_u64_to_dec(u, buf, &n);\n"
    "  hx_out(buf, n);\n"
    "}\n"
    "static inline void hx_print_bool(hx_bool v) { hx_out(v ? \"true\" : \"false\", v ? 4 : 5); }\n"
    "static inline void hx_print_str(hx_str s) { hx_write(1, s.p, s.n); }\n"
    "static inline void hx_print_f64(double d) {\n"
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
    "static inline void hx_print_duration(int64_t ns) {\n"
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
    "static inline void hx_panic(const char *msg, int64_t n) {\n"
    "  hx_write(2, \"hx: error: \", 11);\n"
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
    "static inline int64_t hx_pat_eq_i(int64_t a, int64_t b) { return a == b; }\n"
    "static inline int64_t hx_pat_eq_d(double a, double b) { return a == b; }\n"
    "static inline int64_t hx_pat_eq_s(hx_str a, hx_str b) { return hx_str_eq(a, b); }\n"
    "static inline void hx_propagate_top(hx_result v) {\n"
    "  hx_write(2, \"hx: error no controlado: \", 25);\n"
    "  hx_write(2, v.s.p, v.s.n);\n"
    "  hx_write(2, \"\\n\", 1);\n"
    "  hx_exit(70);\n"
    "}\n";

static const char *HX_RT_INDEX =
    "static inline void hx_panic_i64(const char *msg, int64_t n, int64_t v) {\n"
    "  hx_write(2, msg, n);\n"
    "  hx_write(2, \": \", 2);\n"
    "  char buf[24];\n"
    "  int m = 0;\n"
    "  uint64_t u;\n"
    "  if (v < 0) { buf[m++] = '-'; u = (uint64_t)(-(v + 1)) + 1u; }\n"
    "  else u = (uint64_t)v;\n"
    "  hx_u64_to_dec(u, buf, &m);\n"
    "  hx_write(2, buf, m);\n"
    "  hx_write(2, \" no cabe en un arreglo de ese tamano\\n\", 37);\n"
    "  hx_exit(70);\n"
    "  __builtin_unreachable();\n"
    "}\n"
    "static inline void hx_idx_chk(int64_t i, int64_t n) {\n"
    "  if (i < 0 || i >= n) hx_panic_i64(\"indice fuera de rango\", 22, i);\n"
    "}\n"
    "static inline void *hx_idx_at(void *p, int64_t i, int64_t esz, int64_t n) {\n"
    "  hx_idx_chk(i, n);\n"
    "  return (char *)p + i * esz;\n"
    "}\n"
    "#define HX_AT(p, i, n) (hx_idx_chk((int64_t)(i), (int64_t)(n)), (p)[(i)])\n";

/* El arreglo dinámico: {datos, largo, capacidad}. Crece por duplicación y sin
   realloc: se reserva un bloque nuevo desde la arena y se copia el contenido, así
   que el viejo se queda hasta que la arena se libera. Es memoria de más a
   cambio de no depender de realloc en el perfil freestanding. */
static const char *HX_RT_DARR =
    "typedef struct { void *data; int64_t len, cap; } hx_darr;\n"
    "static hx_darr hx_darr_make(void) {\n"
    "  hx_darr d; d.data = NULL; d.len = 0; d.cap = 0; return d;\n"
    "}\n"
    "static hx_darr hx_darr_make_n(hx_arena *a, int64_t n, int64_t esz) {\n"
    "  hx_darr d = hx_darr_make();\n"
    "  if (n > 0) { d.data = hx_arena_alloc(a, n * esz); d.cap = n; }\n"
    "  return d;\n"
    "}\n"
    "static hx_darr hx_darr_push(hx_arena *a, hx_darr d, int64_t esz, const void *v) {\n"
    "  if (d.len == d.cap) {\n"
    "    int64_t cap = d.cap ? d.cap * 2 : 4;\n"
    "    void *p = hx_arena_alloc(a, cap * esz);\n"
    "    if (d.len) memcpy(p, d.data, (size_t)(d.len * esz));\n"
    "    d.data = p; d.cap = cap;\n"
    "  }\n"
    "  memcpy((char *)d.data + d.len * esz, v, (size_t)esz);\n"
    "  d.len++;\n"
    "  return d;\n"
    "}\n";

static const char *HX_RT_CHECKED =
    "static inline int32_t hx_add_i32(int32_t a, int32_t b) {\n"
    "  int32_t r;\n"
    "  if (__builtin_add_overflow(a, b, &r))\n"
    "    hx_panic(\"desbordamiento de Entero en +\", sizeof(\"desbordamiento de Entero en +\") - 1);\n"
    "  return r;\n"
    "}\n"
    "static inline int32_t hx_sub_i32(int32_t a, int32_t b) {\n"
    "  int32_t r;\n"
    "  if (__builtin_sub_overflow(a, b, &r))\n"
    "    hx_panic(\"desbordamiento de Entero en -\", sizeof(\"desbordamiento de Entero en -\") - 1);\n"
    "  return r;\n"
    "}\n"
    "static inline int64_t hx_add_i64(int64_t a, int64_t b) {\n"
    "  int64_t r;\n"
    "  if (__builtin_add_overflow(a, b, &r))\n"
    "    hx_panic(\"desbordamiento de I64 en +\", sizeof(\"desbordamiento de I64 en +\") - 1);\n"
    "  return r;\n"
    "}\n"
    "static inline int64_t hx_sub_i64(int64_t a, int64_t b) {\n"
    "  int64_t r;\n"
    "  if (__builtin_sub_overflow(a, b, &r))\n"
    "    hx_panic(\"desbordamiento de I64 en -\", sizeof(\"desbordamiento de I64 en -\") - 1);\n"
    "  return r;\n"
    "}\n"
    "static inline int32_t hx_div_i32(int32_t a, int32_t b) {\n"
    "  if (b == 0) hx_panic(\"division por cero\", sizeof(\"division por cero\") - 1);\n"
    "  if (a == -2147483647 - 1 && b == -1)\n"
    "    hx_panic(\"desbordamiento de Entero en /\", sizeof(\"desbordamiento de Entero en /\") - 1);\n"
    "  return a / b;\n"
    "}\n"
    "static inline int64_t hx_div_i64(int64_t a, int64_t b) {\n"
    "  if (b == 0) hx_panic(\"division por cero\", sizeof(\"division por cero\") - 1);\n"
    "  if (a == -9223372036854775807LL - 1 && b == -1)\n"
    "    hx_panic(\"desbordamiento de I64 en /\", sizeof(\"desbordamiento de I64 en /\") - 1);\n"
    "  return a / b;\n"
    "}\n"
    "static inline int32_t hx_mod_i32(int32_t a, int32_t b) {\n"
    "  if (b == 0) hx_panic(\"modulo por cero\", sizeof(\"modulo por cero\") - 1);\n"
    "  if (a == -2147483647 - 1 && b == -1) return 0;\n"
    "  return a % b;\n"
    "}\n"
    "static inline int64_t hx_mod_i64(int64_t a, int64_t b) {\n"
    "  if (b == 0) hx_panic(\"modulo por cero\", sizeof(\"modulo por cero\") - 1);\n"
    "  if (a == -9223372036854775807LL - 1 && b == -1) return 0;\n"
    "  return a % b;\n"
    "}\n"
    "static inline int32_t hx_add_sat_i32(int32_t a, int32_t b) {\n"
    "  int64_t r = (int64_t)a + (int64_t)b;\n"
    "  if (r > 2147483647LL) return 2147483647;\n"
    "  if (r < -2147483647LL - 1) return -2147483647 - 1;\n"
    "  return (int32_t)r;\n"
    "}\n"
    "static inline int32_t hx_sub_sat_i32(int32_t a, int32_t b) {\n"
    "  int64_t r = (int64_t)a - (int64_t)b;\n"
    "  if (r > 2147483647LL) return 2147483647;\n"
    "  if (r < -2147483647LL - 1) return -2147483647 - 1;\n"
    "  return (int32_t)r;\n"
    "}\n"
    "static inline int64_t hx_add_sat_i64(int64_t a, int64_t b) {\n"
    "  if (b > 0 && a > 9223372036854775807LL - b) return 9223372036854775807LL;\n"
    "  if (b < 0 && a < -9223372036854775807LL - 1 - b) return -9223372036854775807LL - 1;\n"
    "  return a + b;\n"
    "}\n"
    "static inline int32_t hx_mul_sat_i32(int32_t a, int32_t b) {\n"
    "  int64_t r = (int64_t)a * (int64_t)b;\n"
    "  if (r > 2147483647LL) return 2147483647;\n"
    "  if (r < -2147483647LL - 1) return -2147483647 - 1;\n"
    "  return (int32_t)r;\n"
    "}\n"
    "static inline int64_t hx_mul_sat_i64(int64_t a, int64_t b) {\n"
    "  if (a == 0 || b == 0) return 0;\n"
    "  int64_t r = a * b;\n"
    "  if (r / b != a) return (a ^ b) < 0 ? -9223372036854775807LL - 1 : 9223372036854775807LL;\n"
    "  return r;\n"
    "}\n"
    "static inline int64_t hx_sub_sat_i64(int64_t a, int64_t b) {\n"
    "  if (b < 0 && a > 9223372036854775807LL + b) return 9223372036854775807LL;\n"
    "  if (b > 0 && a < -9223372036854775807LL - 1 + b) return -9223372036854775807LL - 1;\n"
    "  return a - b;\n"
    "}\n";

static const char *HX_RT_STRFUNS =
    "static inline int64_t hx_len_str(hx_str s) { return s.n; }\n"
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
    "static inline void *hx_arena_alloc(hx_arena *a, int64_t n) {\n"
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
    "static inline void hx_arena_init(hx_arena *a) { a->head = 0; }\n"
    "static inline void hx_arena_free(hx_arena *a) {\n"
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
    "static inline void *hx_raw_alloc(int64_t n) {\n"
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
    "static inline void hx_raw_free(void *p) {\n"
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
    "static inline int64_t hx_static_heap_used;\n"
    "static inline void *hx_raw_alloc(int64_t n) {\n"
    "  char *p;\n"
    "  n = (n + 15) & ~(int64_t)15;\n"
    "  if (hx_static_heap_used + n > (int64_t)sizeof(hx_static_heap)) return 0;\n"
    "  p = hx_static_heap + hx_static_heap_used;\n"
    "  hx_static_heap_used += n;\n"
    "  return p;\n"
    "}\n"
    "static inline void hx_raw_free(void *p) { (void)p; }\n"
    "#endif\n";

static const char *HX_RT_RAWALLOC_LIBC =
    "static inline void *hx_raw_alloc(int64_t n) { return malloc((size_t)n); }\n"
    "static inline void hx_raw_free(void *p) { free(p); }\n"
    "static inline void *hx_heap_alloc(int64_t n) { return malloc((size_t)n); }\n"
    "static inline void hx_heap_free(void *p) { free(p); }\n";

/* El C no dice nada de mover 32 bits un INT, ni de mover a la izquierda un
   numero negativo: es indefinido, y en un lenguaje que verifica la division por
   cero y el desbordamiento de la suma, dejarlo asi seria una incoherencia. Con
   una cuenta constante lo dice el verificador (E0316); con una cuenta variable
   lo dice esto, y aborta con el mismo codigo 70 que el resto. */
static const char *HX_RT_SHIFT =
    "static inline int32_t hx_shl32(int32_t a, int32_t n) {\n"
    "  if (n < 0 || n >= 32) hx_panic(\"desplazamiento a la izquierda fuera de rango\", 44);\n"
    "  return (int32_t)((uint32_t)a << n);\n"
    "}\n"
    "static inline int32_t hx_shr32(int32_t a, int32_t n) {\n"
    "  if (n < 0 || n >= 32) hx_panic(\"desplazamiento a la derecha fuera de rango\", 42);\n"
    "  return a >> n;\n"
    "}\n"
    "static inline int64_t hx_shl64(int64_t a, int64_t n) {\n"
    "  if (n < 0 || n >= 64) hx_panic(\"desplazamiento a la izquierda fuera de rango\", 44);\n"
    "  return (int64_t)((uint64_t)a << n);\n"
    "}\n"
    "static inline int64_t hx_shr64(int64_t a, int64_t n) {\n"
    "  if (n < 0 || n >= 64) hx_panic(\"desplazamiento a la derecha fuera de rango\", 42);\n"
    "  return a >> n;\n"
    "}\n";

static const char *HX_RT_VEC_PRE =
    "static inline long long hx_pow10(int k) {\n"
    "  long long r = 1;\n"
    "  int i;\n"
    "  for (i = 0; i < k; i++) r *= 10;\n"
    "  return r;\n"
    "}\n"
    "static inline void hx_print_f32(float v) {\n"
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
    "static inline float hx_dot(hx_vec3 a, hx_vec3 b) {\n"
    "  return a.x * b.x + a.y * b.y + a.z * b.z;\n"
    "}\n"
    "static hx_vec3 hx_cross(hx_vec3 a, hx_vec3 b) {\n"
    "  return hx_v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);\n"
    "}\n"
    "static inline double hx_sqrt_d(double x) {\n"
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
    /* hx_len3 devolvia el producto escalar, o sea la longitud al cuadrado: un
       LEN de (1,2,3) decia 14 en vez de 3.74. normalized se compensaba con un
       sqrt por su cuenta; con la longitud ya verdadera no hace falta. */
    "static inline float hx_len3(hx_vec3 v) {\n"
    "  return (float)hx_sqrt_d((double)hx_dot(v, v));\n"
    "}\n"
    "static hx_vec3 hx_normalized(hx_vec3 v) {\n"
    "  float l = hx_len3(v);\n"
    "  if (l <= 0.0f) return v;\n"
    "  l = 1.0f / l;\n"
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
    "static inline hx_vec2 hx_zero_vec2(void) { return hx_v2(0.0f, 0.0f); }\n"
    "static inline hx_vec3 hx_zero_vec3(void) { return hx_v3(0.0f, 0.0f, 0.0f); }\n"
    "static inline hx_vec4 hx_zero_vec4(void) {\n"
    "  return hx_v4(0.0f, 0.0f, 0.0f, 0.0f);\n"
    "}\n"
    "static inline hx_mat4 hx_zero_mat4(void) {\n"
    "  hx_mat4 m;\n"
    "  m.r0 = hx_v4(0.0f, 0.0f, 0.0f, 0.0f); m.r1 = m.r0;\n"
    "  m.r2 = m.r0; m.r3 = m.r0;\n"
    "  return m;\n"
    "}\n"
    "static inline void hx_print_vec2(hx_vec2 v) {\n"
    "  hx_out(\"(\", 1); hx_print_f32(v.x); hx_out(\", \", 2);\n"
    "  hx_print_f32(v.y); hx_out(\")\", 1);\n"
    "}\n"
    "static inline void hx_print_vec3(hx_vec3 v) {\n"
    "  hx_out(\"(\", 1); hx_print_f32(v.x); hx_out(\", \", 2);\n"
    "  hx_print_f32(v.y); hx_out(\", \", 2);\n"
    "  hx_print_f32(v.z); hx_out(\")\", 1);\n"
    "}\n"
    "static inline void hx_print_vec4(hx_vec4 v) {\n"
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

/* Texto a partir de un numero, y al reves. El runtime ya tenia hx_str_of_i64 y
   hx_str_of_f64 para PRINT, pero devuelven un puntero a un buffer de la pila, que
   solo sirve mientras dura la llamada: para ToString() el texto tiene que vivir
   mas alla, asi que se copia a memoria propia. */
static const char *HX_RT_TOSTRING =
    /* El formateo se hace en un buffer local y la copia se pide despues.
       Al reves, hx_str_of_i64 devuelve un puntero a la pila de la funcion que
       llama, y la llamada a hx_raw_alloc pisa justo ese sitio: el signo de un
       negativo salia como un espacio en lugar de un guion. */
    "static hx_str hx_i64_str(int64_t v) {\n"
    "  char buf[24];\n"
    "  int n = 0;\n"
    "  uint64_t u;\n"
    "  char *p;\n"
    "  hx_str s;\n"
    "  if (v < 0) { buf[n++] = '-'; u = (uint64_t)(-(v + 1)) + 1u; }\n"
    "  else u = (uint64_t)v;\n"
    "  hx_u64_to_dec(u, buf, &n);\n"
    "  p = (char *)hx_raw_alloc(n + 1);\n"
    "  if (!p) hx_panic(\"sin memoria para un texto\", sizeof(\"sin memoria para un texto\") - 1);\n"
    "  memcpy(p, buf, (size_t)n);\n"
    "  p[n] = 0;\n"
    "  s.p = p; s.n = n;\n"
    "  return s;\n"
    "}\n"
    "static hx_str hx_f64_str(double d) {\n"
    "  hx_str t = hx_str_of_f64(d);\n"
    "  char buf[64];\n"
    "  int64_t n = t.n;\n"
    "  char *p;\n"
    "  hx_str s;\n"
    "  if (n > 63) n = 63;\n"
    "  memcpy(buf, t.p, (size_t)n);\n"
    "  p = (char *)hx_raw_alloc(n + 1);\n"
    "  if (!p) hx_panic(\"sin memoria para un texto\", sizeof(\"sin memoria para un texto\") - 1);\n"
    "  memcpy(p, buf, (size_t)n);\n"
    "  p[n] = 0;\n"
    "  s.p = p; s.n = n;\n"
    "  return s;\n"
    "}\n"
    "static hx_str hx_str_dup(hx_str s) {\n"
    "  char *p = (char *)hx_raw_alloc(s.n + 1);\n"
    "  hx_str r;\n"
    "  if (!p) hx_panic(\"sin memoria para un texto\", sizeof(\"sin memoria para un texto\") - 1);\n"
    "  memcpy(p, s.p, (size_t)s.n);\n"
    "  p[s.n] = 0;\n"
    "  r.p = p; r.n = s.n;\n"
    "  return r;\n"
    "}\n"
    "static hx_str hx_bool_str(hx_bool v) {\n"
    "  const char *t = v ? \"true\" : \"false\";\n"
    "  int64_t n = v ? 4 : 5;\n"
    "  char buf[8];\n"
    "  char *p;\n"
    "  hx_str s;\n"
    "  memcpy(buf, t, (size_t)n);\n"
    "  p = (char *)hx_raw_alloc(n + 1);\n"
    "  if (!p) hx_panic(\"sin memoria para un texto\", sizeof(\"sin memoria para un texto\") - 1);\n"
    "  memcpy(p, buf, (size_t)n);\n"
    "  p[n] = 0;\n"
    "  s.p = p; s.n = n;\n"
    "  return s;\n"
    "}\n";

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

/* ToString depende del tipo del receptor, asi que el nombre del ayudante sale de
   ahi y no de una tabla de nombres: `hx_i64_str`, `hx_f64_str`, `hx_bool_str`. */
static const char *hx_tostring_cname(HxTy *t) {
    if (!t) return "i64_str";
    switch (t->kind) {
        case TY_INT:
        case TY_I64:
        case TY_DURATION: return "i64_str";
        case TY_FLOAT: return "f64_str";
        case TY_BOOL: return "bool_str";
        case TY_STRING: return "str_dup";
        default: return NULL;
    }
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
        /* los vectores sin valor inicial son el vector cero: sin esto salia
           `hx_vec3 v = hx_zero_int32()`, que el C no acepta */
        case TY_VEC2: return "vec2";
        case TY_VEC3: return "vec3";
        case TY_VEC4: return "vec4";
        case TY_QUAT: return "vec4";
        case TY_MAT4: return "mat4";
        case TY_NAMED:
            if (t->decl)
                return hx_arena_sprintf(e->arena, "rec_%s", hx_sym_str(t->decl->name));
            return "int32";
        default: return "int32";
    }
}

static void hx_expr_str(HxEmit *e, HxExpr *x, int prec, HxBuf *b);

/* Len y At de un arreglo de tamano fijo: el tamano vive en el tipo, o sea que
   Len es una constante y At solo necesita la comparacion con ese tamano. */
static void hx_emit_array_member(HxEmit *e, HxExpr *x, HxBuf *b) {
    HxExpr *recv = x->recv ? x->recv : x->member.base;
    HxTy *bt = recv ? recv->ty : NULL;
    long largo = bt ? (long)bt->size : 0;
    const char *metodo = hx_sym_str(x->method);
    if (bt && bt->kind == TY_ARRAY && bt->size < 0) {
        /* ARRAY[T]: el largo vive en el struct y Push devuelve el receptor */
        const char *el = hx_c_ty(e, bt->elem);
        if (!strcmp(metodo, "Len")) {
            hx_buf_str(b, "(");
            hx_expr_str(e, recv, 0, b);
            hx_buf_str(b, ").len");
            return;
        }
        e->uses_index = 1;
        e->uses_darr = 1;
        e->uses_arena = 1;
        if (!strcmp(metodo, "Push")) {
            /* Push devuelve el largo nuevo, asi que el receptor tiene que
               crearse: de ahi el parentesis y el .len de despues */
            hx_buf_str(b, "((");
            hx_expr_str(e, recv, 7, b);
            hx_buf_printf(b, " = hx_darr_push_%s(", el);
            if (e->arena_depth) hx_buf_printf(b, "&%s", e->arena_stack[e->arena_depth - 1]);
            else hx_buf_str(b, "&hx_static_arena");
            hx_buf_str(b, ", ");
            hx_expr_str(e, recv, 7, b);
            hx_buf_str(b, ", ");
            hx_expr_str(e, x->call.args.data[0].value, 0, b);
            hx_buf_str(b, ")).len)");
            return;
        }
        hx_buf_printf(b, "(*(%s *)hx_idx_at(", el);
        hx_expr_str(e, recv, 7, b);
        hx_buf_str(b, ".data, ");
        hx_expr_str(e, x->call.args.data[0].value, 0, b);
        hx_buf_printf(b, ", sizeof(%s), ", el);
        hx_expr_str(e, recv, 7, b);
        hx_buf_str(b, ".len))");
        if (!strcmp(metodo, "Set")) {
            hx_buf_str(b, " = ");
            hx_expr_str(e, x->call.args.data[1].value, 0, b);
        }
        return;
    }
    if (!strcmp(metodo, "Len")) {
        hx_buf_printf(b, "INT64_C(%ld)", largo);
        return;
    }
    /* un arreglo es un hx_span y sus elementos estan en .data */
    e->uses_index = 1;
    hx_buf_printf(b, "HX_AT((%s *)(", hx_c_ty(e, bt ? bt->elem : NULL));
    hx_expr_str(e, recv, 7, b);
    hx_buf_str(b, ").data, ");
    hx_expr_str(e, x->call.args.data[0].value, 0, b);
    hx_buf_printf(b, ", %ld)", largo);
}

static void hx_iter_note_elem(HxEmit *e, HxTy *t);
static void hx_iter_note_map(HxEmit *e, HxTy *from, HxTy *to);
static void hx_iter_note_chain(HxEmit *e, HxExpr *x);
static void hx_iter_suffix(HxEmit *e, HxTy *t, HxBuf *b);
static struct HxFunc *hx_find_func_named(HxEmit *e, const char *name);
static HxConst *hx_find_const_named(HxEmit *e, const char *name);
static void hx_iter_ctor(HxEmit *e, HxExpr *x, const char *arena, char *out, int cap, int ind);

static void hx_stmt_emit(HxEmit *e, HxStmt *s, int ind);
static void hx_body(HxEmit *e, HxStmtVec *body, int ind);
static void hx_scan_stmt(HxEmit *e, HxStmt *s);
static void hx_scan_body(HxEmit *e, HxStmtVec *body);
static void hx_maybe_note_map(HxEmit *e, HxTy *from, HxTy *to);
static void hx_darr_note_push(HxEmit *e, HxTy *elem);

static void hx_scan_expr(HxEmit *e, HxExpr *x) {
    if (!x) return;
    if (x->kind == EX_STR) e->uses_string = 1;
    if (x->kind == EX_VEC || x->vec_component || (x->ty && (x->ty->kind == TY_VEC2 || x->ty->kind == TY_VEC3 || x->ty->kind == TY_VEC4 || x->ty->kind == TY_MAT4 || x->ty->kind == TY_QUAT)))
        e->uses_vec = 1;
    if (x->kind == EX_TRY || x->is_ok_ctor || x->is_err_ctor) e->uses_result = 1;
    if (x->ty && hx_ty_is_result(x->ty)) e->uses_result = 1;
    switch (x->kind) {
        case EX_CALL:
            if (x->is_intrin == 6) {
                /* recibir copia el datagrama en una arena */
                e->uses_net = 1;
                e->uses_arena = 1;
            }
            if (x->is_intrin == 10) { hx_maybe_note_map(e, x->recv->ty, x->ty); }
            if (x->is_intrin == 7 && x->recv && x->recv->ty &&
                x->recv->ty->kind == TY_ARRAY && x->recv->ty->size < 0 &&
                !hx_ascii_casecmp(hx_sym_str(x->method), "Push")) {
                /* Push necesita el runtime del dinámico y un ayudante por tipo */
                e->uses_darr = 1;
                e->uses_arena = 1;
                hx_darr_note_push(e, x->recv->ty->elem);
            }
            if (x->is_intrin == 7 && strcmp(hx_sym_str(x->method), "Len")) e->uses_index = 1;
            if (x->is_intrin == 8) {
                e->uses_tostring = 1;
                e->uses_string = 1;
            }
            hx_scan_expr(e, x->call.callee);
            for (int i = 0; i < x->call.args.len; i++) hx_scan_expr(e, x->call.args.data[i].value);
            break;
        case EX_BIN:
            /* el runtime se escribe despues de escanear, no de emitir: los
               ayudantes de desplazamiento tienen que quedarse pedidos aqui */
            if ((x->bin.op == OP_SHL || x->bin.op == OP_SHR) && x->bin.rhs->kind != EX_INT)
                e->uses_shift = 1;
            hx_scan_expr(e, x->bin.lhs);
            hx_scan_expr(e, x->bin.rhs);
            break;
        case EX_UN: hx_scan_expr(e, x->un.operand); break;
        case EX_MEMB:
            if (x->is_intrin == 10) { hx_maybe_note_map(e, x->recv->ty, x->ty); }
            if (x->is_intrin == 7 && x->recv && x->recv->ty &&
                x->recv->ty->kind == TY_ARRAY && x->recv->ty->size < 0 &&
                !hx_ascii_casecmp(hx_sym_str(x->method), "Push")) {
                /* Push necesita el runtime del dinámico y un ayudante por tipo */
                e->uses_darr = 1;
                e->uses_arena = 1;
                hx_darr_note_push(e, x->recv->ty->elem);
            }
            if (x->is_intrin == 7 && strcmp(hx_sym_str(x->method), "Len")) e->uses_index = 1;
            hx_scan_expr(e, x->member.base);
            break;
        case EX_INDEX:
            hx_scan_expr(e, x->index.base);
            hx_scan_expr(e, x->index.start);
            hx_scan_expr(e, x->index.end);
            break;
        case EX_TRY: hx_scan_expr(e, x->try.inner); break;
        case EX_VEC:
            for (int i = 0; i < x->vec.len; i++) hx_scan_expr(e, x->vec.items[i]);
            break;
        /* literales, FUNC, un nombre y una desreferencia: nada que escanear */
        case EX_INT:
        case EX_FLOAT:
        case EX_STR:
        case EX_DURATION:
        case EX_BOOL:
        case EX_NIL:
        case EX_PATH:
        case EX_MEMBER:
        case EX_DEREF:
        case EX_FUNC: break;
    }
    for (int i = 0; i < x->n_segs; i++) hx_scan_expr(e, x->segs[i].hole);
}

static void hx_scan_body(HxEmit *e, HxStmtVec *body) {
    for (int i = 0; i < body->len; i++) hx_scan_stmt(e, &body->data[i]);
}

static void hx_scan_stmt(HxEmit *e, HxStmt *s) {
    HxStmtVec *body = NULL;
    switch (s->kind) {
        case ST_MATCH: {
            /* los patrones literales usan los mismos ayudantes que Result */
            e->uses_result = 1;
            hx_scan_expr(e, s->match.subject);
            for (int k = 0; k < s->match.cases.len; k++) {
                hx_scan_expr(e, s->match.cases.data[k].guard);
                hx_scan_body(e, &s->match.cases.data[k].body);
            }
            hx_scan_body(e, &s->match.else_body);
            break;
        }
        case ST_EXPR: hx_scan_expr(e, s->expr); break;
        case ST_ASSIGN:
            hx_scan_expr(e, s->assign.target);
            hx_scan_expr(e, s->assign.value);
            break;
        case ST_DIM:
            hx_scan_expr(e, s->dim.init);
            if (s->dim.ty && s->dim.ty->kind == TY_ARRAY) {
                if (s->dim.ty->size > 0) e->uses_arena = 1;
                else if (s->dim.ty->size < 0) {
                    /* dinámico: la primera reserva la hace Push, desde la arena */
                    e->uses_darr = 1;
                    e->uses_arena = 1;
                }
            }
            /* un MAYBE necesita su typedef antes de que se emita el codigo */
            if (s->dim.ty && s->dim.ty->kind == TY_MAYBE) hx_c_ty(e, s->dim.ty);
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
        case ST_FORIN:
            /* el estado de un iterador vive en una arena creada por la sentencia */
            e->uses_iter = 1;
            e->uses_arena = 1;
            if (s->forin_.iter->ty && s->forin_.iter->ty->kind == TY_ITER)
                hx_iter_note_elem(e, s->forin_.iter->ty->elem);
            hx_iter_note_chain(e, s->forin_.iter);
            hx_scan_expr(e, s->forin_.iter);
            hx_scan_body(e, &s->forin_.body);
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
        case ST_ARENA:
            /* el tipo hx_arena se declara en _runtime.h, que se escribe antes */
            e->uses_arena = 1;
            body = &s->arena.body;
            break;
        case ST_DEFER: hx_scan_body(e, &s->inner); break;
        /* estas no tienen nada que escanear: no llevan expresiones */
        case ST_BREAK:
        case ST_CONTINUE:
        case ST_NOP: break;
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
        case TY_UNKNOWN:
        case TY_VOID:
        case TY_BOOL:
        case TY_INT:
        case TY_I64:
        case TY_FLOAT:
        case TY_STRING:
        case TY_DURATION:
        case TY_NAMED:
        case TY_ARRAY:
        case TY_REF:
        case TY_PTR:
        case TY_MAT4:
        case TY_ITER:
        case TY_MAYBE: break;
    }
    return 0;
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
        /* un literal, un nombre o una llamada no llevan operadores dentro */
        case EX_INT:
        case EX_FLOAT:
        case EX_STR:
        case EX_DURATION:
        case EX_BOOL:
        case EX_NIL:
        case EX_PATH:
        case EX_CALL:
        case EX_INDEX:
        case EX_MEMBER:
        case EX_TRY:
        case EX_VEC:
        case EX_MEMB:
        case EX_DEREF:
        case EX_FUNC: return 7;
    }
    return 7;
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
        case TY_INT:
        case TY_I64:
        case TY_NAMED:
        case TY_REF:
        case TY_PTR:
        case TY_ARRAY:
        case TY_UNKNOWN:
        case TY_VOID:
        case TY_ITER:
        case TY_MAYBE: return "hx_print_i64";
    }
    return "hx_print_i64";
}

static const char *hx_str_of_fn(HxTy *t) {
    if (!t) return "hx_str_of_i64";
    switch (t->kind) {
        case TY_FLOAT: return "hx_str_of_f64";
        case TY_BOOL: return "hx_str_of_bool";
        case TY_INT:
        case TY_I64:
        case TY_STRING:
        case TY_DURATION:
        case TY_NAMED:
        case TY_REF:
        case TY_PTR:
        case TY_ARRAY:
        case TY_VEC2:
        case TY_VEC3:
        case TY_VEC4:
        case TY_MAT4:
        case TY_QUAT:
        case TY_ITER:
        case TY_MAYBE:
        case TY_UNKNOWN:
        case TY_VOID: return "hx_str_of_i64";
    }
    return "hx_str_of_i64";
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

static void hx_enum_member_str(HxEmit *e, HxExpr *x, HxBuf *b);
/* Las llamadas de la capacidad net se traducen a las primitivas del runtime.
   La direccion se recibe como I64 con los cuatro octetos empaquetados. */
static void hx_emit_net(HxEmit *e, HxExpr *x, HxBuf *b) {
    const char *nm = hx_sym_str(x->method);
    /* los argumentos son HxArg: hay que copiar los valores, no reinterpretar */
    HxExpr *vals[8];
    int n = x->call.args.len < 8 ? x->call.args.len : 8;
    for (int i = 0; i < n; i++) vals[i] = x->call.args.data[i].value;
    HxExpr **a = vals;
    e->uses_net = 1;
    if (!hx_ascii_casecmp(nm, "NET_UDP") || !hx_ascii_casecmp(nm, "NET_TCP")) {
        hx_buf_printf(b, "hx_socket(%sL)", !hx_ascii_casecmp(nm, "NET_TCP") ? "1" : "2");
        return;
    }
    if (!a || n < 1) {
        hx_buf_printf(b, "0 /* %s */", nm);
        return;
    }
    if (!hx_ascii_casecmp(nm, "NET_ERROR")) {
        hx_buf_str(b, "hx_net_error()");
        return;
    }
    if (!hx_ascii_casecmp(nm, "NET_CLOSE")) {
        hx_buf_str(b, "hx_close_fd(");
        hx_expr_str(e, a[0], 0, b);
        hx_buf_str(b, ")");
        return;
    }
    if (!hx_ascii_casecmp(nm, "NET_LISTEN") && n >= 2) {
        hx_buf_str(b, "hx_listen(");
        hx_expr_str(e, a[0], 0, b);
        hx_buf_str(b, ", ");
        hx_expr_str(e, a[1], 0, b);
        hx_buf_str(b, ")");
        return;
    }
    if ((!hx_ascii_casecmp(nm, "NET_BIND") || !hx_ascii_casecmp(nm, "NET_CONNECT")) && n >= 2) {
        hx_buf_printf(b, "hx_%s(", !hx_ascii_casecmp(nm, "NET_BIND") ? "bind" : "connect");
        hx_expr_str(e, a[0], 0, b);
        hx_buf_str(b, ", hx_inet_de(");
        hx_expr_str(e, a[1], 0, b);
        hx_buf_str(b, ", 127, 0, 0, 1))");
        return;
    }
    if (!hx_ascii_casecmp(nm, "NET_ACCEPT")) {
        hx_buf_str(b, "hx_accept(");
        hx_expr_str(e, a[0], 0, b);
        hx_buf_str(b, ", 0)");
        return;
    }
    if (!hx_ascii_casecmp(nm, "NET_SEND") && n >= 4) {
        /* la direccion llega como I64 con los cuatro octetos empaquetados */
        hx_buf_str(b, "hx_sendto(");
        hx_expr_str(e, a[0], 0, b);
        hx_buf_str(b, ", hx_span_of(");
        hx_expr_str(e, a[3], 0, b);
        hx_buf_str(b, "), hx_inet_de(");
        hx_expr_str(e, a[1], 0, b);
        for (int k = 24; k >= 0; k -= 8) {
            hx_buf_printf(b, ", (int)(((int64_t)(");
            hx_expr_str(e, a[2], 0, b);
            hx_buf_printf(b, ") >> %d) & 255)", k);
        }
        hx_buf_str(b, "))");
        return;
    }
    if (!hx_ascii_casecmp(nm, "NET_RECV")) {
        hx_buf_str(b, "hx_net_recv(");
        hx_expr_str(e, a[0], 0, b);
        hx_buf_str(b, ", ");
        if (e->arena_depth) hx_buf_printf(b, "&%s", e->arena_stack[e->arena_depth - 1]);
        else hx_buf_str(b, "&hx_static_arena");
        hx_buf_str(b, ")");
        return;
    }
    if (!hx_ascii_casecmp(nm, "NET_RECV_DE")) {
        hx_buf_str(b, "hx_net_recv_de(");
        hx_expr_str(e, a[0], 0, b);
        hx_buf_str(b, ", ");
        if (e->arena_depth) hx_buf_printf(b, "&%s", e->arena_stack[e->arena_depth - 1]);
        else hx_buf_str(b, "&hx_static_arena");
        hx_buf_str(b, ", ");
        if (n > 1 && a[1] && a[1]->ty && a[1]->ty->kind == TY_REF && a[1]->path.parts.len)
            hx_buf_printf(b, "&hx_v_%s", hx_sym_str(a[1]->path.parts.data[0].name));
        else hx_buf_str(b, "0");
        hx_buf_str(b, ", ");
        if (n > 2 && a[2] && a[2]->ty && a[2]->ty->kind == TY_REF && a[2]->path.parts.len)
            hx_buf_printf(b, "&hx_v_%s", hx_sym_str(a[2]->path.parts.data[0].name));
        else hx_buf_str(b, "0");
        hx_buf_str(b, ")");
        return;
    }
    hx_buf_printf(b, "0 /* %s */", nm);
}

/* Cuando el verificador acepta un registro con mas campos donde se piden
   menos, la conversion se materializa copiando los campos comunes. */
static void hx_emit_conv(HxEmit *e, HxExpr *x, HxBuf *b) {
    /* T -> MAYBE<T>: el ayudante pone la bandera y guarda el valor */
    if (x->conv_ty && x->conv_ty->kind == TY_MAYBE) {
        const char *inner = hx_c_ty(e, x->conv_ty->elem);
        hx_c_ty(e, x->conv_ty);
        HxTy *marca = x->conv_ty;
        x->conv_ty = NULL;
        hx_buf_printf(b, "hx_maybe_some_%s(", inner);
        hx_expr_str(e, x, 0, b);
        hx_buf_str(b, ")");
        x->conv_ty = marca;
        return;
    }
    HxTypeDecl *from = x->ty && x->ty->decl ? x->ty->decl : NULL;
    HxTypeDecl *to = x->conv_ty && x->conv_ty->decl ? x->conv_ty->decl : NULL;
    if (!from || !to) {
        hx_expr_str(e, x, 0, b);
        return;
    }
    hx_buf_printf(b, "(%s){", hx_c_ty(e, x->conv_ty));
    /* los campos se emiten del valor original: hay que quitar la marca para no
       recursar en la misma conversion */
    HxTy *marca = x->conv_ty;
    x->conv_ty = NULL;
    int primero = 1;
    for (int i = 0; i < to->fields.len; i++) {
        const char *fname = hx_sym_str(to->fields.data[i].name);
        int j = -1;
        for (int k = 0; k < from->fields.len; k++)
            if (!hx_ascii_casecmp(hx_sym_str(from->fields.data[k].name), fname)) j = k;
        if (j < 0) continue;
        if (!primero) hx_buf_str(b, ", ");
        primero = 0;
        hx_buf_printf(b, ".%s = ", fname);
        hx_expr_str(e, x, 0, b);
        hx_buf_printf(b, ".%s", fname);
    }
    hx_buf_str(b, "}");
    x->conv_ty = marca;
}

static void hx_expr_str(HxEmit *e, HxExpr *x, int prec, HxBuf *b) {
    if (x && x->conv_ty) {
        hx_emit_conv(e, x, b);
        return;
    }
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
            /* el verificador ya le dio un tipo MAYBE: el cero del struct */
            if (x->is_nil && x->ty) hx_buf_printf(b, "((%s){ 0 })", hx_c_ty(e, x->ty));
            else hx_buf_str(b, x->ival ? "1" : "0");
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
            if (x->is_intrin == 5) {
                hx_enum_member_str(e, x, b);
                break;
            }
            if (x->is_intrin == 6) {
                hx_emit_net(e, x, b);
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
            if (x->is_intrin == 7) {
                hx_emit_array_member(e, x, b);
                break;
            }
            if (x->is_intrin == 9) { /* m.Or(x) */
                hx_buf_printf(b, "hx_maybe_or_%s(", hx_c_ty(e, x->recv->ty->elem));
                hx_expr_str(e, x->recv, 0, b);
                hx_buf_str(b, ", ");
                hx_expr_str(e, x->call.args.data[0].value, 0, b);
                hx_buf_str(b, ")");
                break;
            }
            if (x->is_intrin == 10) { /* m.Map(f): f es una FUNC elevada */
                HxExpr *farg = x->call.args.data[0].value;
                struct HxFunc *fnn = NULL;
                if (farg->kind == EX_FUNC && farg->lit) fnn = farg->lit;
                else if (farg->kind == EX_PATH && farg->path.parts.len)
                    fnn = hx_find_func_named(e, hx_sym_str(
                             farg->path.parts.data[farg->path.parts.len - 1].name));
                hx_maybe_note_map(e, x->recv->ty, x->ty);
                hx_buf_printf(b, "hx_maybe_map_%s_%s(",
                              hx_c_ty(e, x->recv->ty->elem), hx_c_ty(e, x->ty->elem));
                hx_expr_str(e, x->recv, 0, b);
                hx_buf_printf(b, ", &hx_call_%s)", hx_sym_str(fnn ? fnn->name : "?"));
                break;
            }
            if (x->is_ok_ctor || x->is_err_ctor) {
                e->uses_result = 1;
                const char *fn = x->is_ok_ctor ? "hx_ok" : "hx_err";
                /* en M4 el error de un Result es siempre STRING */
                const char *sfx = "s";
                if (x->is_ok_ctor) {
                    if (x->payload_ty && x->payload_ty->kind == TY_FLOAT) sfx = "f";
                    else if (x->payload_ty &&
                             (x->payload_ty->kind == TY_STRING || x->payload_ty->kind == TY_NAMED))
                        sfx = "s";
                    else sfx = "i";
                }
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
                if (!hx_ascii_casecmp(nm, "ADD") || !hx_ascii_casecmp(nm, "SUB")) {
                    hx_buf_printf(b, "hx_%s(", !hx_ascii_casecmp(nm, "ADD") ? "add3" : "sub3");
                    hx_expr_str(e, x->call.args.data[0].value, 0, b);
                    hx_buf_str(b, ", ");
                    hx_expr_str(e, x->call.args.data[1].value, 0, b);
                    hx_buf_str(b, ")");
                    break;
                }
                if (!hx_ascii_casecmp(nm, "SCALE")) {
                    hx_buf_str(b, "hx_scale3(");
                    hx_expr_str(e, x->call.args.data[0].value, 0, b);
                    hx_buf_str(b, ", ");
                    hx_expr_str(e, x->call.args.data[1].value, 0, b);
                    hx_buf_str(b, ")");
                    break;
                }
                if (!hx_ascii_casecmp(nm, "SWIZZLE")) {
                    hx_buf_str(b, "hx_v4(");
                    hx_expr_str(e, x->call.args.data[0].value, 0, b);
                    hx_buf_str(b, ".x, ");
                    hx_expr_str(e, x->call.args.data[0].value, 0, b);
                    hx_buf_str(b, ".y, ");
                    hx_expr_str(e, x->call.args.data[0].value, 0, b);
                    hx_buf_str(b, ".z, ");
                    hx_expr_str(e, x->call.args.data[0].value, 0, b);
                    hx_buf_str(b, ".w)");
                    break;
                }
                hx_buf_str(b, "hx_normalized(");
                hx_expr_str(e, x->call.args.data[0].value, 0, b);
                hx_buf_str(b, ")");
                break;
            }
            if (x->is_intrin == 5) {
                hx_enum_member_str(e, x, b);
                break;
            }
            if (x->is_intrin == 6) {
                hx_emit_net(e, x, b);
                break;
            }
            if (x->is_intrin == 8) {
                /* ToString: el texto vive en memoria propia, no en la pila */
                e->uses_tostring = 1;
                e->uses_string = 1;
                hx_buf_printf(b, "hx_%s(", hx_tostring_cname(x->recv ? x->recv->ty : NULL));
                hx_expr_str(e, x->recv, 0, b);
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
            if (x->fn)
                hx_buf_printf(b, "hx_call_%s(", hx_sym_str(x->fn->name));
            else if (callee->kind == EX_PATH)
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
            if ((x->bin.op == OP_EQ || x->bin.op == OP_NE) && x->bin.lhs &&
                x->bin.lhs->ty && x->bin.lhs->ty->kind == TY_STRING) {
                hx_buf_str(b, "hx_str_eq(");
                hx_expr_str(e, x->bin.lhs, 0, b);
                hx_buf_str(b, ", ");
                hx_expr_str(e, x->bin.rhs, 0, b);
                hx_buf_str(b, x->bin.op == OP_NE ? ") == 0" : ")");
                break;
            }
            const char *cop = o;
            if (x->bin.op == OP_ADDW) cop = "+";
            if (x->bin.op == OP_SUBW) cop = "-";
            if (x->bin.op == OP_ADDS) cop = "+";
            if (x->bin.op == OP_SUBS) cop = "-";
            if (x->bin.op == OP_MULS) cop = "*";
            if (x->bin.op == OP_NE) cop = "!=";
            if (x->bin.op == OP_EQ) cop = "==";
            /* AND, OR y XOR leen de dos maneras y el tipo del operando izquierdo
               dice cual: con BOOL son los operadores booleanos de C, con entero
               son la operacion de bits. El verificador ya ha comprobado que los
               dos lados son del mismo bando. */
            if (x->bin.lhs->ty && x->bin.lhs->ty->kind == TY_BOOL) {
                if (x->bin.op == OP_AND) cop = "&&";
                if (x->bin.op == OP_OR) cop = "||";
                if (x->bin.op == OP_XOR) cop = "!=";
            } else {
                if (x->bin.op == OP_AND) cop = "&";
                if (x->bin.op == OP_OR) cop = "|";
                if (x->bin.op == OP_XOR) cop = "^";
            }
            if (x->bin.op == OP_SHL || x->bin.op == OP_SHR) {
                /* con cuenta constante el verificador ya ha comprobado que cabe,
                   y el C directo es el codigo mas pequeño */
                int cuenta_constante = x->bin.rhs->kind == EX_INT;
                int i64 = x->bin.lhs->ty && x->bin.lhs->ty->kind == TY_I64;
                if (!cuenta_constante) {
                    e->uses_shift = 1;
                    hx_buf_printf(b, "hx_%s%d(", x->bin.op == OP_SHL ? "shl" : "shr", i64 ? 64 : 32);
                    hx_expr_str(e, x->bin.lhs, 5, b);
                    hx_buf_str(b, ", ");
                    hx_expr_str(e, x->bin.rhs, 0, b);
                    hx_buf_str(b, ")");
                    break;
                }
                cop = x->bin.op == OP_SHL ? "<<" : ">>";
            }
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
            if ((x->bin.op == OP_DIV || x->bin.op == OP_MOD) && x->ty &&
                x->ty->kind != TY_FLOAT) {
                /* el divisor se comprueba en ejecucion: no se sabe su valor */
                const char *k = x->ty->kind == TY_I64 || x->ty->kind == TY_DURATION ? "i64" : "i32";
                hx_buf_printf(b, "hx_%s_%s(", x->bin.op == OP_DIV ? "div" : "mod", k);
                hx_expr_str(e, x->bin.lhs, 0, b);
                hx_buf_str(b, ", ");
                hx_expr_str(e, x->bin.rhs, 0, b);
                hx_buf_str(b, ")");
                break;
            }
            if (x->bin.op == OP_ADDW || x->bin.op == OP_SUBW) {
                const char *k = x->ty && (x->ty->kind == TY_I64 || x->ty->kind == TY_DURATION)
                                    ? "int64_t" : "int32_t";
                const char *uk = x->ty && (x->ty->kind == TY_I64 || x->ty->kind == TY_DURATION)
                                     ? "uint64_t" : "uint32_t";
                hx_buf_printf(b, "((%s)((%s)(", k, uk);
                hx_expr_str(e, x->bin.lhs, 0, b);
                hx_buf_printf(b, ") %s (%s)(", x->bin.op == OP_ADDW ? "+" : "-", uk);
                hx_expr_str(e, x->bin.rhs, 0, b);
                hx_buf_str(b, ")))");
                break;
            }
            if (x->bin.op == OP_ADD || x->bin.op == OP_SUB || x->bin.op == OP_ADDS ||
                x->bin.op == OP_SUBS || x->bin.op == OP_MULS) {
                const char *kind = x->ty && x->ty->kind == TY_FLOAT   ? "f64"
                                   : x->ty && x->ty->kind == TY_I64    ? "i64"
                                   : x->ty && x->ty->kind == TY_DURATION ? "i64"
                                                                        : "i32";
                const char *fn = NULL;
                if (x->bin.op == OP_ADD) fn = hx_kind(kind, "add", "");
                if (x->bin.op == OP_SUB) fn = hx_kind(kind, "sub", "");
                if (x->bin.op == OP_ADDS) fn = hx_kind(kind, "add_sat", "");
                if (x->bin.op == OP_SUBS) fn = hx_kind(kind, "sub_sat", "");
                if (x->bin.op == OP_MULS) fn = hx_kind(kind, "mul_sat", "");
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
            if (x->un.op == UOP_ADDR) {
                hx_buf_printf(b, "(&hx_v_%s)",
                              hx_sym_str(x->un.operand->path.parts.data[0].name));
                break;
            }
            if (x->un.op == UOP_NOT) {
                /* `~x` sobre un entero es el complemento a bits, no una negacion */
                hx_buf_str(b, x->un.operand->ty && x->un.operand->ty->kind == TY_BOOL ? "!" : "~");
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
        case EX_DEREF:
            hx_buf_str(b, "(*");
            hx_expr_str(e, x->try.inner, 0, b);
            hx_buf_str(b, ")");
            break;
        case EX_MEMB: {
            if (x->is_intrin == 5) {
                hx_enum_member_str(e, x, b);
                break;
            }
            if (x->is_intrin == 7) {
                hx_emit_array_member(e, x, b);
                break;
            }
            if (x->is_intrin == 8) { /* m.IsNil */
                hx_buf_str(b, "((");
                hx_expr_str(e, x->member.base, 0, b);
                hx_buf_str(b, ").hay == 0)");
                break;
            }
            if (x->is_intrin == 6) {
                hx_emit_net(e, x, b);
                break;
            }
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
            case ST_FORIN: n += hx_count_defers(&st->forin_.body); break;
            case ST_ARENA: n += hx_count_defers(&st->arena.body); break;
            case ST_BLOCK: n += hx_count_defers(&st->block.stmts); break;
            case ST_MATCH:
                for (int k = 0; k < st->match.cases.len; k++)
                    n += hx_count_defers(&st->match.cases.data[k].body);
                n += hx_count_defers(&st->match.else_body);
                break;
            /* un DEFER dentro de otro DEFER tambien cuenta */
            case ST_DEFER: n += hx_count_defers(&st->inner); break;
            /* ninguna de estas envuelve un cuerpo */
            case ST_EXPR:
            case ST_ASSIGN:
            case ST_DIM:
            case ST_PRINT:
            case ST_RETURN:
            case ST_BREAK:
            case ST_CONTINUE:
            case ST_EXIT:
            case ST_CONST:
            case ST_NOP: break;
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

/* El temporal con el valor de dentro de un MAYBE, declarado justo antes de la
   cadena de if del MATCH. */
static const char *hx_maybe_valor(HxEmit *e, const char *subj) {
    return hx_arena_sprintf(e->arena, "(%s).valor", subj);
}

static const char *hx_payload_field(HxTy *t) {
    if (!t) return "i";
    if (t->kind == TY_FLOAT) return "f";
    if (t->kind == TY_STRING) return "s";
    return "i";
}

static void hx_emit_pattern_test(HxEmit *e, HxPattern *pat, const char *subj, HxTy *subj_ty) {
    HxBuf *b = &e->out;
    /* En un MATCH sobre MAYBE, el binding y los literales ven el valor de
       dentro, asi que se les pasa el temporal ya desempaquetado. */
    const char *val = subj_ty && subj_ty->kind == TY_MAYBE ? hx_maybe_valor(e, subj)
                                                            : subj;
    switch (pat->kind) {
        case PAT_WILDCARD: hx_buf_str(b, "1"); return;
        case PAT_NIL: hx_buf_printf(b, "(%s.hay == 0)", subj); return;
        case PAT_BIND: hx_buf_str(b, "1"); return;
        case PAT_LITERAL: {
            HxTy *cmp = subj_ty && subj_ty->kind == TY_MAYBE ? subj_ty->elem : subj_ty;
            if (subj_ty && subj_ty->kind == TY_MAYBE) hx_buf_printf(b, "(%s.hay && ", subj);
            hx_buf_printf(b, "hx_pat_eq_%s(%s, ", hx_payload_field(cmp), val);
            hx_expr_str(e, pat->lit, 0, b);
            hx_buf_str(b, ")");
            if (subj_ty && subj_ty->kind == TY_MAYBE) hx_buf_str(b, ")");
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
            /* una variante de ENUM se compara con su constante */
            if (subj_ty && subj_ty->kind == TY_NAMED && subj_ty->decl &&
                subj_ty->decl->is_enum) {
                char *cn = hx_arena_sprintf(e->arena, "%s_%s",
                                             hx_sym_str(subj_ty->decl->name),
                                             hx_sym_str(pat->ctor));
                if (hx_find_const_named(e, cn)) {
                    hx_buf_printf(b, "(%s == hx_v_%s)", subj, cn);
                    return;
                }
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
        case PAT_BIND: {
            HxTy *bt = subj_ty && subj_ty->kind == TY_MAYBE ? subj_ty->elem : subj_ty;
            const char *val = subj_ty && subj_ty->kind == TY_MAYBE ? hx_maybe_valor(e, subj)
                                                                  : subj;
            hx_indent(b, ind);
            hx_buf_printf(b, "%s hx_v_%s = %s;\n", hx_c_ty(e, bt), hx_sym_str(pat->name), val);
            return;
        }
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
        /* estos patrones no atan ningun nombre */
        case PAT_WILDCARD:
        case PAT_NIL:
        case PAT_LITERAL:
        case PAT_RANGE: return;
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
        char *dest = hx_arena_sprintf(e->arena, "%s", "");
        if (s->assign.target->kind == EX_INDEX) {
            dest = hx_arena_strdup(e->arena, "");
            hx_expr_str(e, s->assign.target, 0, db);
            /* %.* toma un int, y db->len es size_t: en x86-64 funciona por
               casualidad, pero el tipo no es el que dice el formato */
            dest = hx_arena_sprintf(e->arena, "%.*s = ", (int)db->len, db->data);
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
    /* un campo REF guarda la direccion: es un prestamo, no una copia */
    if (s->assign.target && s->assign.target->ty && s->assign.target->ty->kind == TY_REF &&
        s->assign.value && !s->assign.compound)
        hx_buf_str(b, "&");
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
            if (s->dim.ty && s->dim.ty->kind == TY_ARRAY && s->dim.ty->size < 0 &&
                !s->dim.init) {
                e->uses_darr = 1;
                e->uses_arena = 1;
                hx_indent(b, ind);
                hx_buf_printf(b, "hx_darr hx_v_%s = hx_darr_make();\n",
                              hx_sym_str(s->dim.name));
                break;
            }
            if (s->dim.ty && s->dim.ty->kind == TY_ARRAY && !s->dim.init) {
                e->uses_arena = 1;
                const char *el = hx_c_ty(e, s->dim.ty->elem);
                int64_t cnt = s->dim.ty->size;
                hx_indent(b, ind);
                hx_buf_printf(b, "%s hx_v_%s = hx_span_make(hx_arena_alloc(",
                              hx_c_ty(e, s->dim.ty), hx_sym_str(s->dim.name));
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
                /* `DIM d AS REF C = k` guarda la direccion: es un prestamo */
                if (s->dim.ty && s->dim.ty->kind == TY_REF) hx_buf_str(b, "&");
                hx_expr_str(e, s->dim.init, 0, b);
            } else if (s->dim.ty && s->dim.ty->kind == TY_STRING) {
                e->uses_string = 1;
                hx_buf_str(b, "hx_lit(\"\", 0)");
            } else if (s->dim.ty && s->dim.ty->kind == TY_ARRAY) {
                if (s->dim.ty->size < 0) {
                    e->uses_darr = 1;
                    hx_buf_str(b, "hx_darr_make()");
                } else {
                    hx_buf_str(b, "hx_span_make(NULL, 0)");
                }
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
        case ST_FORIN: {
            HxTy *elem = s->forin_.iter->ty && s->forin_.iter->ty->kind == TY_ITER
                             ? s->forin_.iter->ty->elem
                             : hx_ty_builtin(e->arena, TY_INT);
            e->uses_iter = 1;
            e->uses_arena = 1;
            hx_iter_note_elem(e, elem);
            int aname_idx = e->tmp++;
            char *aname = hx_arena_sprintf(e->arena, "hx_a%d", aname_idx);
            if (e->arena_depth < 16) e->arena_stack[e->arena_depth++] = aname;
            char it[64];
            if (s->forin_.iter->is_intrin == 4) {
                /* Rango(...).Map(...) y compañía: el estado del iterador vive
                   en una arena que se abre aqui */
                hx_indent(b, ind);
                hx_buf_printf(b, "hx_arena %s;\n", aname);
                hx_indent(b, ind);
                hx_buf_printf(b, "hx_arena_init(&%s);\n", aname);
                const char *arena = aname;
                HxBuf *saved = &e->out;
                e->out = *b;
                hx_iter_ctor(e, s->forin_.iter, arena, it, sizeof(it), ind);
                b->data = e->out.data;
                b->len = e->out.len;
                b->cap = e->out.cap;
                e->out = *saved;
            } else {
                /* Un ITER que ya existe (un parametro, un campo): no hay nada
                   que construir, solo recorrerlo */
                snprintf(it, sizeof(it), "hx_it%d", e->iter_n++);
                hx_indent(b, ind);
                hx_buf_printf(b, "hx_iter %s = ", it);
                hx_expr_str(e, s->forin_.iter, 0, b);
                hx_buf_str(b, ";\n");
            }
            hx_indent(b, ind);
            hx_buf_printf(b, "%s hx_itv%d;\n", hx_c_ty(e, elem), aname_idx);
            hx_indent(b, ind);
            hx_buf_printf(b, "while (%s.paso(%s.estado, &hx_itv%d)) {\n", it, it, aname_idx);
            hx_indent(b, ind + 1);
            hx_buf_printf(b, "%s hx_v_%s = hx_itv%d;\n", hx_c_ty(e, elem),
                          hx_sym_str(s->forin_.var), aname_idx);
            int lab = hx_body_has_defer(&s->forin_.body) ? hx_push_epilogue(e) : -1;
            hx_block(e, &s->forin_.body, ind + 1);
            if (lab >= 0) hx_epilogue_end(e, lab, &s->forin_.body, ind + 1);
            hx_indent(b, ind);
            hx_buf_str(b, "}\n");
            /* la arena solo existe si el iterable se construia aqui */
            if (s->forin_.iter->is_intrin == 4) {
                hx_indent(b, ind);
                hx_buf_printf(b, "hx_arena_free(&%s);\n", aname);
            }
            if (e->arena_depth) e->arena_depth--;
            break;
        }
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
            /* Cada CASE necesita sus ligaduras en su propio ambito: el guard
               las usa (`CASE x WHEN x > 10`) y las del CASE siguiente no pueden
               pisarlas. El `else if` encadenado no vale, porque al terminar un
               CASE sus ligaduras ya estan muertas y el siguiente las necesita
               vivas. Con una bandera los brazos quedan planos, en el orden del
               fuente, y solo corre el primero que cumple: que es lo que hace un
               `else if`, sin anidar el C. */
            int hay_casos = s->match.cases.len > 0;
            char *flag = hay_casos ? hx_arena_sprintf(e->arena, "hx_match%d", id) : NULL;
            if (hay_casos) {
                hx_indent(b, ind);
                hx_buf_printf(b, "int %s = 0;\n", flag);
            }
            for (int i = 0; i < s->match.cases.len; i++) {
                HxMatchCase *mc = &s->match.cases.data[i];
                hx_indent(b, ind);
                hx_buf_printf(b, "if (!%s) {\n", flag);
                hx_emit_pattern_bind(e, mc->pattern, subj, s->match.subject->ty, ind + 1);
                hx_indent(b, ind + 1);
                hx_buf_str(b, "if (");
                hx_emit_pattern_test(e, mc->pattern, subj, s->match.subject->ty);
                if (mc->guard) {
                    hx_buf_str(b, " && (");
                    hx_expr_str(e, mc->guard, 0, b);
                    hx_buf_str(b, ")");
                }
                hx_buf_str(b, ") {\n");
                hx_block(e, &mc->body, ind + 2);
                hx_indent(b, ind + 2);
                hx_buf_printf(b, "%s = 1;\n", flag);
                hx_indent(b, ind + 1);
                hx_buf_str(b, "}\n");
                hx_indent(b, ind);
                hx_buf_str(b, "}\n");
            }
            if (s->match.has_else) {
                hx_indent(b, ind);
                /* sin casos no hay bandera, y el else va suelto: antes se
                   imprimia un %s con NULL, que sale como "(null)" en el C */
                if (hay_casos) hx_buf_printf(b, "if (!%s) {\n", flag);
                else hx_buf_str(b, "{\n");
                hx_block(e, &s->match.else_body, ind + 1);
                hx_indent(b, ind);
                hx_buf_str(b, "}\n");
            }
            if (hay_casos) {
                hx_indent(b, ind);
                hx_buf_printf(b, "(void)%s;\n", flag);
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

/* Un CONST de cadena tiene que ser un inicializador constante de C: ni hx_lit
   ni hx_concat lo son. La expresion se dobla aqui y sale un solo literal. */
static int hx_const_str_fold(HxEmit *e, HxExpr *x, HxBuf *raw, int *ok) {
    if (!x) return 0;
    if (x->kind == EX_STR && !x->has_holes) {
        hx_buf_put(raw, x->str.raw, (size_t)x->str.len);
        return 1;
    }
    if (x->kind == EX_BIN && x->bin.op == OP_CONCAT)
        return hx_const_str_fold(e, x->bin.lhs, raw, ok) &&
               hx_const_str_fold(e, x->bin.rhs, raw, ok);
    if (!*ok) {
        *ok = 0;
        hx_error(e->diags, x->span, "E0310",
                 "un CONST de cadena tiene que ser un literal o una concatenacion de literales");
    }
    return 0;
}

static void hx_emit_decls(HxEmit *e, HxModule *m) {
    HxBuf *b = &e->out;
    if (!m->types.len) return;
    for (int i = 0; i < m->types.len; i++)
        hx_buf_printf(b, "typedef struct hx_T_%s hx_T_%s;\n", hx_sym_str(m->types.data[i].name),
                      hx_sym_str(m->types.data[i].name));
    for (int i = 0; i < m->types.len; i++) {
        HxTypeDecl *t = &m->types.data[i];
        if (t->is_enum) continue;
        hx_buf_printf(b, "struct hx_T_%s {\n", hx_sym_str(t->name));
        for (int j = 0; j < t->fields.len; j++)
            hx_buf_printf(b, "  %s %s;\n", hx_c_ty(e, t->fields.data[j].ty),
                          hx_sym_str(t->fields.data[j].name));
        hx_buf_str(b, "};\n");
        hx_buf_printf(b,
                      "static inline hx_T_%s hx_zero_rec_%s(void) {\n"
                      "  hx_T_%s v;\n"
                      "  __builtin_memset(&v, 0, sizeof(v));\n",
                      hx_sym_str(t->name), hx_sym_str(t->name), hx_sym_str(t->name));
        /* sin esto, escribir en t.celdas[0] seria escribir en un puntero nulo */
        for (int j = 0; j < t->fields.len; j++) {
            HxField *campo = &t->fields.data[j];
            if (!campo->ty || campo->ty->kind != TY_ARRAY) continue;
            if (campo->ty->size < 0) {
                hx_buf_printf(b, "  v.%s = hx_darr_make();\n", hx_sym_str(campo->name));
                e->uses_darr = 1;
                continue;
            }
            if (campo->ty->size <= 0) continue;
            if (!campo->ty->elem) continue;
            hx_buf_printf(b,
                          "  v.%s = hx_span_make(hx_arena_alloc(&hx_static_arena, %lld), %lld);\n",
                          hx_sym_str(campo->name),
                          (long long)(campo->ty->size * (int64_t)hx_size_of(e, campo->ty->elem)),
                          (long long)campo->ty->size);
        }
        hx_buf_str(b, "  return v;\n}\n");
    }
}

static void hx_emit_param_list(HxEmit *e, struct HxFunc *f, HxBuf *b);

static void hx_emit_func(HxEmit *e, struct HxFunc *f) {
    HxBuf *b = &e->out;
    hx_buf_printf(b, "%s%s hx_call_%s(", (f->is_export || f->is_instance) ? "" : "static ",

                  hx_c_ty(e, f->ret), hx_sym_str(f->name));
    hx_emit_param_list(e, f, b);
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

/* Los ayudantes de MAP dependen del par (origen, resultado); los de FILTER y
   TAKE sólo del tipo del elemento. */
static void hx_iter_map_helpers(HxBuf *b, const char *ct, const char *tag, const char *ct2,
                                const char *tag2) {
    hx_buf_printf(b, "typedef struct { hx_iter src; void *f; } hx_iter_st_map_%s_to_%s;\n", tag,
                  tag2);
    hx_buf_printf(b, "static inline int32_t hx_iter_paso_map_%s_to_%s(void *st, void *out) {\n",
                  tag, tag2);
    hx_buf_printf(b, "  hx_iter_st_map_%s_to_%s *s = (hx_iter_st_map_%s_to_%s *)st;\n", tag,
                  tag2, tag, tag2);
    hx_buf_printf(b, "  %s v;\n", ct);
    hx_buf_str(b, "  if (!s->src.paso(s->src.estado, &v)) return 0;\n");
    hx_buf_printf(b, "  *(%s *)out = ((%s (*)(%s))s->f)(v);\n", ct2, ct2, ct);
    hx_buf_str(b, "  return 1;\n}\n");
    hx_buf_printf(b,
                  "static inline hx_iter hx_iter_map_%s_to_%s(hx_arena *a, hx_iter src, "
                  "void *f) {\n",
                  tag, tag2);
    hx_buf_printf(b,
                  "  hx_iter_st_map_%s_to_%s *s = (hx_iter_st_map_%s_to_%s *)"
                  "hx_arena_alloc(a, sizeof(*s));\n",
                  tag, tag2, tag, tag2);
    hx_buf_str(b, "  s->src = src; s->f = f;\n");
    hx_buf_printf(b,
                  "  hx_iter it; it.estado = s; it.paso = hx_iter_paso_map_%s_to_%s; "
                  "return it;\n}\n",
                  tag, tag2);
}

static void hx_iter_filter_helpers(HxBuf *b, const char *ct, const char *tag) {
    hx_buf_printf(b, "typedef struct { hx_iter src; void *f; } hx_iter_st_filt_%s;\n", tag);
    hx_buf_printf(b, "static inline int32_t hx_iter_paso_filt_%s(void *st, void *out) {\n", tag);
    hx_buf_printf(b, "  hx_iter_st_filt_%s *s = (hx_iter_st_filt_%s *)st;\n", tag, tag);
    hx_buf_printf(b, "  %s v;\n", ct);
    hx_buf_str(b, "  while (s->src.paso(s->src.estado, &v)) {\n");
    hx_buf_printf(b, "    if (((int32_t (*)(%s))s->f)(v)) { *(%s *)out = v; return 1; }\n", ct,
                  ct);
    hx_buf_str(b, "  }\n  return 0;\n}\n");
    hx_buf_printf(b,
                  "static inline hx_iter hx_iter_filter_%s(hx_arena *a, hx_iter src, "
                  "void *f) {\n",
                  tag);
    hx_buf_printf(b,
                  "  hx_iter_st_filt_%s *s = (hx_iter_st_filt_%s *)hx_arena_alloc(a, "
                  "sizeof(*s));\n",
                  tag, tag);
    hx_buf_str(b, "  s->src = src; s->f = f;\n");
    hx_buf_printf(b,
                  "  hx_iter it; it.estado = s; it.paso = hx_iter_paso_filt_%s; return it;\n}\n",
                  tag);
}

static void hx_iter_take_helpers(HxBuf *b, const char *tag) {
    hx_buf_printf(b, "typedef struct { hx_iter src; int64_t n, vistos; } hx_iter_st_take_%s;\n",
                  tag);
    hx_buf_printf(b, "static inline int32_t hx_iter_paso_take_%s(void *st, void *out) {\n", tag);
    hx_buf_printf(b, "  hx_iter_st_take_%s *s = (hx_iter_st_take_%s *)st;\n", tag, tag);
    hx_buf_str(b, "  if (s->vistos >= s->n) return 0;\n");
    hx_buf_str(b, "  if (!s->src.paso(s->src.estado, out)) return 0;\n");
    hx_buf_str(b, "  s->vistos++;\n  return 1;\n}\n");
    hx_buf_printf(b,
                  "static inline hx_iter hx_iter_take_%s(hx_arena *a, hx_iter src, "
                  "int64_t n) {\n",
                  tag);
    hx_buf_printf(b,
                  "  hx_iter_st_take_%s *s = (hx_iter_st_take_%s *)hx_arena_alloc(a, "
                  "sizeof(*s));\n",
                  tag, tag);
    hx_buf_str(b, "  s->src = src; s->n = n; s->vistos = 0;\n");
    hx_buf_printf(b,
                  "  hx_iter it; it.estado = s; it.paso = hx_iter_paso_take_%s; return it;\n}\n",
                  tag);
}


/* registra el tipo de elemento para generar sus ayudantes una sola vez */
/* Un Push necesita un ayudante por tipo de elemento */
static void hx_darr_note_push(HxEmit *e, HxTy *elem) {
    if (!elem) return;
    const char *t = hx_c_ty(e, elem);
    for (int i = 0; i < e->darr_push_types.len; i++)
        if (!strcmp(e->darr_push_types.data[i], t)) return;
    if (e->darr_push_types.len >= 32) return;
    HX_VEC_PUSH(e->darr_push_types, t);
}

/* Un MAP de MAYBE necesita un ayudante por par de tipos: la firma en C depende
   de los dos, y escribir el ternario a mano se enreda con los literales
   compuestos. */
static void hx_maybe_note_map(HxEmit *e, HxTy *from, HxTy *to) {
    for (int i = 0; i < e->maybe_maps.len; i++)
        if (hx_ty_equal(e->maybe_maps.data[i].from, from) &&
            hx_ty_equal(e->maybe_maps.data[i].to, to))
            return;
    if (e->maybe_maps.len >= 16) return;
    HxTy *a = (HxTy *)hx_arena_calloc(e->arena, sizeof(HxTy));
    HxTy *b = (HxTy *)hx_arena_calloc(e->arena, sizeof(HxTy));
    *a = *from;
    *b = *to;
    HxIterMap *m = (HxIterMap *)hx_arena_calloc(e->arena, sizeof(HxIterMap));
    m->from = a;
    m->to = b;
    HX_VEC_PUSH(e->maybe_maps, *m);
}

static void hx_iter_note_elem(HxEmit *e, HxTy *t) {
    for (int i = 0; i < e->iter_elems.len; i++)
        if (hx_ty_equal(&e->iter_elems.data[i], t)) return;
    if (e->iter_elems.len >= 32) return;
    HxTy *copy = (HxTy *)hx_arena_calloc(e->arena, sizeof(HxTy));
    *copy = *t;
    HX_VEC_PUSH(e->iter_elems, *copy);
}

static void hx_iter_note_map(HxEmit *e, HxTy *from, HxTy *to) {
    for (int i = 0; i < e->iter_maps.len; i++)
        if (hx_ty_equal(e->iter_maps.data[i].from, from) &&
            hx_ty_equal(e->iter_maps.data[i].to, to))
            return;
    if (e->iter_maps.len >= 32) return;
    HxIterMap m;
    m.from = (HxTy *)hx_arena_calloc(e->arena, sizeof(HxTy));
    m.to = (HxTy *)hx_arena_calloc(e->arena, sizeof(HxTy));
    *m.from = *from;
    *m.to = *to;
    HX_VEC_PUSH(e->iter_maps, m);
}

/* Registra los tipos de una cadena de iteradores antes de escribir el
   runtime: la cabecera se emite antes que los modulos. */
static void hx_iter_note_chain(HxEmit *e, HxExpr *x) {
    while (x && x->kind == EX_CALL && x->is_intrin == 4) {
        const char *nm = hx_sym_str(x->method);
        HxTy *elem = x->ty && x->ty->kind == TY_ITER ? x->ty->elem : NULL;
        if (!hx_ascii_casecmp(nm, "Take") || !hx_ascii_casecmp(nm, "First")) {
            if (elem) hx_iter_note_elem(e, elem);
            hx_iter_note_chain(e, x->recv);
            return;
        }
        HxTy *from =
            x->recv && x->recv->ty && x->recv->ty->kind == TY_ITER ? x->recv->ty->elem : NULL;
        if (!hx_ascii_casecmp(nm, "Map")) {
            if (from) hx_iter_note_elem(e, from);
            if (elem) hx_iter_note_elem(e, elem);
            if (from && elem) hx_iter_note_map(e, from, elem);
        } else if (from) {
            hx_iter_note_elem(e, from);
        }
        hx_iter_note_chain(e, x->recv);
        return;
    }
}

static void hx_iter_suffix(HxEmit *e, HxTy *t, HxBuf *b) {
    char tag[96];
    hx_ty_mangle(t ? t : hx_ty_builtin(e->arena, TY_UNKNOWN), tag, sizeof(tag));
    hx_buf_str(b, tag);
}

static HxConst *hx_find_const_named(HxEmit *e, const char *name) {
    for (int m = 0; m < e->unit->modules.len; m++)
        for (int i = 0; i < e->unit->modules.data[m].consts.len; i++)
            if (!hx_ascii_casecmp(hx_sym_str(e->unit->modules.data[m].consts.data[i].name), name))
                return &e->unit->modules.data[m].consts.data[i];
    return NULL;
}

static struct HxFunc *hx_find_func_named(HxEmit *e, const char *name) {
    for (int m = 0; m < e->unit->modules.len; m++)
        for (int i = 0; i < e->unit->modules.data[m].funcs.len; i++)
            if (!hx_ascii_casecmp(hx_sym_str(e->unit->modules.data[m].funcs.data[i].name), name))
                return &e->unit->modules.data[m].funcs.data[i];
    return NULL;
}

/* Emite una cadena de iteradores dentro de la arena `arena` y devuelve el
   nombre de la variable C que la contiene. El recorrido es perezoso: sólo se
   construye el estado, nunca la secuencia. */
static void hx_iter_ctor(HxEmit *e, HxExpr *x, const char *arena, char *out, int cap, int ind) {
    const char *nm = hx_sym_str(x->method);
    e->uses_iter = 1;
    e->uses_arena = 1;
    if (!hx_ascii_casecmp(nm, "Rango") || !hx_ascii_casecmp(nm, "RangoF")) {
        int is_f = x->ty && x->ty->kind == TY_ITER && x->ty->elem && x->ty->elem->kind == TY_FLOAT;
        snprintf(out, (size_t)cap, "hx_it%d", e->iter_n++);
        hx_indent(&e->out, ind);
        hx_buf_printf(&e->out, "hx_iter %s = hx_iter_rango_%s(&%s, ", out, is_f ? "f" : "i",
                      arena);
        hx_expr_str(e, x->call.args.data[0].value, 0, &e->out);
        hx_buf_str(&e->out, ", ");
        hx_expr_str(e, x->call.args.data[1].value, 0, &e->out);
        if (is_f) hx_buf_str(&e->out, ", 1.0");
        hx_buf_str(&e->out, ");\n");
        return;
    }
    char inner[64];
    if (x->recv && x->recv->kind == EX_CALL) {
        hx_iter_ctor(e, x->recv, arena, inner, sizeof(inner), ind);
    } else {
        snprintf(inner, sizeof(inner), "hx_it%d", e->iter_n++);
        hx_indent(&e->out, ind);
        hx_buf_printf(&e->out, "hx_iter %s;\n", inner);
    }
    HxTy *t = x->recv && x->recv->ty && x->recv->ty->kind == TY_ITER ? x->recv->ty->elem : NULL;
    HxTy *out_t = x->ty && x->ty->kind == TY_ITER ? x->ty->elem : t;
    if (t) hx_iter_note_elem(e, t);
    if (out_t) hx_iter_note_elem(e, out_t);
    snprintf(out, (size_t)cap, "hx_it%d", e->iter_n++);
    hx_indent(&e->out, ind);
    if (!hx_ascii_casecmp(nm, "Take")) {
        hx_buf_printf(&e->out, "hx_iter %s = hx_iter_take_", out);
        hx_iter_suffix(e, t, &e->out);
        hx_buf_printf(&e->out, "(&%s, %s, ", arena, inner);
        hx_expr_str(e, x->call.args.data[0].value, 0, &e->out);
        hx_buf_str(&e->out, ");\n");
        return;
    }
    HxExpr *farg = x->call.args.data[0].value;
    struct HxFunc *fn = NULL;
    if (farg->kind == EX_FUNC && farg->lit) fn = farg->lit;
    else if (farg->kind == EX_PATH && farg->path.parts.len)
        fn = hx_find_func_named(e, hx_sym_str(farg->path.parts.data[farg->path.parts.len - 1].name));
    hx_buf_printf(&e->out, "hx_iter %s = hx_iter_%s_", out,
                  !hx_ascii_casecmp(nm, "Map") ? "map" : "filter");
    hx_iter_suffix(e, t, &e->out);
    if (!hx_ascii_casecmp(nm, "Map")) {
        hx_buf_str(&e->out, "_to_");
        hx_iter_suffix(e, out_t, &e->out);
    }
    hx_buf_printf(&e->out, "(&%s, %s, (void *)&hx_call_%s);\n", arena, inner,
                  hx_sym_str(fn ? fn->name : "?"));
}

static void hx_emit_iter_helpers(HxEmit *e, HxBuf *b) {
    HxBuf *saved = &e->out;
    e->out = *b;
    for (int i = 0; i < e->iter_maps.len; i++) {
        HxIterMap *m = &e->iter_maps.data[i];
        char c1[96], c2[96];
        hx_ty_mangle(m->from, c1, sizeof(c1));
        hx_ty_mangle(m->to, c2, sizeof(c2));
        hx_iter_map_helpers(&e->out, hx_c_ty(e, m->from), c1, hx_c_ty(e, m->to), c2);
        if (m->from->kind == TY_FLOAT) continue; /* MAP de FLOAT usa el mismo ayudante */
    }
    for (int i = 0; i < e->iter_elems.len; i++) {
        HxTy *t = &e->iter_elems.data[i];
        char tag[96];
        hx_ty_mangle(t, tag, sizeof(tag));
        hx_iter_filter_helpers(&e->out, hx_c_ty(e, t), tag);
        hx_iter_take_helpers(&e->out, tag);
    }
    b->data = e->out.data;
    b->len = e->out.len;
    b->cap = e->out.cap;
    e->out = *saved;
}

/* `.Ordinal` y `.ENUM_A_INT` dan el entero; `.Nombre` da el texto de la
   variante mediante una cadena constante generada por el compilador. */
static void hx_enum_member_str(HxEmit *e, HxExpr *x, HxBuf *b) {
    HxTypeDecl *ed = x->payload_ty && x->payload_ty->decl ? x->payload_ty->decl : NULL;
    const char *mn = hx_sym_str(x->method);
    if (ed && !hx_ascii_casecmp(mn, "Nombre")) {
        e->uses_string = 1;
        hx_buf_printf(b, "hx_enum_nombre_%s(", hx_sym_str(ed->name));
        hx_expr_str(e, x->recv, 0, b);
        hx_buf_str(b, ")");
        return;
    }
    hx_expr_str(e, x->recv, 0, b);
}

/* El nombre de una variante se decide en tiempo de ejecucion con una cadena
   constante por variante; el compilador la genera para cada ENUM usado. */
static void hx_emit_enum_names(HxEmit *e, HxBuf *b, HxUnit *unit) {
    for (int m = 0; m < unit->modules.len; m++)
        for (int i = 0; i < unit->modules.data[m].types.len; i++) {
            HxTypeDecl *td = &unit->modules.data[m].types.data[i];
            if (!td->is_enum) continue;
            hx_buf_printf(b, "static inline hx_str hx_enum_nombre_%s(int32_t v) {\n",
                          hx_sym_str(td->name));
            hx_buf_str(b, "  switch (v) {\n");
            for (int f = 0; f < td->fields.len; f++) {
                hx_buf_printf(b, "    case %d: return hx_lit(\"%s\", %d);\n", f,
                              hx_sym_str(td->fields.data[f].name),
                              (int)strlen(hx_sym_str(td->fields.data[f].name)));
            }
            hx_buf_str(b, "    default: return hx_lit(\"?\", 1);\n  }\n}\n");
        }
}

static void hx_emit_runtime_header(HxEmit *e, HxBuf *b) {
    hx_buf_printf(b, "/* runtime hxc %s */\n", HX_VERSION);
    hx_buf_str(b, "#ifndef HX_RUNTIME_H\n#define HX_RUNTIME_H\n");
    hx_buf_str(b, "#include <stdint.h>\n#include <stddef.h>\n#include <string.h>\n");
    hx_buf_str(b, e->profile == HX_PROFILE_FREESTANDING ? HX_RT_FREESTANDING : HX_RT_LIBC);
    if (e->profile == HX_PROFILE_FREESTANDING) {
        hx_buf_str(b, HX_RT_RAWALLOC_FREESTANDING);
    } else {
        hx_buf_str(b, HX_RT_RAWALLOC_LIBC);
    }
    hx_buf_str(b, HX_RT_TYPES);
    hx_buf_str(b, HX_RT_CORE);
    hx_buf_str(b, HX_RT_ITER_TYPE);
    if (e->uses_vec) {
        hx_buf_str(b, HX_RT_VEC_PRE);
        hx_buf_str(b, HX_RT_VEC);
    }
    if (e->uses_shift) hx_buf_str(b, HX_RT_SHIFT);
    if (e->uses_string) hx_buf_str(b, HX_RT_STRING);
    if (e->uses_tostring) hx_buf_str(b, HX_RT_TOSTRING);
    hx_buf_str(b, HX_RT_ZERO);
    if (e->uses_result) hx_buf_str(b, HX_RT_RESULT);
    hx_buf_str(b, HX_RT_CHECKED);
    if (e->uses_index) hx_buf_str(b, HX_RT_INDEX);
    if (e->uses_arena) {
        hx_buf_str(b, HX_RT_ARENA);
        hx_buf_str(b, "extern hx_arena hx_static_arena;\n");
        hx_buf_str(b, "void hx_static_init(void);\n");
    }
    if (e->uses_net) {
        e->uses_arena = 1;
        hx_buf_str(b, HX_NET_PRE);
        /* El net de libc es POSIX (sys/socket.h); en Windows hace falta Winsock,
           que no esta. Emitir los stubs es mejor que emitir una cabecera que no
           existe: el programa sigue compilando y las llamadas de red devuelven
           -1, que es lo que ya pasaria sin permiso. */
#if defined(_WIN32)
        hx_buf_str(b, HX_RT_NET_STUBS);
#else
        if (e->profile == HX_PROFILE_FREESTANDING) {
            hx_buf_str(b, HX_RT_NET_FREESTANDING);
            hx_buf_str(b, HX_RT_NET_STUBS);
        } else {
            hx_buf_str(b, HX_RT_NET_LIBC);
        }
#endif
    }
    /* el arreglo dinámico usa la arena, asi que sus ayudantes van despues */
    if (e->uses_darr) hx_buf_str(b, HX_RT_DARR);
    if (e->uses_iter) {
        hx_buf_str(b, HX_RT_ITER);
        hx_emit_iter_helpers(e, b);
    }
    if (e->uses_string) {
        hx_buf_str(b, HX_RT_STRFUNS);
        hx_buf_str(b, HX_RT_STRMORE);
    }
    if (e->uses_enum) hx_emit_enum_names(e, b, e->unit);
    /* El perfil libc ya tiene memcpy y memset, y declararlos otra vez pisa los
       prototipos: en macOS, ademas, memcpy es una macro. El freestanding es el
       unico caso en el que hacen falta, porque sus definiciones viven en otro
       archivo y hay que declararlas antes de usarlas. */
    if (e->profile == HX_PROFILE_FREESTANDING) hx_buf_str(b, HX_RT_MEM_DECL);
    hx_buf_str(b, "#endif\n");
}

static void hx_emit_param_list(HxEmit *e, struct HxFunc *f, HxBuf *b) {
    if (!f->params.len) {
        hx_buf_str(b, "void");
        return;
    }
    for (int j = 0; j < f->params.len; j++) {
        if (j) hx_buf_str(b, ", ");
        hx_buf_printf(b, "%s hx_v_%s", hx_c_ty(e, f->params.data[j].ty),
                      hx_sym_str(f->params.data[j].name));
    }
}


/* las instancias se declaran en la cabecera del modulo que las define para que
   cualquier unidad de traduccion que lo importe pueda llamarlas */
/* las instancias de un TYPE generico se definen en la cabecera del modulo que
   las origina, junto a las del resto de sus tipos */
static void hx_emit_type_instances(HxEmit *e, HxUnit *unit, HxModule *m, HxBuf *b) {
    for (int i = 0; i < unit->n_type_instances; i++) {
        HxTypeDecl *td = unit->type_instances[i];
        if (td->module != m->index) continue;
        hx_buf_printf(b, "typedef struct hx_T_%s hx_T_%s;\n", hx_sym_str(td->name),
                      hx_sym_str(td->name));
    }
    for (int i = 0; i < unit->n_type_instances; i++) {
        HxTypeDecl *td = unit->type_instances[i];
        if (td->module != m->index) continue;
        hx_buf_printf(b, "struct hx_T_%s {\n", hx_sym_str(td->name));
        for (int j = 0; j < td->fields.len; j++)
            hx_buf_printf(b, "  %s %s;\n", hx_c_ty(e, td->fields.data[j].ty),
                          hx_sym_str(td->fields.data[j].name));
        hx_buf_str(b, "};\n");
        hx_buf_printf(b,
                      "static inline hx_T_%s hx_zero_rec_%s(void) {\n"
                      "  hx_T_%s v;\n"
                      "  __builtin_memset(&v, 0, sizeof(v));\n"
                      "  return v;\n"
                      "}\n",
                      hx_sym_str(td->name), hx_sym_str(td->name), hx_sym_str(td->name));
    }
}

static void hx_emit_instance_decls(HxEmit *e, HxUnit *unit, HxModule *m, HxBuf *b) {
    for (int i = 0; i < unit->n_instances; i++) {
        struct HxFunc *inst = unit->instances[i];
        if (inst->module != m->index) continue;
        hx_buf_printf(b, "extern %s hx_call_%s(", hx_c_ty(e, inst->ret), hx_sym_str(inst->name));
        hx_emit_param_list(e, inst, b);
        hx_buf_str(b, ");\n");
    }
}

/* Un MAYBE<T> es un struct con una bandera y el valor, y sus ayudantes. */
static void hx_emit_maybe_types(HxEmit *e, HxBuf *b) {
    /* El typedef va en la cabecera de cada modulo, porque un MAYBE de un TYPE
       declarado necesita hx_T_Nombre antes de existir. Con dos modulos los dos
       lo escriben, asi que cada bloque va con su guarda: gana el primero, y los
       demas son el mismo texto. Que gane el correcto lo asegura el orden de
      includes: un modulo que usa un TYPE siempre incluye antes al que lo
       declara. */
    for (int i = 0; i < e->maybe_inners.len; i++) {
        const char *t = e->maybe_inners.data[i];
        hx_buf_printf(b, "#ifndef HX_MAYBE_%s\n#define HX_MAYBE_%s\n", t, t);
        hx_buf_printf(b, "typedef struct { uint8_t hay; %s valor; } hx_maybe_%s;\n", t, t);
        hx_buf_printf(b, "static inline hx_maybe_%s hx_maybe_some_%s(%s v) {\n", t, t, t);
        hx_buf_printf(b, "  hx_maybe_%s m; m.hay = 1; m.valor = v; return m; }\n", t);
        hx_buf_printf(b, "static inline %s hx_maybe_or_%s(hx_maybe_%s m, %s otro) {\n", t, t,
                      t, t);
        hx_buf_str(b, "  return m.hay ? m.valor : otro; }\n");
        hx_buf_str(b, "#endif\n");
    }
    for (int i = 0; i < e->darr_push_types.len; i++) {
        const char *t = e->darr_push_types.data[i];
        /* con dos modulos los dos escriben el ayudante, igual que los de MAYBE */
        char macro[96];
        int k = 0;
        for (const char *p = t; *p && k + 1 < (int)sizeof(macro); p++)
            macro[k++] = ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                           (*p >= '0' && *p <= '9'))
                              ? *p
                              : '_';
        macro[k] = 0;
        hx_buf_printf(b, "#ifndef HX_DARR_PUSH_%s\n#define HX_DARR_PUSH_%s\n", macro, macro);
        hx_buf_printf(b,
                      "static inline hx_darr hx_darr_push_%s(hx_arena *a, hx_darr d, %s v) {\n"
                      "  return hx_darr_push(a, d, sizeof(%s), &v);\n}\n",
                      t, t, t);
        hx_buf_str(b, "#endif\n");
    }
    for (int i = 0; i < e->maybe_maps.len; i++) {
        HxIterMap *mp = &e->maybe_maps.data[i];
        const char *desde = hx_c_ty(e, mp->from->elem);
        const char *hasta = hx_c_ty(e, mp->to->elem);
        hx_buf_printf(b, "#ifndef HX_MAYBE_MAP_%s_%s\n#define HX_MAYBE_MAP_%s_%s\n",
                      desde, hasta, desde, hasta);
        hx_buf_printf(b, "typedef hx_maybe_%s (*hx_maybe_f_%s)(%s);\n", hasta, hasta, desde);
        hx_buf_printf(b, "static inline hx_maybe_%s hx_maybe_map_%s_%s(hx_maybe_%s m, "
                         "hx_maybe_f_%s f) {\n  hx_maybe_%s r; r.hay = 0;\n",
                      hasta, desde, hasta, desde, hasta, hasta);
        hx_buf_str(b, "  if (m.hay) r = f(m.valor);\n  return r; }\n");
        hx_buf_str(b, "#endif\n");
    }
}

static void hx_emit_module_header(HxEmit *e, HxUnit *unit, HxModule *m, HxBuf *b) {
    char *guard = hx_arena_sprintf(e->arena, "HX_MOD_%s_H", hx_sym_str(m->name));
    for (char *p = guard; *p; p++) *p = hx_ascii_upper(*p);
    hx_buf_printf(b, "/* modulo %s */\n", hx_sym_str(m->name));
    hx_buf_printf(b, "#ifndef %s\n#define %s\n", guard, guard);
    hx_buf_str(b, "#include \"_runtime.h\"\n");
    hx_emit_decls(e, m);
    /* los typedef de MAYBE van aqui: un TYPE declarado se convierte en
       hx_T_Nombre en este mismo punto, y el typedef lo necesita */
    hx_emit_maybe_types(e, b);
    for (int k = 0; k < m->consts.len; k++) {
        HxConst *kc = &m->consts.data[k];
        if (!kc->value || !kc->is_export) continue;
        hx_buf_printf(b, "extern const %s hx_v_%s;\n", hx_c_ty(e, kc->ty),
                      hx_sym_str(kc->name));
    }
    for (int i = 0; i < m->funcs.len; i++) {
        struct HxFunc *f = &m->funcs.data[i];
        if (!f->is_export || f->is_generic) continue;
        hx_buf_printf(b, "extern %s hx_call_%s(", hx_c_ty(e, f->ret),
                      hx_sym_str(f->name));
        if (!f->params.len) hx_buf_str(b, "void");
        for (int j = 0; j < f->params.len; j++) {
            if (j) hx_buf_str(b, ", ");
            hx_buf_printf(b, "%s hx_v_%s", hx_c_ty(e, f->params.data[j].ty),
                          hx_sym_str(f->params.data[j].name));
        }
        hx_buf_str(b, ");\n");
    }
    for (int i = 0; i < m->impls.len; i++)
        for (int j = 0; j < m->impls.data[i].methods.len; j++) {
            struct HxFunc *f = &m->impls.data[i].methods.data[j];
            hx_buf_printf(b, "extern %s hx_call_%s(", hx_c_ty(e, f->ret), hx_sym_str(f->name));
            hx_emit_param_list(e, f, b);
            hx_buf_str(b, ");\n");
        }
    hx_emit_type_instances(e, unit, m, b);
    hx_emit_instance_decls(e, unit, m, b);
    hx_buf_printf(b, "#endif /* %s */\n", guard);
}

static void hx_emit_module_source(HxEmit *e, HxUnit *unit, HxModule *m, HxBuf *b,
                                  int emit_funcs) {
    hx_buf_printf(b, "/* %s -- generado por hxc %s */\n", hx_sym_str(m->name), HX_VERSION);
    hx_buf_str(b, "#include \"_runtime.h\"\n");
    for (int i = 0; i < m->imports.len; i++) {
        HxImport *im = &m->imports.data[i];
        const char *ip = hx_sym_str(im->path);
        const char *dot = strrchr(ip, '.');
        const char *base = dot ? dot + 1 : ip;
        HxSym as = im->alias ? im->alias : hx_intern_cstr(unit->intern, base);
        /* La cabecera generada se llama como el módulo, no como el alias: con
           `IMPORT std.texto` el módulo es std.texto y se llama texto, así que
           incluir "texto.h" no encontraba nada. */
        const char *cabecera = hx_sym_str(as);
        for (int k = 0; k < unit->modules.len; k++)
            if (!hx_ascii_casecmp(hx_sym_str(unit->modules.data[k].name), ip)) {
                cabecera = hx_sym_str(unit->modules.data[k].name);
                break;
            }
        hx_buf_printf(b, "#include \"%s.h\"\n", cabecera);
    }
    hx_buf_printf(b, "#include \"%s.h\"\n", hx_sym_str(m->name));
    for (int k = 0; k < m->consts.len; k++) {
        HxConst *kc = &m->consts.data[k];
        if (!kc->value) continue;
        int es_cadena = kc->ty && kc->ty->kind == TY_STRING;
        if (!kc->is_export)
            hx_buf_printf(b, "static %s%s hx_v_%s = ", es_cadena ? "" : "const ",
                          hx_c_ty(e, kc->ty), hx_sym_str(kc->name));
        else
            hx_buf_printf(b, "%s%s hx_v_%s __attribute__((used)) = ", es_cadena ? "" : "const ",
                          hx_c_ty(e, kc->ty), hx_sym_str(kc->name));
        if (es_cadena) {
            HxBuf crudo;
            int ok = 1;
            memset(&crudo, 0, sizeof(crudo));
            if (hx_const_str_fold(e, kc->value, &crudo, &ok)) {
                hx_buf_str(b, "{");
                hx_put_c_string(b, crudo.data ? crudo.data : "", (int)crudo.len);
                hx_buf_printf(b, ", %d}", (int)crudo.len);
            } else {
                hx_expr_str(e, kc->value, 0, b);
            }
        } else {
            hx_expr_str(e, kc->value, 0, b);
        }
        hx_buf_str(b, ";\n");
    }
    for (int i = 0; i < unit->n_instances; i++) {
        struct HxFunc *inst = unit->instances[i];
        if (inst->module != m->index) continue;
        hx_emit_func(e, inst);
    }
    if (!emit_funcs) return;
    for (int j = 0; j < m->funcs.len; j++) {
        struct HxFunc *f = &m->funcs.data[j];
        if (f->is_export || f->is_generic) continue;
        hx_buf_printf(b, "static %s hx_call_%s(", hx_c_ty(e, f->ret), hx_sym_str(f->name));
        hx_emit_param_list(e, f, b);
        hx_buf_str(b, ");\n");
    }
    for (int i = 0; i < unit->n_lambdas; i++)
        if (unit->lambdas[i]->module == m->index) hx_emit_func(e, unit->lambdas[i]);
    for (int i = 0; i < m->impls.len; i++)
        for (int j = 0; j < m->impls.data[i].methods.len; j++)
            hx_emit_func(e, &m->impls.data[i].methods.data[j]);
    for (int j = 0; j < m->funcs.len; j++)
        if (!m->funcs.data[j].is_generic) hx_emit_func(e, &m->funcs.data[j]);
}

int hx_emit_unit(HxArena *arena, HxUnit *unit, HxEmitOptions *opt) {
    HxEmit e;
    memset(&e, 0, sizeof(e));
    e.arena = arena;
    e.unit = unit;
    e.uses_enum = 0;
    e.diags = unit->diags;
    e.profile = opt->profile;
    e.out.arena = arena;

    for (int i = 0; i < unit->modules.len; i++) {
        HxModule *m = &unit->modules.data[i];
        hx_scan_body(&e, &m->top);
        for (int j = 0; j < m->funcs.len; j++) {
            hx_scan_body(&e, &m->funcs.data[j].body);
            if (m->funcs.data[j].ret && m->funcs.data[j].ret->kind == TY_MAYBE)
                hx_c_ty(&e, m->funcs.data[j].ret);
            for (int q = 0; q <= m->funcs.data[j].params.len; q++) {
                HxTy *pt = q == 0 ? m->funcs.data[j].ret : m->funcs.data[j].params.data[q - 1].ty;
                if (pt && pt->kind == TY_ARRAY && pt->size < 0) {
                    e.uses_darr = 1;
                    e.uses_arena = 1;
                }
            }
            for (int q = 0; q < m->funcs.data[j].params.len; q++)
                if (hx_ty_is_result(m->funcs.data[j].params.data[q].ty)) e.uses_result = 1;
        }
        for (int t = 0; t < m->types.len; t++) {
            if (m->types.data[t].is_enum) {
                e.uses_enum = 1;
                e.uses_string = 1;
            }
            for (int q = 0; q < m->types.data[t].fields.len; q++) {
                HxTy *ft = m->types.data[t].fields.data[q].ty;
                if (ft && ft->kind == TY_MAYBE) hx_c_ty(&e, ft);
                if (hx_ty_is_result(ft)) e.uses_result = 1;
                if (ft && hx_vec_len(ft)) e.uses_vec = 1;
                /* un campo de arreglo se reserva al construir el registro */
                if (ft && ft->kind == TY_ARRAY && ft->size > 0) e.uses_arena = 1;
            }
        }
        if (m->is_entry && m->top.len) hx_scan_body(&e, &m->top);
        for (int j = 0; j < m->funcs.len; j++) hx_scan_body(&e, &m->funcs.data[j].body);
        for (int im = 0; im < m->impls.len; im++)
            for (int mt = 0; mt < m->impls.data[im].methods.len; mt++)
                hx_scan_body(&e, &m->impls.data[im].methods.data[mt].body);
    }
    for (int i = 0; i < unit->n_instances; i++) hx_scan_body(&e, &unit->instances[i]->body);

    HxBuf rt = {arena, NULL, 0, 0};
    {
        HxBuf saved_r = e.out;
        e.out = rt;
        hx_emit_runtime_header(&e, &e.out);
        rt = e.out;
        e.out = saved_r;
    }
    if (hx_write_file(opt->dir_runtime, rt.data, rt.len) != 0) {
        fprintf(stderr, "hx: no se pudo escribir %s\n", opt->dir_runtime);
        return 1;
    }

    if (opt->profile == HX_PROFILE_FREESTANDING) {
        HxBuf mb = {arena, NULL, 0, 0};
        hx_buf_str(&mb, "/* memcpy/memset propios del perfil freestanding */\n");
        hx_buf_str(&mb, "#include \"_runtime.h\"\n");
        hx_buf_str(&mb, HX_RT_FREESTANDING_MEM);
        char *mp2 = hx_arena_sprintf(arena, "%s/_rtmem.c", opt->dir_gen);
        if (hx_write_file(mp2, mb.data, mb.len) != 0) return 1;
        free(mb.data);
    }

    HxModule *entry = NULL;
    for (int i = 0; i < unit->modules.len; i++)
        if (unit->modules.data[i].is_entry) entry = &unit->modules.data[i];

    for (int i = 0; i < unit->modules.len; i++) {
        HxModule *m = &unit->modules.data[i];
        HxBuf hb = {arena, NULL, 0, 0};
        HxBuf saved_h = e.out;
        e.out = hb;
        hx_emit_module_header(&e, unit, m, &e.out);
        hb = e.out;
        e.out = saved_h;
        char *hp = hx_arena_sprintf(arena, "%s/%s.h", opt->dir_gen, hx_sym_str(m->name));
        if (hx_write_file(hp, hb.data, hb.len) != 0) return 1;
        free(hb.data);
        if (!m->is_entry) {
            HxBuf cb = {arena, NULL, 0, 0};
            HxBuf saved_c = e.out;
            e.out = cb;
            hx_emit_module_source(&e, unit, m, &e.out, 1);
            cb = e.out;
            e.out = saved_c;
            char *cp = hx_arena_sprintf(arena, "%s/%s.c", opt->dir_gen, hx_sym_str(m->name));
            if (hx_write_file(cp, cb.data, cb.len) != 0) return 1;
            free(cb.data);
        }
    }

    HxBuf main_b = {arena, NULL, 0, 0};
    hx_buf_printf(&main_b, "/* punto de entrada -- generado por hxc %s */\n", HX_VERSION);
    hx_buf_str(&main_b, "#include \"_runtime.h\"\n");
    for (int i = 0; i < unit->modules.len; i++)
        hx_buf_printf(&main_b, "#include \"%s.h\"\n", hx_sym_str(unit->modules.data[i].name));
    if (opt->profile == HX_PROFILE_FREESTANDING) hx_buf_str(&main_b, "void _start(void);\n");
    if (entry) {
        HxBuf sv = e.out;
        e.out = main_b;
        hx_emit_module_source(&e, unit, entry, &e.out, 0);
        main_b = e.out;
        e.out = sv;
    }
    if (entry) {
        HxBuf saved_f = e.out;
        e.out = main_b;
        for (int i = 0; i < unit->n_lambdas; i++)
            if (unit->lambdas[i]->module == entry->index)
                hx_buf_printf(&e.out, "static %s hx_call_%s(",
                              hx_c_ty(&e, unit->lambdas[i]->ret),
                              hx_sym_str(unit->lambdas[i]->name)), hx_emit_param_list(&e, unit->lambdas[i], &e.out),
                hx_buf_str(&e.out, ");\n");
        for (int j = 0; j < entry->funcs.len; j++) {
            struct HxFunc *f = &entry->funcs.data[j];
            if (f->is_export || f->is_generic) continue;
            hx_buf_printf(&e.out, "static %s hx_call_%s(", hx_c_ty(&e, f->ret),
                          hx_sym_str(f->name));
            hx_emit_param_list(&e, f, &e.out);
            hx_buf_str(&e.out, ");\n");
        }
        main_b = e.out;
        e.out = saved_f;
    }
    hx_buf_str(&main_b, "static int32_t hx_main(void);\n");
    hx_buf_str(&main_b, "static int32_t hx_main(void) {\n");
    if (e.uses_arena) {
        hx_buf_str(&main_b, "  hx_static_init();\n");
    }
    if (entry) {
        HxBuf saved = e.out;
        e.out = main_b;
        hx_body(&e, &entry->top, 1);
        main_b = e.out;
        e.out = saved;
    }
    hx_buf_str(&main_b, "  return 0;\n}\n\n");
    if (entry) {
        HxBuf saved = e.out;
        e.out = main_b;
        for (int i = 0; i < unit->n_lambdas; i++)
            if (unit->lambdas[i]->module == entry->index) hx_emit_func(&e, unit->lambdas[i]);
        for (int i = 0; i < entry->impls.len; i++)
            for (int j = 0; j < entry->impls.data[i].methods.len; j++)
                hx_emit_func(&e, &entry->impls.data[i].methods.data[j]);
        for (int j = 0; j < entry->funcs.len; j++)
            if (!entry->funcs.data[j].is_generic) hx_emit_func(&e, &entry->funcs.data[j]);
        main_b = e.out;
        main_b = e.out;
        e.out = saved;
    }
    if (e.uses_arena) {
        hx_buf_str(&main_b,
                   "hx_arena hx_static_arena;\n"
                   "void hx_static_init(void) { hx_arena_init(&hx_static_arena); }\n");
    }
    if (opt->profile == HX_PROFILE_FREESTANDING)
        /* El kernel llama a _start con la pila alineada a 16 y luego empuja
           argc: rsp queda a 8, pero el ABI de SysV supone 16 en cada entrada
           de funcion. Con escalares no se nota; en cuanto un struct se copia
           con movaps, revienta. */
        hx_buf_str(&main_b,
                   "void _start(void) {\n"
                   "  __asm__ volatile(\"andq $-16, %rsp\");\n"
                   "  hx_exit((int)hx_main());\n  __builtin_unreachable();\n}\n");
    else
        hx_buf_str(&main_b, "int main(void) { return (int)hx_main(); }\n");
    char *mp = hx_arena_sprintf(arena, "%s/_entry.c", opt->dir_gen);
    if (hx_write_file(mp, main_b.data, main_b.len) != 0) return 1;
    free(main_b.data);

    if (opt->keep_asm) {
        HxBuf all = {arena, NULL, 0, 0};
        for (int i = 0; i < unit->modules.len; i++) {
            HxBuf cb = {arena, NULL, 0, 0};
            HxBuf saved_k = e.out;
            e.out = cb;
            hx_emit_module_source(&e, unit, &unit->modules.data[i], &e.out, 0);
            cb = e.out;
            e.out = saved_k;
            hx_buf_put(&all, cb.data ? cb.data : "", cb.len);
            free(cb.data);
        }
        FILE *f = fopen(opt->keep_asm_path, "wb");
        if (f) {
            fwrite(rt.data, 1, rt.len, f);
            fwrite(all.data, 1, all.len, f);
            fclose(f);
        }
        free(all.data);
    }
    free(rt.data);
    return 0;
}
