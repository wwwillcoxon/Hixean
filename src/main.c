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

#ifdef _WIN32
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

static int hx_run(char **argv) {
#ifdef _WIN32
    intptr_t r = _spawnvp(_P_WAIT, argv[0], argv);
    return (int)r;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

/* Lanza cc sin esperar: permite compilar varias unidades a la vez. */
static pid_t hx_spawn(char **argv) {
#ifdef _WIN32
    return 0;
#else
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    return pid;
#endif
}

static int hx_wait(pid_t pid) {
    if (pid <= 0) return 0;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

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

static const char **g_extra_mod_dirs;
static int g_n_extra_mod_dirs;

static void hx_collect_modules2(HxSession *s, const char *entry_path,
                                           const char *use_hxc) {

    HX_VEC(pending, char *);
    HX_VEC(done, const char *);
    char *dir = hx_path_dirname(&s->arena, entry_path);
    HX_VEC_PUSH(pending, entry_path);
    HX_VEC_PUSH(done, entry_path);
    (void)dir;
    int entry_loaded = 0;

    while (pending.len) {
        char *path = pending.data[0];
        int is_entry = path == entry_path;
        memmove(pending.data, pending.data + 1, (pending.len - 1) * sizeof(char *));
        pending.len--;
        HxModule *m = hx_load_module(s, path, is_entry);
        if (!m) continue;
        if (is_entry) entry_loaded = 1;
        for (int i = 0; i < m->imports.len; i++) {
            HxImport *im = &m->imports.data[i];
            const char *sp = hx_sym_str(im->path);
            const char *dot = strrchr(sp, '.');
            char *stem = dot ? hx_arena_strndup(&s->arena, sp, (size_t)(dot - sp)) : (char *)sp;
            char *cand = hx_arena_sprintf(&s->arena, "%s/%s.hxs", dir, stem);
            int found = hx_file_exists(cand);
            char *encontrado = found ? cand : NULL;
            for (int d = 0; !found && d < g_n_extra_mod_dirs; d++) {
                /* los paquetes del manifiesto aportan sus modulos */
                char *c2 = hx_arena_sprintf(&s->arena, "%s/%s.hxs", g_extra_mod_dirs[d], stem);
                if (hx_file_exists(c2)) {
                    encontrado = c2;
                    found = 1;
                }
            }
            if (found) {
                int seen = 0;
                for (int k = 0; k < done.len; k++)
                    if (!strcmp(done.data[k], encontrado)) seen = 1;
                if (!seen) {
                    HX_VEC_PUSH(done, encontrado);
                    HX_VEC_PUSH(pending, encontrado);
                }
            } else {
                char *cand2 = hx_arena_sprintf(&s->arena, "%s/%s.hxf", dir, stem);
                if (hx_file_exists(cand2)) {
                    int seen = 0;
                    for (int k = 0; k < done.len; k++)
                        if (!strcmp(done.data[k], cand2)) seen = 1;
                    if (!seen) {
                        HX_VEC_PUSH(done, cand2);
                        HX_VEC_PUSH(pending, cand2);
                    }
                } else if (use_hxc) {
                    char *hxc = hx_arena_sprintf(&s->arena, "%s/%s.hxc", use_hxc, stem);
                    HxModule *um = hx_hxc_read(&s->arena, s->intern, &s->diags, hxc);
                    if (!um) {
                        hx_error(&s->diags, im->path_span, "E0501",
                                 hx_arena_sprintf(&s->arena,
                                                  "no hay fuente ni unidad .hxc para '%s'", sp),
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
                    hx_error(&s->diags, im->path_span, "E0501",
                             hx_arena_sprintf(&s->arena,
                                              "no se encontró el módulo '%s' (busqué %s.hxs y "
                                              "%s.hxf)",
                                              sp, stem, stem),
                             "las rutas de IMPORT se resuelven relativas al archivo de entrada",
                             NULL);
                }
            }
        }
    }
    (void)entry_loaded;
    for (int i = 0; i < s->unit.modules.len; i++) {
        HxModule *m = &s->unit.modules.data[i];
        for (int j = 0; j < m->funcs.len; j++) {
            for (int k = j + 1; k < m->funcs.len; k++) {
                if (m->funcs.data[j].name == m->funcs.data[k].name) {
                    hx_error(&s->diags, m->funcs.data[k].name_span, "E0502",
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
    char **argv; /* vector de argumentos propio cuando se compila en paralelo */
    pid_t pid;   /* 0 si no hay proceso en vuelo */
} HxTu;

static int hx_link_objects(HxSession *s, HxBuildOpts *o, HxTu *tus, int ntus,
                          const char *bin_path) {
    char *argv[1024];
    int n = 0;
    argv[n++] = "cc";
    for (int i = 0; i < ntus; i++) argv[n++] = (char *)tus[i].obj;
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
    argv[n++] = (char *)bin_path;
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

static int hx_cc_argv(char **argv, const char *cc_file, const char *obj_file, int optimize,
                      HxProfile profile) {
    int n = 0;
    argv[n++] = "cc";
    argv[n++] = "-c";
    argv[n++] = (char *)cc_file;
    argv[n++] = "-o";
    argv[n++] = (char *)obj_file;
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

static int hx_compile_tu(const char *cc_file, const char *obj_file, int optimize,
                         HxProfile profile, int verbose) {
    char *argv[32];
    hx_cc_argv(argv, cc_file, obj_file, optimize, profile);
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
    /* una compilacion por nucleo; cc ya usa varios hilos por dentro */
    if (o->jobs > 0) jobs = o->jobs;
    else {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
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
                tu->argv = (char **)hx_arena_calloc(&s->arena, sizeof(char *) * 32);
                hx_cc_argv(tu->argv, tu->cfile, tu->obj, o->optimize, o->profile);
                pid_t pid = hx_spawn(tu->argv);
                if (pid < 0) {
                    if (hx_compile_tu(tu->cfile, tu->obj, o->optimize, o->profile, o->verbose) != 0)
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
                if (hx_compile_tu(tu->cfile, tu->obj, o->optimize, o->profile, o->verbose) != 0)
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
            char *stem = hx_sym_str(m->name);
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
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s > \"%s\" 2>&1", bin, out_path);
    int rc = system(cmd);
    return rc == 0 ? 0 : 1;
#else
    char *argv[3];
    argv[0] = (char *)bin;
    argv[1] = NULL;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (!freopen(out_path, "w", stdout)) _exit(127);
        if (!freopen(out_path, "a", stderr)) _exit(127);
        execv(bin, argv);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
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
            "  hxc kit   <archivo.hxk> [--path DIR]   (resuelve dependencias)\n"
            "  hxc check <archivo.hxe>\n"
            "  hxc size <binario>\n"
            "  hxc version\n", HX_VERSION);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        hx_usage();
        return 2;
    }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version") || !strcmp(cmd, "-V")) {
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
            hx_diag_render(&diags, NULL, stderr);
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
            double ms = 0;
            if (hx_build_main(ts, arg, &to, bin, &ms) != 0) {
                hx_diag_render(&ts->diags, NULL, stderr);
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
            hx_diag_render(&s->diags, NULL, stderr);
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

    g_extra_mod_dirs = o.mod_dirs;
    g_n_extra_mod_dirs = o.n_mod_dirs;
    double ms = 0;
    if (hx_build_main(s, entry, &o, bin_path, &ms) != 0) {
        const char *src = NULL;
        for (int i = 0; i < s->unit.modules.len; i++) src = s->unit.modules.data[i].src;
        hx_diag_render(&s->diags, src ? src : "", stderr);
        return 1;
    }
    hx_diag_render(&s->diags, NULL, stderr);

    if (emit_only) return 0;

    if (run_mode) {
        char *rargv[3];
        rargv[0] = (char *)bin_path;
        rargv[1] = NULL;
        int rc = hx_run(rargv);
        return rc < 0 ? 1 : rc;
    }
    return 0;
}