#define _POSIX_C_SOURCE 200809L

#include "hx/check.h"
#include "hx/diag.h"
#include "hx/emit.h"
#include "hx/hxc.h"
#include "hx/kit.h"
#include "hx/parse.h"

#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/wait.h>
#endif
#include <sys/stat.h>
#include <time.h>

#ifndef _WIN32
#include <sys/wait.h>
#endif

/* execvp y execv piden char *const argv[] y la arena devuelve const char *:
   copiar el puntero una vez es mas barato que mentir sobre el const. Y el scratch
   de los valores por defecto lo usan los dos caminos, asi que va fuera del
   condicional: estaba dentro del de POSIX y en Windows no existia. */
static char *hx_arg(HxArena *a, const char *s);
static HxArena g_arena_scratch;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> /* CreateProcess y companhia, para hx_exec */
#include <process.h>
#define HX_EXEC(p, a) _spawnvp(_P_WAIT, p, a)
#define HX_STRCPY_STRDUP(d, s) ((d) = _strdup(s))
#else
#include <unistd.h>
#define HX_EXEC(p, a) execvp(p, a)
#define HX_STRCPY_STRDUP(d, s) ((d) = strdup(s))
#endif

typedef struct {
    int optimize;
    int keep_asm;
    const char *keep_asm_path;
    const char *out_bin;
    const char *emit_hxc;
    const char *use_hxc;
    int jobs; /* 0 = tantos como nucleos */
    const char *kit_file;
    const char *defines[HX_KIT_MAX];
    int n_defines;
    const char *extra_opts[HX_KIT_MAX];
    int n_opts;
    const char *mod_dirs[HX_KIT_MAX];
    int n_mod_dirs;
    /* -I: directorios donde buscar IMPORT, antes que los del manifiesto */
    const char *include[HX_KIT_MAX];
    int n_include;
    HxProfile profile;
    int emit_only;
    int verbose;
} HxBuildOpts;

static double hx_now_ms(void) {
#ifdef _WIN32
    return (double)clock() / (CLOCKS_PER_SEC / 1000.0);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

#ifndef _WIN32
static void hx_execvp(const char *const argv[]);
#endif

/* Los vectores de argumentos son const de principio a fin: execvp es la unica
   llamada que no los admite, y ahi se copia el vector de punteros una vez. */
static int hx_run(const char *const argv[]) {
#ifdef _WIN32
    /* _spawnvp toma const char *const argv[]: el vector se declara const desde
       el principio, porque quitarle el const con un cast es lo que hacia falta y
       ademas -Wcast-qual lo prohibe */
    const char *ejecucion[1024];
    int n = 0;
    while (argv[n] && n < 1023) {
        ejecucion[n] = argv[n];
        n++;
    }
    ejecucion[n] = NULL;
    intptr_t r = _spawnvp(_P_WAIT, ejecucion[0], (const char *const *)ejecucion);
    return (int)r;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        hx_execvp(argv);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

/* execvp es la unica llamada del compilador que no admite char *const. Se copia
   el puntero con memcpy en vez de castear: es lo mismo en ensamblador y no
   obliga a silenciar -Wcast-qual en todo el fichero. */
#ifndef _WIN32
static void hx_execvp(const char *const argv[]) {
    char *ejecucion[1024];
    int n = 0;
    while (argv[n] && n < 1023) {
        memcpy(&ejecucion[n], &argv[n], sizeof(char *));
        n++;
    }
    ejecucion[n] = NULL;
    execvp(ejecucion[0], ejecucion);
}
#endif

/* Lanza cc sin esperar: permite compilar varias unidades a la vez. */

static pid_t hx_spawn(const char *const argv[]) {
#ifdef _WIN32
    return 0;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        hx_execvp(argv);
        _exit(127);
    }
    return pid;
#endif
}

/* Espera un proceso. En Windows no hace falta: los procesos se lanzan con
   _spawnvp(_P_WAIT), que ya espera y devuelve el codigo de salida, asi que hx_wait
   solo se usa en la compilacion paralela de POSIX. */
#ifdef _WIN32
static int hx_wait(int pid) {
    (void)pid;
    return 0;
}
#else
static int hx_wait(pid_t pid) {
    if (pid <= 0) return 0;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
#endif

static int hx_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (int)st.st_size;
}

typedef struct {
    HxArena arena;
    HxIntern *intern;
    HxDiagBag diags;
    HxUnit unit;
    const char *hxc_dir;
} HxSession;

static void hx_session_init(HxSession *s) {
    memset(s, 0, sizeof(*s));
    hx_arena_init(&s->arena);
    s->intern = hx_intern_new(&s->arena);
    hx_diag_init(&s->diags, &s->arena);
    s->diags.max_errors = 40;
    s->unit.arena = &s->arena;
    s->unit.intern = s->intern;
    s->unit.diags = &s->diags;
}

static HxModule *hx_load_module(HxSession *s, const char *path, int is_entry) {
    size_t len = 0;
    char *src = hx_read_file(&s->arena, path, &len);
    if (!src) return NULL;
    HxModule tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.index = s->unit.modules.len;
    tmp.is_entry = is_entry;
    HX_VEC_PUSH(s->unit.modules, tmp);
    HxModule *m = &s->unit.modules.data[s->unit.modules.len - 1];
    hx_parse_module(&s->unit, m, src, path);
    return m;
}

static int o_verbose;

/* La cache de objetos se invalida cuando cambia el compilador. Con la sola
   version no vale: durante el desarrollo de hxc el numero no cambia y un .o
   viejo se reutiliza, produciendo un binario roto sin avisar. Se hashea el
   ejecutable una vez por sesion; son ~900 KB y se lee una vez. */
static const char *g_hxc_build_id;
static const char *g_hxc_self;

/* Dónde está este ejecutable. Con eso se sabe de dos cosas que antes no se
   sabían: qué biblioteca estándar viene con el compilador, y qué binario hay
   que hashear para invalidar la caché de objetos. Antes ponía "build/hxc" a pelo
   y, si no encontraba el archivo, caía a la versión: invocar hxc desde otro
   directorio invalidaba la caché por versión y no por binario. */
static const char *g_argv0 = "hxc";

static void hx_calc_self(HxSession *s) {
    char buf[1024];
    ssize_t n = 0;
#ifndef _WIN32
    n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
#endif
    if (n > 0) {
        buf[n] = 0;
        g_hxc_self = hx_arena_strdup(&s->arena, buf);
    } else if (g_argv0 && *g_argv0) {
        g_hxc_self = hx_arena_strdup(&s->arena, g_argv0);
    } else {
        g_hxc_self = NULL;
    }
}

static void hx_calc_build_id(HxSession *s) {
    HxHash h;
    hx_fnv_init(&h);
    size_t n = 0;
    char *data = g_hxc_self ? hx_read_file(&s->arena, g_hxc_self, &n) : NULL;
    if (!data) {
        /* no se puede leer el ejecutable: se usa la version y ya */
        hx_fnv_str(&h, HX_VERSION);
    } else {
        hx_fnv_bytes(&h, data, (size_t)n);
    }
    char hex[20];
    hx_fnv_hex(&h, hex, sizeof(hex));
    g_hxc_build_id = hx_arena_strdup(&s->arena, hex);
}

static const char **g_extra_mod_dirs;
static int g_n_extra_mod_dirs;
static int g_cap_extra_mod_dirs;

/* La lista de directorios donde se buscan los IMPORT. No es el array fijo de
   HX_KIT_MAX que usan las dependencias de un manifiesto: aqui caben tambien los
   -I, HX_LIB y la biblioteca del compilador, y su número no tiene un tope
   razonable. */
static void hx_busqueda_add(HxSession *s, const char *dir) {
    if (!dir || !*dir) return;
    for (int i = 0; i < g_n_extra_mod_dirs; i++)
        if (!strcmp(g_extra_mod_dirs[i], dir)) return;
    if (g_n_extra_mod_dirs >= g_cap_extra_mod_dirs) {
        int cap = g_cap_extra_mod_dirs ? g_cap_extra_mod_dirs * 2 : 8;
        const char **nuevo = (const char **)hx_arena_alloc(
            &s->arena, sizeof(const char *) * (size_t)cap);
        for (int i = 0; i < g_n_extra_mod_dirs; i++) nuevo[i] = g_extra_mod_dirs[i];
        g_extra_mod_dirs = nuevo;
        g_cap_extra_mod_dirs = cap;
    }
    g_extra_mod_dirs[g_n_extra_mod_dirs++] = hx_arena_strdup(&s->arena, dir);
}

/* Un IMPORT puede venir con puntos: "std.texto". Se prueban las dos formas, y
   la primera que exista gana. El orden es el de siempre (el nombre corto antes
   que el largo) para no cambiar lo que ya funciona, y dentro de cada directorio
   primero el corto y luego el con puntos. */
static const char *hx_modulo_en(HxArena *a, const char *dir, const char *sp,
                                const char *ext) {
    const char *dot = strrchr(sp, '.');
    if (dot) {
        char *corto = hx_arena_sprintf(a, "%s/%.*s%s", dir, (int)(dot - sp), sp, ext);
        if (hx_file_exists(corto)) return corto;
        char *largo = hx_arena_sprintf(a, "%s/%s%s", dir, sp, ext);
        if (hx_file_exists(largo)) return largo;
        return NULL;
    }
    char *c = hx_arena_sprintf(a, "%s/%s%s", dir, sp, ext);
    return hx_file_exists(c) ? c : NULL;
}

/* El orden es lo explicito primero y lo que viene con el compilador al final:
   el directorio de la entrada lo busca hx_collect_modules2, y despues -I, HX_LIB,
   los directorios del manifiesto y la biblioteca que vino con este hxc. La lista
   es global porque hay dos sitios que compilan (hx build/run y hxc test) y los dos
   tienen que buscar igual. */
static void hx_preparar_busqueda(HxSession *s, const HxBuildOpts *o) {
    g_extra_mod_dirs = NULL;
    g_n_extra_mod_dirs = 0;
    g_cap_extra_mod_dirs = 0;
    if (!g_hxc_self) hx_calc_self(s);
    for (int i = 0; i < o->n_include; i++) hx_busqueda_add(s, o->include[i]);
    {
        const char *env = getenv("HX_LIB");
        if (env && *env) {
            char *copia = hx_arena_strdup(&s->arena, env);
            for (char *p = copia;;) {
                char *sep = p;
                while (*sep && *sep != ':' && *sep != ';') sep++;
                int fin = *sep == 0;
                if (!fin) *sep = 0;
                if (*p) hx_busqueda_add(s, p);
                if (fin) break;
                p = sep + 1;
            }
        }
    }
    for (int i = 0; i < o->n_mod_dirs; i++) hx_busqueda_add(s, o->mod_dirs[i]);
    if (g_hxc_self) {
        char *self_dir = hx_path_dirname(&s->arena, g_hxc_self);
        hx_busqueda_add(s, hx_arena_sprintf(&s->arena, "%s/lib", self_dir));
        hx_busqueda_add(s, hx_arena_sprintf(&s->arena, "%s/../lib/hixean", self_dir));
        hx_busqueda_add(s, hx_arena_sprintf(&s->arena, "%s/../lib", self_dir));
    }
    if (o->verbose)
        for (int i = 0; i < g_n_extra_mod_dirs; i++)
            fprintf(stderr, "hx:   busqueda %s\n", g_extra_mod_dirs[i]);
}

static void hx_collect_modules2(HxSession *s, const char *entry_path,
                                           const char *use_hxc) {

    (void)0;
    HX_VEC(pending, const char *);
    /* el nombre que pedia el IMPORT de cada archivo pendiente y donde estaba
       escrito: sin esto se carga cualquier hxs que coincida con el nombre
       corto y no se dice nada */
    HX_VEC(esperado, const char *);
    HX_VEC(pedido_span, HxSpan);
    /* el fuente del módulo que hizo el IMPORT, para señalar en su sitio */
    HX_VEC(pedido_src, const char *);
    HX_VEC(pedido_file, const char *);
    HX_VEC(done, const char *);
    char *dir = hx_path_dirname(&s->arena, entry_path);
    HX_VEC_PUSH(pending, entry_path);
    HX_VEC_PUSH(esperado, NULL);
    HX_VEC_PUSH(pedido_span, ((HxSpan){0, 0}));
    HX_VEC_PUSH(pedido_src, NULL);
    HX_VEC_PUSH(pedido_file, NULL);
    HX_VEC_PUSH(done, entry_path);
    (void)dir;
    int entry_loaded = 0;

    while (pending.len) {
        const char *path = pending.data[0];
        const char *pedido = esperado.data[0];
        HxSpan pedido_en = pedido_span.data[0];
        const char *src_del_import = pedido_src.data[0];
        const char *file_del_import = pedido_file.data[0];
        int is_entry = path == entry_path;
        memmove(pending.data, pending.data + 1,
                (pending.len - 1) * sizeof(pending.data[0]));
        pending.len--;
        memmove(esperado.data, esperado.data + 1,
                (esperado.len - 1) * sizeof(esperado.data[0]));
        esperado.len--;
        memmove(pedido_span.data, pedido_span.data + 1,
                (pedido_span.len - 1) * sizeof(pedido_span.data[0]));
        pedido_span.len--;
        memmove(pedido_src.data, pedido_src.data + 1,
                (pedido_src.len - 1) * sizeof(pedido_src.data[0]));
        pedido_src.len--;
        memmove(pedido_file.data, pedido_file.data + 1,
                (pedido_file.len - 1) * sizeof(pedido_file.data[0]));
        pedido_file.len--;
        HxModule *m = hx_load_module(s, path, is_entry);
        if (!m) continue;
        if (pedido && hx_ascii_casecmp(hx_sym_str(m->name), pedido)) {
            /* el error es del IMPORT, no del módulo: el archivo que seImprime
               tiene que ser el que lo escribio, no el que se acaba de cargar */
            const char *src_previo = s->diags.ctx_src;
            const char *file_previo = s->diags.ctx_file;
            s->diags.ctx_src = src_del_import;
            s->diags.ctx_file = file_del_import;
            hx_diag_note(&s->diags, pedido_en, "E0501",
                         hx_arena_sprintf(&s->arena,
                                          "se pidió el módulo '%s' y '%s' declara '%s'",
                                          pedido, path, hx_sym_str(m->name)),
                         "el nombre del módulo tiene que ser el de la ruta del IMPORT; "
                         "si el fichero no lleva MODULE, se toma del nombre del archivo",
                         NULL);
            s->diags.ctx_src = src_previo;
            s->diags.ctx_file = file_previo;
        }
        if (is_entry) entry_loaded = 1;
        for (int i = 0; i < m->imports.len; i++) {
            HxImport *im = &m->imports.data[i];
            const char *sp = hx_sym_str(im->path);
            const char *dot = strrchr(sp, '.');
            char *stem = dot ? hx_arena_strndup(&s->arena, sp, (size_t)(dot - sp))
                             : hx_arg(&s->arena, sp);
            /* primero todos los .hxs (el directorio de la entrada y los que
               aportan los paquetes), luego todos los .hxf */
            const char *encontrado = hx_modulo_en(&s->arena, dir, sp, ".hxs");
            for (int d = 0; !encontrado && d < g_n_extra_mod_dirs; d++)
                encontrado = hx_modulo_en(&s->arena, g_extra_mod_dirs[d], sp, ".hxs");
            int found = encontrado != NULL;
            if (found) {
                int seen = 0;
                for (int k = 0; k < done.len; k++)
                    if (!strcmp(done.data[k], encontrado)) seen = 1;
                if (!seen) {
                    HX_VEC_PUSH(done, encontrado);
                    HX_VEC_PUSH(pending, encontrado);
                    HX_VEC_PUSH(esperado, sp);
                    HX_VEC_PUSH(pedido_span, im->path_span);
                    HX_VEC_PUSH(pedido_src, m->src);
                    HX_VEC_PUSH(pedido_file, m->file);
                }
            } else {
                encontrado = hx_modulo_en(&s->arena, dir, sp, ".hxf");
                for (int d = 0; !encontrado && d < g_n_extra_mod_dirs; d++)
                    encontrado = hx_modulo_en(&s->arena, g_extra_mod_dirs[d], sp, ".hxf");
                found = encontrado != NULL;
                if (found) {
                    int seen = 0;
                    for (int k = 0; k < done.len; k++)
                        if (!strcmp(done.data[k], encontrado)) seen = 1;
                    if (!seen) {
                        HX_VEC_PUSH(done, encontrado);
                        HX_VEC_PUSH(pending, encontrado);
                        HX_VEC_PUSH(esperado, sp);
                        HX_VEC_PUSH(pedido_span, im->path_span);
                        HX_VEC_PUSH(pedido_src, m->src);
                        HX_VEC_PUSH(pedido_file, m->file);
                    }
                } else if (use_hxc) {
                    char *hxc = hx_arena_sprintf(&s->arena, "%s/%s.hxc", use_hxc, sp);
                    HxModule *um = hx_hxc_read(&s->arena, s->intern, &s->diags, hxc);
                    if (!um) {
                        /* hx_diag_note y no hx_error: hx_error es variadica y se
                           comia la ayuda sin imprimirla */
                        hx_diag_note(&s->diags, im->path_span, "E0501",
                                     hx_arena_sprintf(&s->arena,
                                                      "no hay fuente ni unidad .hxc para '%s'",
                                                      sp),
                                     "publica la biblioteca con --emit-hxc DIR", NULL);
                    } else {
                        HxModule *slot =
                            (HxModule *)hx_arena_calloc(&s->arena, sizeof(HxModule));
                        *slot = *um;
                        slot->from_hxc = 1;
                        slot->index = s->unit.modules.len;
                        HX_VEC_PUSH(s->unit.modules, *slot);
                        if (o_verbose) fprintf(stderr, "hx:   unidad %s (%s.hxc)\n",
                                                hx_sym_str(um->name), stem);
                    }
                } else {
                    hx_diag_note(&s->diags, im->path_span, "E0501",
                                 hx_arena_sprintf(&s->arena,
                                                  "no se encontró el módulo '%s' (busqué %s.hxs y "
                                                  "%s.hxf)",
                                                  sp, stem, stem),
                                 "las rutas de IMPORT son el directorio del archivo de entrada, "
                                 "los -I, lo que diga HX_LIB, los que aporta --kit y la "
                                 "biblioteca que vino con este hxc",
                                 NULL);
                }
            }
        }
    }
    (void)entry_loaded;
    for (int i = 0; i < s->unit.modules.len; i++) {
        HxModule *m = &s->unit.modules.data[i];
        for (int j = 0; j < m->funcs.len; j++) {
            /* dos OPERATOR + de tipos distintos no son sobrecarga de funciones:
               cada uno tiene su nombre cuando se comprueba la firma */
            if (m->funcs.data[j].is_operator) continue;
            for (int k = j + 1; k < m->funcs.len; k++) {
                if (m->funcs.data[k].is_operator) continue;
                if (m->funcs.data[j].name == m->funcs.data[k].name) {
                    hx_diag_note(&s->diags, m->funcs.data[k].name_span, "E0502",
                                 hx_arena_sprintf(&s->arena,
                                                  "'%s' está declarado más de una vez en '%s'",
                                                  hx_sym_str(m->funcs.data[k].name),
                                                  hx_sym_str(m->name)),
                                 "Hixean no admite sobrecarga de funciones", NULL);
                }
            }
        }
    }
}

typedef struct {
    const char *cfile;
    const char *obj;
    const char **argv; /* vector de argumentos propio cuando se compila en paralelo */
    pid_t pid;   /* 0 si no hay proceso en vuelo */
} HxTu;

static int hx_link_objects(HxSession *s, HxBuildOpts *o, HxTu *tus, int ntus,
                          const char *bin_path) {
    const char *argv[1024];
    int n = 0;
    argv[n++] = "cc";
    for (int i = 0; i < ntus; i++) argv[n++] = hx_arg(&s->arena, tus[i].obj);
    for (int i = 0; i < s->unit.modules.len; i++) {
        HxModule *m = &s->unit.modules.data[i];
        if (!m->from_hxc) continue;
        char *arc = hx_arena_sprintf(&s->arena, "%s/lib%s.a", s->hxc_dir, hx_sym_str(m->name));
        if (!hx_file_exists(arc)) {
            fprintf(stderr, "hx: falta la biblioteca %s (compila el modulo que exporta %s "
                            "con --emit-hxc DIR)\n",
                    arc, hx_sym_str(m->name));
            exit(1);
        }
        argv[n++] = arc;
    }
    argv[n++] = "-o";
    argv[n++] = hx_arg(&s->arena, bin_path);
    argv[n++] = "-Wl,--gc-sections";
    if (o->profile == HX_PROFILE_FREESTANDING) {
        argv[n++] = "-nostdlib";
        argv[n++] = "-static";
        argv[n++] = "-no-pie";
        argv[n++] = "-fno-pie";
    }
    argv[n++] = "-s";
    argv[n++] = NULL;
    double t = hx_now_ms();
    if (hx_run(argv) != 0) {
        fprintf(stderr, "hx: fallo el enlazado de %s\n", bin_path);
        exit(1);
    }
    if (o->verbose) fprintf(stderr, "hx:   link %-28s %6.1f ms\n", bin_path, hx_now_ms() - t);
    return 0;
}

static int hx_cc_argv(HxArena *a, const char **argv, const char *cc_file, const char *obj_file,
                      int optimize, HxProfile profile) {
    int n = 0;
    argv[n++] = "cc";
    argv[n++] = "-c";
    argv[n++] = hx_arg(a, cc_file);
    argv[n++] = "-o";
    argv[n++] = hx_arg(a, obj_file);
    argv[n++] = optimize > 1 ? "-O2" : optimize == 1 ? "-O1" : "-O0";
    if (optimize >= 1) argv[n++] = "-fomit-frame-pointer";
    argv[n++] = "-w";
    if (profile == HX_PROFILE_FREESTANDING) {
        argv[n++] = "-ffreestanding";
        argv[n++] = "-fno-builtin";
        argv[n++] = "-fno-stack-protector";
        argv[n++] = "-U_FORTIFY_SOURCE";
        argv[n++] = "-D_FORTIFY_SOURCE=0";
    }
    argv[n++] = "-std=c11";
    argv[n++] = "-ffunction-sections";
    argv[n++] = "-fdata-sections";
    argv[n++] = NULL;
    return n;
}


/* Espera el cc mas antiguo en vuelo y avisa si fallo. */
static int hx_flush_one(HxTu *tus, int ntus, int *inflight) {
    for (int j = 0; j < ntus; j++) {
        if (!tus[j].pid) continue;
        int rc = hx_wait(tus[j].pid);
        tus[j].pid = 0;
        (*inflight)--;
        if (rc != 0) {
            fprintf(stderr, "hx: fallo cc al compilar %s\n", tus[j].cfile);
            return 1;
        }
        return 0;
    }
    return 0;
}

static int hx_compile_tu(HxArena *a, const char *cc_file, const char *obj_file, int optimize,
                         HxProfile profile, int verbose) {
    const char *argv[32];
    hx_cc_argv(a, argv, cc_file, obj_file, optimize, profile);
    double t = hx_now_ms();
    if (hx_run(argv) != 0) {
        fprintf(stderr, "hx: fallo cc al compilar %s\n", cc_file);
        return 1;
    }
    if (verbose) fprintf(stderr, "hx:   cc %-28s %6.1f ms\n", cc_file, hx_now_ms() - t);
    return 0;
}

static int hx_build_main(HxSession *s, const char *entry, HxBuildOpts *o, const char *bin_path,
                         double *out_ms) {
    double t0 = hx_now_ms();
    if (!hx_file_exists(entry)) {
        fprintf(stderr, "hx: no existe el archivo de entrada %s\n", entry);
        return 1;
    }
    o_verbose = o->verbose;
    s->hxc_dir = o->use_hxc;
    hx_collect_modules2(s, entry, o->use_hxc);
    hx_check_unit(&s->unit);
    if (s->diags.errors) return 1;


    char *gen_dir = hx_arena_strdup(&s->arena, "build/gen");
    char *obj_dir = hx_arena_strdup(&s->arena, "build/obj");
    hx_mkdir_p("build");
    hx_mkdir_p(gen_dir);
    hx_mkdir_p(obj_dir);
    char *rt_path = hx_arena_sprintf(&s->arena, "%s/_runtime.h", gen_dir);

    if (o->emit_hxc) {
        hx_mkdir_p(o->emit_hxc);
        for (int i = 0; i < s->unit.modules.len; i++) {
            HxModule *m = &s->unit.modules.data[i];
            char *hp = hx_arena_sprintf(&s->arena, "%s/%s.hxc", o->emit_hxc,
                                        hx_sym_str(m->name));
            if (hx_hxc_write(&s->arena, &s->unit, m, hp) != 0) return 1;
            if (o->verbose) fprintf(stderr, "hx:   .hxc %s\n", hp);
        }
    }

    HxEmitOptions eo;
    memset(&eo, 0, sizeof(eo));
    eo.profile = o->profile;
    eo.dir_gen = gen_dir;
    eo.dir_runtime = rt_path;
    eo.keep_asm = o->keep_asm;
    eo.keep_asm_path = o->keep_asm_path;
    if (hx_emit_unit(&s->arena, &s->unit, &eo) != 0) return 1;
    double t_emit = hx_now_ms();

    HX_VEC(tus, HxTu);
    HX_VEC(libdirs, const char *);
    for (int i = 0; i < s->unit.modules.len; i++) {
        HxModule *m = &s->unit.modules.data[i];
        if (m->from_hxc) {
            HX_VEC_PUSH(libdirs, hx_sym_str(m->name));
            continue;
        }
        if (m->is_entry) continue;
        char *cf = hx_arena_sprintf(&s->arena, "%s/%s.c", gen_dir, hx_sym_str(m->name));
        HxTu tu = {cf, NULL, NULL, 0};
        HX_VEC_PUSH(tus, tu);
    }
    int entry_tu = -1;
    {
        char *cf = hx_arena_sprintf(&s->arena, "%s/_entry.c", gen_dir);
        HxTu tu = {cf, NULL, NULL, 0};
        entry_tu = tus.len;
        HX_VEC_PUSH(tus, tu);
    }
    if (o->profile == HX_PROFILE_FREESTANDING) {
        char *cf = hx_arena_sprintf(&s->arena, "%s/_rtmem.c", gen_dir);
        HxTu tu = {cf, NULL, NULL, 0};
        HX_VEC_PUSH(tus, tu);
    }

    HxTu *tu_entry = &tus.data[entry_tu];

    int rebuilt = 0;
    int jobs = 1;
#ifdef _WIN32
    jobs = 1; /* sin waitpid: se compila en serie */
#else
    /* una compilacion por nucleo; cc ya usa varios hilos por dentro.
       _SC_NPROCESSORS_ONLN es de Linux y en macOS no existe, asi que alli el
       numero se pide por sysctl y, si tampoco se puede, se compila en serie:
       fewer es mejor que un numero inventado. */
    if (o->jobs > 0) {
        jobs = o->jobs;
    } else {
        long ncpu = 0;
#ifdef _SC_NPROCESSORS_ONLN
        ncpu = sysconf(_SC_NPROCESSORS_ONLN);
#endif
        jobs = (int)(ncpu > 0 ? ncpu : 2);
    }
    if (jobs > 8) jobs = 8;
    if (jobs > tus.len) jobs = tus.len;
    if (jobs < 1) jobs = 1;
#endif
    int inflight = 0;
    for (int i = 0; i < tus.len; i++) {
        HxTu *tu = &tus.data[i];
        HxHash hx;
        hx_fnv_init(&hx);
        hx_fnv_str(&hx, HX_VERSION);
        if (!g_hxc_build_id) {
            hx_calc_self(s);
            hx_calc_build_id(s);
        }
        hx_fnv_str(&hx, g_hxc_build_id);
        hx_fnv_u64(&hx, (uint64_t)o->profile);
        hx_fnv_u64(&hx, (uint64_t)o->optimize);
        size_t n = 0;
        char *rt = hx_read_file(&s->arena, rt_path, &n);
        if (rt) hx_fnv_bytes(&hx, rt, n);
        char *src = hx_read_file(&s->arena, tu->cfile, &n);
        if (src) hx_fnv_bytes(&hx, src, n);
        for (int j = 0; j < s->unit.modules.len; j++) {
            HxModule *m = &s->unit.modules.data[j];
            for (int k = 0; k < m->imports.len; k++) {
                const char *ip = hx_sym_str(m->imports.data[k].path);
                const char *dot = strrchr(ip, '.');
                const char *base = dot ? dot + 1 : ip;
                HxSym as = m->imports.data[k].alias
                               ? m->imports.data[k].alias
                               : hx_intern_cstr(s->intern, base);
                char *hp = hx_arena_sprintf(&s->arena, "%s/%s.h", gen_dir, hx_sym_str(as));
                char *hd = hx_read_file(&s->arena, hp, &n);
                if (hd) hx_fnv_bytes(&hx, hd, n);
            }
        }
        char hex[20];
        hx_fnv_hex(&hx, hex, sizeof(hex));
        tu->obj = hx_arena_sprintf(&s->arena, "%s/%s.o", obj_dir, hex);
        if (!hx_file_exists(tu->obj)) {
            if (jobs > 1) {
                /* cada unidad guarda su argv en memoria propia: hx_cc_argv
                   escribe punteros, no se puede compartir el mismo vector */
                tu->argv = (const char **)hx_arena_calloc(&s->arena, sizeof(char *) * 32);
                hx_cc_argv(&s->arena, tu->argv, tu->cfile, tu->obj, o->optimize, o->profile);
                pid_t pid = hx_spawn(tu->argv);
                if (pid < 0) {
                    if (hx_compile_tu(&s->arena, tu->cfile, tu->obj, o->optimize, o->profile,
                                              o->verbose) != 0)
                        return 1;
                } else {
                    tu->pid = pid;
                    inflight++;
                    rebuilt++;
                    /* no lanzar mas hasta que haya hueco */
                    while (inflight >= jobs) {
                        int bad = hx_flush_one(tus.data, tus.len, &inflight);
                        if (bad) return 1;
                    }
                    continue;
                }
            } else {
                if (hx_compile_tu(&s->arena, tu->cfile, tu->obj, o->optimize, o->profile,
                                              o->verbose) != 0)
                    return 1;
            }
            rebuilt++;
        }
    }
    while (inflight > 0)
        if (hx_flush_one(tus.data, tus.len, &inflight)) return 1;
    double t_objs = hx_now_ms();

    if (o->emit_hxc) {
        for (int i = 0; i < s->unit.modules.len; i++) {
            HxModule *m = &s->unit.modules.data[i];
            const char *stem = hx_sym_str(m->name);
            const char *srcobj = NULL;
            if (m->is_entry) srcobj = tu_entry->obj;
            else
                for (int j = 0; j + 1 < tus.len; j++) {
                    char *bn = hx_path_basename(&s->arena, tus.data[j].cfile);
                    size_t sl = strlen(stem);
                    if (!strncmp(bn, stem, sl) && bn[sl] == '.') {
                        srcobj = tus.data[j].obj;
                        break;
                    }
                }
            if (!srcobj || !hx_file_exists(srcobj)) continue;
            char *arc = hx_arena_sprintf(&s->arena, "%s/lib%s.a", o->emit_hxc, stem);
            if (!srcobj || !hx_file_exists(srcobj)) continue;
            if (m->is_entry) {
                char *pub = hx_arena_sprintf(&s->arena, "%s/%s.pub.o", o->emit_hxc, stem);
                char *cp = hx_arena_sprintf(&s->arena, "cp \"%s\" \"%s\"", srcobj, pub);
                if (system(cp) != 0) return 1;
                char *pre = hx_arena_sprintf(
                    &s->arena,
                    "objcopy --redefine-sym _start=__hxlib_start"
                    " --redefine-sym main=__hxlib_main"
                    " --redefine-sym hx_main=__hxlib_hx_main"
                    " --redefine-sym hx_static_init=__hxlib_static_init"
                    " --redefine-sym hx_static_arena=__hxlib_static_arena \"%s\"",
                    pub);
                if (system(pre) != 0) {
                    fprintf(stderr, "hx: no pude preparar la biblioteca %s\n", arc);
                    return 1;
                }
                srcobj = pub;
            }
            char *cc = hx_arena_sprintf(&s->arena, "ar rcs \"%s\" \"%s\"", arc, srcobj);
            if (system(cc) != 0) {
                fprintf(stderr, "hx: no pude archivar %s\n", arc);
                return 1;
            }
            if (o->verbose) fprintf(stderr, "hx:   lib  %s\n", arc);
        }
    }

    if (hx_link_objects(s, o, tus.data, tus.len, bin_path) != 0) return 1;
    double t_end = hx_now_ms();
    if (o->verbose) {
        int sz = hx_file_size(bin_path);
        fprintf(stderr, "hx: front-end+emit %.1f ms | cc %.1f ms (%d TU recompiladas) | link %.1f ms"
                        " | %d bytes\n",
                t_emit - t0, t_objs - t_emit, rebuilt, t_end - t_objs, sz);
    }
    if (out_ms) *out_ms = t_end - t0;
    return 0;
}

static int hx_exec(const char *bin, const char *out_path) {
#ifdef _WIN32
    /* Ejecutar y mandar stdout y stderr a un fichero. La via corta era system() con
       una redireccion, pero el shell de Windows se traga el codigo de salida y no
       hay forma de saber si el programa fallo: `salida > fichero 2>&1` deja el
       codigo de cmd, no el del programa, asi que hxc test comparaba un fichero
       vacio y daba FALLO en todo el corpus. Con CreateProcess el codigo de salida
       es el del proceso, y la redireccion va en STARTUPINFO. */
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE salida = CreateFileA(out_path, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (salida == INVALID_HANDLE_VALUE) return 1;
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = salida;
    si.hStdError = salida;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    char linea[2048];
    snprintf(linea, sizeof(linea), "\"%s\"", bin);
    if (!CreateProcessA(NULL, linea, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(salida);
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(salida);
    return (int)rc;
#else
    const char *argv[3];
    argv[0] = bin;
    argv[1] = NULL;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (!freopen(out_path, "w", stdout)) _exit(127);
        if (!freopen(out_path, "a", stderr)) _exit(127);
        hx_execvp(argv);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

/* --json decide como se imprimen los diagnosticos en todos los comandos */
static int g_json = 0;

/* Solo para los valores por defecto de install, que se calculan antes de tener
   arena propia; no sobrevive a main y no se usa para diagnostics. */
static char *hx_arg(HxArena *a, const char *s) { return hx_arena_strdup(a, s ? s : ""); }

static void hx_render(HxDiagBag *d, const char *src) {
    if (g_json)
        hx_diag_render_json(d, src, stderr);
    else
        hx_diag_render(d, src, stderr);
}

/* Publicar un paquete es copiar su manifiesto y los modulos que importa al
   layout que ya entienden hxc query, hxc kit y hxc install:
   <registro>/<nombre>/<nombre>.hxk mas los fuentes, todos con nombre plano. */
static int hx_cmd_pack(const char *manifest, const char *out) {
    HxSession *s = (HxSession *)malloc(sizeof(HxSession));
    hx_session_init(s);
    HxPathList paths;
    memset(&paths, 0, sizeof(paths));
    paths.data[paths.len++] = ".";
    HxKit kit;
    memset(&kit, 0, sizeof(kit));
    kit.arena = &s->arena;
    if (!hx_kit_parse(&kit, manifest, &s->diags)) {
        hx_render(&s->diags, NULL);
        return 1;
    }
    const char *nombre = hx_sym_str(kit.name);
    char *destino = hx_arena_sprintf(&s->arena, "%s/%s", out, nombre);
    s->diags.ctx_file = manifest;
    if (hx_path_exists(destino)) {
        hx_error(&s->diags, (HxSpan){0, 0}, "E0815",
                 "el registro ya tiene '%s': borra %s o publica otra version", nombre, destino);
        hx_render(&s->diags, NULL);
        return 1;
    }
    char *entry = hx_kit_entry_path(&s->arena, &kit, manifest);
    if (!hx_file_exists(entry)) {
        hx_error(&s->diags, (HxSpan){0, 0}, "E0819",
                 "el ENTRY %s no existe; un paquete sin su fuente no se puede publicar", entry);
        hx_render(&s->diags, NULL);
        return 1;
    }
    hx_collect_modules2(s, entry, NULL);
    hx_mkdir_p(destino);
    int n = 0;
    /* el ENTRY se copia siempre: una biblioteca (.hxs) no entra en la unidad
       que construye hxc, y un paquete sin su fuente no sirve para nada */
    for (int i = 0; i < s->unit.modules.len; i++) {
        HxModule *m = &s->unit.modules.data[i];
        if (!m->file) continue;
        size_t len = 0;
        char *data = hx_read_file(&s->arena, m->file, &len);
        if (!data) {
            hx_error(&s->diags, (HxSpan){0, 0}, "E0816", "no se pudo leer el modulo %s", m->file);
            hx_render(&s->diags, NULL);
            return 1;
        }
        char *dest = hx_arena_sprintf(&s->arena, "%s/%s", destino, hx_path_basename(&s->arena, m->file));
        if (hx_write_file(dest, data, len) != 0) {
            hx_error(&s->diags, (HxSpan){0, 0}, "E0816", "no se pudo escribir %s", dest);
            hx_render(&s->diags, NULL);
            return 1;
        }
        n++;
    }
    char *entrada_dest = hx_arena_sprintf(&s->arena, "%s/%s", destino,
                                          hx_path_basename(&s->arena, entry));
    if (!hx_file_exists(entrada_dest)) {
        size_t elen = 0;
        char *edata = hx_read_file(&s->arena, entry, &elen);
        if (!edata || hx_write_file(entrada_dest, edata, elen) != 0) {
            hx_error(&s->diags, (HxSpan){0, 0}, "E0816", "no se pudo escribir %s", entrada_dest);
            hx_render(&s->diags, NULL);
            return 1;
        }
        n++;
    }
    size_t mlen = 0;
    char *mtext = hx_read_file(&s->arena, manifest, &mlen);
    char *mkit = hx_arena_sprintf(&s->arena, "%s/%s.hxk", destino, nombre);
    if (!mtext || hx_write_file(mkit, mtext, mlen) != 0) {
        hx_error(&s->diags, (HxSpan){0, 0}, "E0816", "no se pudo escribir %s", mkit);
        hx_render(&s->diags, NULL);
        return 1;
    }
    printf("%s %s  ->  %s (%d modulos)\n", nombre, kit.version, mkit, n);
    return 0;
}

static void hx_usage(void) {
    fprintf(stderr,
            "hxc %s\n"
            "uso:\n"
            "  hxc run <archivo.hxe> [--freestanding|--libc] [--timing] [--keep-c]\n"
            "  hxc build <archivo.hxe> [-o salida] [--emit-only] [--keep-c]\n"
            "  hxc build <archivo.hxe> --emit-hxc DIR   (escribe una unidad .hxc por modulo)\n"
            "  hxc build <archivo.hxe> --use-hxc DIR     (compila contra interfaces .hxc)\n"
            "  hxc build --kit <archivo.hxk>   (construye el paquete)\n"
            "  hxc kit    <archivo.hxk> [--path DIR]  (resuelve dependencias)\n"
            "  hxc query  <archivo.hxq> [--path DIR]  (busca paquetes por sus PROVIDES)\n"
            "  hxc pack   <archivo.hxk> [--out DIR]    publica un paquete en un registro\n"
            "  hxc install <nombre> [--registry DIR] [--into DIR]\n"
            "  hxc check <archivo.hxe> [--json]\n"
            "  hxc size <binario>\n"
            "  hxc version\n", HX_VERSION);
}

static const char *kit_gates[HX_KIT_MAX];
static int kit_gates_n;
static int kit_gates_set;

int main(int argc, char **argv) {
    if (argc > 0 && argv[0]) g_argv0 = argv[0];
    hx_arena_init(&g_arena_scratch);
    if (argc < 2) {
        hx_usage();
        return 2;
    }
    const char *cmd = argv[1];
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--json")) g_json = 1;
    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version") || !strcmp(cmd, "-V")) {
        if (g_json)
            printf("{\"name\": \"hixean\", \"compiler\": \"hxc\", \"version\": \"%s\","
                   " \"profile_default\": \"freestanding\", \"size_gate\": 12288}\n",
                   HX_VERSION);
        else
            printf("hxc %s\n", HX_VERSION);
        return 0;
    }
    if (!strcmp(cmd, "size")) {
        if (argc < 3) {
            hx_usage();
            return 2;
        }
        int sz = hx_file_size(argv[2]);
        if (sz < 0) {
            fprintf(stderr, "hx: no existe %s\n", argv[2]);
            return 1;
        }
        printf("%s %d bytes (%.2f KiB)\n", argv[2], sz, (double)sz / 1024.0);
        return 0;
    }
    if (!strcmp(cmd, "query")) {
        const char *file = NULL;
        HxPathList paths;
        memset(&paths, 0, sizeof(paths));
        paths.data[paths.len++] = ".";
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--path") && i + 1 < argc)
                paths.data[paths.len++] = argv[++i];
            else if (!argv[i][0] || argv[i][0] == '-')
                continue;
            else if (!file)
                file = argv[i];
        }
        if (!file) {
            hx_usage();
            return 2;
        }
        HxArena arena;
        hx_arena_init(&arena);
        HxDiagBag diags;
        hx_diag_init(&diags, &arena);
        HxQuery q;
        q.arena = &arena;
        if (!hx_query_parse(&arena, file, &diags, &q)) {
            hx_render(&diags, NULL);
            return 1;
        }
        if (hx_query_run(&q, &paths, &diags) < 0) return 1;
        return 0;
    }
    if (!strcmp(cmd, "pack")) {
        if (argc < 3) {
            hx_usage();
            return 2;
        }
        const char *out = "registro";
        const char *manifest = NULL;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
            else if (argv[i][0] != '-') manifest = argv[i];
        }
        if (!manifest) {
            hx_usage();
            return 2;
        }
        return hx_cmd_pack(manifest, out);
    }
    if (!strcmp(cmd, "install")) {
        if (argc < 3) {
            hx_usage();
            return 2;
        }
        const char *name = argv[2];
        const char *registry = NULL;
        const char *into = NULL;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--registry") && i + 1 < argc) registry = argv[++i];
            else if (!strcmp(argv[i], "--into") && i + 1 < argc) into = argv[++i];
        }
        if (!registry) {
            const char *home = getenv("HOME");
            char *defecto = hx_arena_sprintf(&g_arena_scratch, "%s/.hixean/registro",
                                             home ? home : ".");
            registry = defecto;
        }
        if (!into) {
            const char *home = getenv("HOME");
            into = hx_arena_sprintf(&g_arena_scratch, "%s/.hixean/paquetes",
                                    home ? home : ".");
        }
        HxArena arena;
        hx_arena_init(&arena);
        HxDiagBag diags;
        hx_diag_init(&diags, &arena);
        char donde[1024];
        if (!hx_kit_install(&arena, name, registry, into, &diags, donde, sizeof(donde))) {
            hx_render(&diags, NULL);
            return 1;
        }
        printf("%s instalado en %s\n", name, donde);
        printf("añade esa ruta a tus comandos:\n  --path %s\n", into);
        return 0;
    }
    if (!strcmp(cmd, "kit")) {
        const char *file = NULL;
        HxPathList paths;
        memset(&paths, 0, sizeof(paths));
        paths.data[paths.len++] = ".";
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--path") && i + 1 < argc)
                paths.data[paths.len++] = argv[++i];
            else if (!argv[i][0])
                continue;
            else if (!file)
                file = argv[i];
        }
        if (!file) {
            hx_usage();
            return 2;
        }
        HxArena arena;
        hx_arena_init(&arena);
        HxDiagBag diags;
        hx_diag_init(&diags, &arena);
        HxKit kit;
        if (!hx_kit_resolve(&arena, file, &paths, &diags, &kit)) {
            hx_render(&diags, NULL);
            return 1;
        }
        printf("%s %s\n", kit.name, kit.version);
        printf("  entry    %s\n", kit.entry);
        for (int i = 0; i < kit.deps.len; i++)
            printf("  %-8s %s %s\n",
                   kit.deps.data[i].is_require ? "require" : "dep",
                   kit.deps.data[i].name, kit.deps.data[i].version);
        for (int i = 0; i < kit.n_features; i++) printf("  feature  %s\n", kit.features[i]);
        for (int i = 0; i < kit.n_kits; i++)
            printf("  resolution %s %s\n", kit.kits[i]->name, kit.kits[i]->version);
        return 0;
    }
    if (!strcmp(cmd, "test")) {
        int pass = 0, fail = 0;
        for (int i = 2; i < argc; i++) {
            const char *arg = argv[i];
            if (arg[0] == '-') continue;
            HxSession *ts = (HxSession *)malloc(sizeof(HxSession));
            hx_session_init(ts);
            char *stem = hx_path_stem(&ts->arena, arg);
            char *bin = hx_arena_sprintf(&ts->arena, "build/%s", stem);
            char *actual = hx_arena_sprintf(&ts->arena, "build/%s.out", stem);
            char *expected = hx_arena_sprintf(&ts->arena, "%s.out", arg);
            HxBuildOpts to;
            memset(&to, 0, sizeof(to));
            to.optimize = 2;
            to.profile = HX_PROFILE_FREESTANDING;
            /* hxc test tambien necesita la ruta de la biblioteca: un programa
               del corpus puede importar un modulo de lib/hixean */
            hx_preparar_busqueda(ts, &to);
            double ms = 0;
            if (hx_build_main(ts, arg, &to, bin, &ms) != 0) {
                hx_render(&ts->diags, NULL);
                fail++;
                continue;
            }
            int rc = hx_exec(bin, actual);
            size_t alen = 0, elen = 0;
            char *adata = hx_read_file(&ts->arena, actual, &alen);
            char *edata = hx_read_file(&ts->arena, expected, &elen);
            if (!edata) {
                FILE *f = fopen(expected, "wb");
                if (f && adata) fwrite(adata, 1, alen, f);
                if (f) fclose(f);
                printf("  nuevo  %s (esperado escrito en %s, rc=%d)\n", arg, expected, rc);
                pass++;
                continue;
            }
            if (alen == elen && adata && memcmp(adata, edata, alen) == 0) {
                printf("  ok     %s\n", arg);
                pass++;
            } else {
                printf("  FALLO  %s (rc=%d)\n", arg, rc);
                printf("    --- esperado ---\n%s    --- obtenido ---\n%s", edata,
                       adata ? adata : "");
                fail++;
            }
        }
        printf("%d pruebas, %d fallos\n", pass + fail, fail);
        return fail ? 1 : 0;
    }
    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        hx_usage();
        return 0;
    }

    const char *entry = NULL;
    const char *out = NULL;
    HxBuildOpts o;
    memset(&o, 0, sizeof(o));
    o.optimize = 2;
    o.profile = HX_PROFILE_FREESTANDING;
    int emit_only = 0;
    int run_mode = 0;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(a, "--libc")) o.profile = HX_PROFILE_LIBC;
        else if (!strcmp(a, "--freestanding")) o.profile = HX_PROFILE_FREESTANDING;
        else if (!strcmp(a, "--emit-only")) emit_only = 1;
        else if (!strcmp(a, "--keep-c")) {
            o.keep_asm = 1;
            o.keep_asm_path = "build/generated.c";
        } else if (!strcmp(a, "--timing")) o.verbose = 1;
        else if (!strcmp(a, "--emit-hxc") && i + 1 < argc) o.emit_hxc = argv[++i];
        else if (!strcmp(a, "--use-hxc") && i + 1 < argc) o.use_hxc = argv[++i];
        else if (!strcmp(a, "--jobs") && i + 1 < argc) o.jobs = atoi(argv[++i]);
        else if (!strcmp(a, "--kit") && i + 1 < argc) { o.kit_file = argv[++i]; if (!entry) entry = ""; }
        else if (!strcmp(a, "--path") && i + 1 < argc) ++i; /* se lee antes */
        else if (!strcmp(a, "-I") && i + 1 < argc) {
            if (o.n_include >= HX_KIT_MAX) {
                fprintf(stderr, "hx: como mucho %d directorios con -I\n", HX_KIT_MAX);
                return 2;
            }
            o.include[o.n_include++] = argv[++i];
        }
        else if (!strcmp(a, "--json")) g_json = 1;
        else if (a[0] == '-') {
            fprintf(stderr, "hx: opción desconocida '%s'\n", a);
            return 2;
        } else if (!entry) entry = a;
    }
    o.emit_only = emit_only;
    run_mode = !strcmp(cmd, "run");
    if (!strcmp(cmd, "run")) run_mode = 1;
    else if (!strcmp(cmd, "build")) run_mode = 0;
    else if (!strcmp(cmd, "check")) run_mode = 0;
    else {
        fprintf(stderr, "hx: comando desconocido '%s'\n", cmd);
        hx_usage();
        return 2;
    }
    HxSession *s = (HxSession *)malloc(sizeof(HxSession));
    hx_session_init(s);
    HxPathList kitpaths;
    memset(&kitpaths, 0, sizeof(kitpaths));
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--path") && i + 1 < argc) kitpaths.data[kitpaths.len++] = argv[i + 1];
    if (!kitpaths.len) kitpaths.data[kitpaths.len++] = ".";
    if (o.kit_file) {
        /* el manifiesto decide el punto de entrada, el perfil y los DEFINE */
        HxKit kit;
        if (!hx_kit_resolve(&s->arena, o.kit_file, &kitpaths, &s->diags, &kit)) {
            hx_render(&s->diags, NULL);
            return 1;
        }
        if (o.verbose) {
            fprintf(stderr, "hx: kit %s %s\n", kit.name, kit.version);
            for (int i = 0; i < kit.n_defines; i++)
                fprintf(stderr, "hx:   define %s\n", kit.defines[i]);
        }
        entry = hx_kit_entry_path(&s->arena, &kit, o.kit_file);
        if (!hx_file_exists(entry)) {
            fprintf(stderr, "hx: el manifiesto %s apunta a %s y no existe\n", o.kit_file, entry);
            return 1;
        }
        if (kit.profile && !hx_ascii_casecmp(kit.profile, "libc")) o.profile = HX_PROFILE_LIBC;
        for (int i = 0; i < kit.n_caps && kit_gates_n < HX_KIT_MAX; i++)
            kit_gates[kit_gates_n++] = kit.caps[i];
        kit_gates_set = 1;
        /* cada dependencia resuelta aporta su directorio a la busqueda */
        for (int i = 0; i < kit.n_kits && i < HX_KIT_MAX; i++) {
            const char *dir = kitpaths.data[0];
            if (kit.kits[i]->file) {
                char *d = hx_arena_strdup(&s->arena, kit.kits[i]->file);
                char *slash = strrchr(d, '/');
                if (slash) {
                    *slash = 0;
                    o.mod_dirs[o.n_mod_dirs++] = d;
                }
            }
            (void)dir;
        }
        for (int i = 0; i < kit.n_defines && o.n_defines < HX_KIT_MAX; i++)
            o.defines[o.n_defines++] = kit.defines[i];
        for (int i = 0; i < kit.n_opts && o.n_opts < HX_KIT_MAX; i++)
            o.extra_opts[o.n_opts++] = kit.opts[i];
    }
    if (!entry || !*entry) {
        hx_usage();
        return 2;
    }



    const char *base = hx_path_basename(&s->arena, entry);
    if (!strcmp(base, "build")) {
        char *rel = hx_path_join(&s->arena, ".", "examples/hola.hxe");
        entry = rel;
    }

    char *bin_path;
    if (run_mode && !out) {
        char *stem = hx_path_stem(&s->arena, entry);
        bin_path = hx_arena_sprintf(&s->arena, "build/%s", stem);
    } else if (out) {
        bin_path = hx_arena_strdup(&s->arena, out);
    } else {
        char *stem = hx_path_stem(&s->arena, entry);
        bin_path = hx_arena_sprintf(&s->arena, "build/%s.bin", stem);
    }

    hx_preparar_busqueda(s, &o);
    double ms = 0;
    if (hx_build_main(s, entry, &o, bin_path, &ms) != 0) {
        const char *src = NULL;
        for (int i = 0; i < s->unit.modules.len; i++) src = s->unit.modules.data[i].src;
        hx_render(&s->diags, src ? src : "");
        return 1;
    }
    hx_render(&s->diags, NULL);

    /* un paquete debe declarar cada capacidad que usen sus fuentes */
    if (o.kit_file && kit_gates_set) {
        for (int i = 0; i < s->unit.n_caps_used; i++) {
            const char *usada = s->unit.caps_used[i];
            int declarada = 0;
            for (int j = 0; j < kit_gates_n; j++)
                if (!hx_ascii_casecmp(kit_gates[j], usada)) declarada = 1;
            if (!declarada) {
                fprintf(stderr,
                        "hx: %s usa la capacidad '%s' pero el manifiesto no la declara con "
                        "CAPABILITY %s\n",
                        o.kit_file, usada, usada);
                return 1;
            }
        }
    }

    if (emit_only) return 0;

    if (run_mode) {
        const char *rargv[3];
        rargv[0] = hx_arg(&s->arena, bin_path);
        rargv[1] = NULL;
        int rc = hx_run(rargv);
        return rc < 0 ? 1 : rc;
    }
    return 0;
}