#define _POSIX_C_SOURCE 200809L

#include "hx/check.h"
#include "hx/diag.h"
#include "hx/emit.h"
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

static char *hx_with_ext(HxArena *a, const char *path, const char *ext) {
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    if (dot && (!slash || dot > slash))
        return hx_arena_sprintf(a, "%.*s%s", (int)(dot - path), path, ext);
    return hx_arena_sprintf(a, "%s%s", path, ext);
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

static void hx_collect_modules(HxSession *s, const char *entry_path) {
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
            if (hx_file_exists(cand)) {
                int seen = 0;
                for (int k = 0; k < done.len; k++)
                    if (!strcmp(done.data[k], cand)) seen = 1;
                if (!seen) {
                    HX_VEC_PUSH(done, cand);
                    HX_VEC_PUSH(pending, cand);
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

static void hx_link(HxSession *s, HxBuildOpts *o, const char *c_path, const char *obj_path,
                    const char *bin_path) {
    char *o_path = hx_with_ext(&s->arena, c_path, ".o");
    char *cc = "cc";
    char *argv[32];
    int n = 0;
    argv[n++] = cc;
    argv[n++] = "-c";
    argv[n++] = (char *)c_path;
    argv[n++] = "-o";
    argv[n++] = o_path;
    argv[n++] = o->optimize > 1 ? "-O2" : o->optimize == 1 ? "-O1" : "-O0";
    if (o->optimize >= 1) {
        argv[n++] = "-fomit-frame-pointer";
    }
    argv[n++] = "-w";
    if (o->profile == HX_PROFILE_FREESTANDING) {
        argv[n++] = "-ffreestanding";
        argv[n++] = "-fno-builtin";
        argv[n++] = "-fno-stack-protector";
        argv[n++] = "-U_FORTIFY_SOURCE";
        argv[n++] = "-D_FORTIFY_SOURCE=0";
    }
    argv[n++] = "-std=c11";
    argv[n++] = NULL;
    if (hx_run(argv) != 0) {
        fprintf(stderr, "hx: falló la compilación de %s\n", c_path);
        exit(1);
    }

    n = 0;
    argv[n++] = cc;
    argv[n++] = o_path;
    argv[n++] = "-o";
    argv[n++] = (char *)bin_path;
    if (o->profile == HX_PROFILE_FREESTANDING) {
        argv[n++] = "-nostdlib";
        argv[n++] = "-static";
        argv[n++] = "-no-pie";
        argv[n++] = "-fno-pie";
    }
    argv[n++] = "-s";
    argv[n++] = NULL;
    if (hx_run(argv) != 0) {
        fprintf(stderr, "hx: falló el enlazado de %s\n", bin_path);
        exit(1);
    }
}

static int hx_build_main(HxSession *s, const char *entry, HxBuildOpts *o, const char *bin_path,
                         double *out_ms) {
    double t0 = hx_now_ms();
    hx_collect_modules(s, entry);
    hx_check_unit(&s->unit);
    if (s->diags.errors) return 1;

    char *cpath = hx_with_ext(&s->arena, bin_path, ".c");
    char *dir = hx_path_dirname(&s->arena, cpath);
    if (!hx_file_exists(dir)) hx_mkdir_p(dir);
    hx_emit_unit(&s->arena, &s->unit, cpath, o->profile, bin_path, o->keep_asm,
                 o->keep_asm_path);
    double t_emit = hx_now_ms();

    hx_link(s, o, cpath, NULL, bin_path);
    double t_end = hx_now_ms();
    if (o->verbose) {
        int sz = hx_file_size(bin_path);
        fprintf(stderr, "hx: front-end+emit %.1f ms | cc+link %.1f ms | %d bytes\n",
                t_emit - t0, t_end - t_emit, sz);
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
    if (!entry) {
        hx_usage();
        return 2;
    }

    HxSession *s = (HxSession *)malloc(sizeof(HxSession));
    hx_session_init(s);

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