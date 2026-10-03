#include "hx/kit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#endif

/* Un `.hxk` describe un paquete:
     KIT nombre 1.2.3
       TARGET hixe >= 0.1
       PROFILE freestanding
       ENTRY matematicas.hxe
       DEP base 0.2
       REQUIRE net >= 0.1
       FEATURE iteradores
       PROVIDES matematicas
       ASSET datos/malla.hxm
       DEFINE HX_GPU 1
       OPT "-O3"
     END KIT

   La resolucion busca cada DEP en las rutas indicadas con --path, comprueba
   la version contra la peticion y recoge las FEATURE de las dependencias para
   poder validar los REQUIRE. */

static char *hx_kit_skip_ws(char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static void hx_kit_word(char **pp, char *out, int cap) {
    char *p = hx_kit_skip_ws(*pp);
    int n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && n < cap - 1)
        out[n++] = *p++;
    out[n] = 0;
    *pp = p;
}

static void hx_kit_line(char **pp, char *out, int cap) {
    char *p = hx_kit_skip_ws(*pp);
    int n = 0;
    while (*p && *p != '\n' && n < cap - 1) out[n++] = *p++;
    out[n] = 0;
    while (*p && *p != '\n') p++;
    if (*p == '\n') p++;
    *pp = p;
}

int hx_version_cmp(const char *a, const char *b) {
    for (;;) {
        long x = strtol(a, (char **)&a, 10);
        long y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        while (*a && *a != '.') a++;
        while (*b && *b != '.') b++;
        if (!*a && !*b) return 0;
        if (!*a) return -1;
        if (!*b) return 1;
        a++;
        b++;
    }
}

static int hx_kit_visit(HxKit *kit, const char *path, const char *want_version,
                        HxPathList *paths, HxDiagBag *diags, int depth);

static int hx_kit_feature(HxKit *kit, const char *name) {
    for (int i = 0; i < kit->n_features; i++)
        if (!strcmp(kit->features[i], name)) return 1;
    return 0;
}

static int hx_kit_parse(HxKit *kit, const char *file, HxDiagBag *diags) {
    char *src = hx_read_file(kit->arena, file, NULL);
    if (!src) {
        hx_error(diags, (HxSpan){0, 0}, "E0801", "no se encontró el manifiesto %s", file);
        return 0;
    }
    kit->file = hx_arena_strdup(kit->arena, file);
    const char *ctx_prev = diags->ctx_file;
    diags->ctx_file = kit->file;
    char *p = src;
    int line = 0;
    char word[256], rest[512];
    /* KIT nombre version */
    hx_kit_word(&p, word, sizeof(word));
    line++;
    if (strcmp(word, "KIT")) {
        hx_error(diags, (HxSpan){0, 0}, "E0802", "un manifiesto empieza con KIT");
        diags->ctx_file = ctx_prev;
        return 0;
    }
    hx_kit_word(&p, word, sizeof(word));
    if (!word[0]) {
        hx_error(diags, (HxSpan){0, 0}, "E0802", "falta el nombre del KIT");
        diags->ctx_file = ctx_prev;
        return 0;
    }
    kit->name = hx_arena_strdup(kit->arena, word);
    hx_kit_word(&p, word, sizeof(word));
    kit->version = hx_arena_strdup(kit->arena, word[0] ? word : "0.0.0");

    while (*p) {
        char *antes = p;
        hx_kit_word(&p, word, sizeof(word));
        if (!word[0]) {
            hx_kit_line(&p, rest, sizeof(rest));
            continue;
        }
        line++;
        if (!strcmp(word, "END")) {
            hx_kit_word(&p, word, sizeof(word));
            if (strcmp(word, "KIT")) {
                hx_error(diags, (HxSpan){0, 0}, "E0802", "se esperaba END KIT");
                diags->ctx_file = ctx_prev;
            diags->ctx_file = ctx_prev;
        return 0;
            }
            if (!kit->entry) {
                hx_error(diags, (HxSpan){0, 0}, "E0803", "falta ENTRY", file);
                diags->ctx_file = ctx_prev;
                diags->ctx_file = ctx_prev;
            diags->ctx_file = ctx_prev;
        return 0;
            }
            diags->ctx_file = ctx_prev;
            return 1;
        }
        if (!strcmp(word, "ENTRY")) {
            hx_kit_word(&p, word, sizeof(word));
            kit->entry = hx_arena_strdup(kit->arena, word);
        } else if (!strcmp(word, "TARGET")) {
            hx_kit_line(&p, rest, sizeof(rest));
            kit->target = hx_arena_strdup(kit->arena, rest);
        } else if (!strcmp(word, "PROFILE")) {
            hx_kit_word(&p, word, sizeof(word));
            kit->profile = hx_arena_strdup(kit->arena, word);
        } else if (!strcmp(word, "PROVIDES")) {
            hx_kit_word(&p, word, sizeof(word));
            if (kit->n_provides < HX_KIT_MAX) {
                kit->provides[kit->n_provides++] = hx_arena_strdup(kit->arena, word);
            }
        } else if (!strcmp(word, "CAPABILITY")) {
            hx_kit_line(&p, rest, sizeof(rest));
            if (kit->n_caps < HX_KIT_MAX) {
                kit->caps[kit->n_caps++] = hx_arena_strdup(kit->arena, rest);
            }
        } else if (!strcmp(word, "FEATURE")) {
            hx_kit_word(&p, word, sizeof(word));
            if (kit->n_features < HX_KIT_MAX) {
                kit->features[kit->n_features++] = hx_arena_strdup(kit->arena, word);
            }
        } else if (!strcmp(word, "ASSET")) {
            hx_kit_word(&p, word, sizeof(word));
            if (kit->n_assets < HX_KIT_MAX) {
                kit->assets[kit->n_assets++] = hx_arena_strdup(kit->arena, word);
            }
        } else if (!strcmp(word, "DEP") || !strcmp(word, "REQUIRE")) {
            char kind[16];
            snprintf(kind, sizeof(kind), "%.8s", word);
            hx_kit_word(&p, word, sizeof(word));
            char nm[128];
            snprintf(nm, sizeof(nm), "%.127s", word);
            hx_kit_word(&p, word, sizeof(word));
            /* `>= 0.2` o `0.2` */
            char *q = hx_kit_skip_ws(p);
            char op[8] = "=";
            if (q[0] == '>' || q[0] == '<' || q[0] == '=') {
                int i = 0;
                op[i++] = *q++;
                if (*q == '=') op[i++] = *q++;
                op[i] = 0;
                while (*q == ' ') q++;
                p = q;
            }
            hx_kit_word(&p, word, sizeof(word));
            HxKitDep d;
            memset(&d, 0, sizeof(d));
            d.name = hx_arena_strdup(kit->arena, nm);
            d.op = hx_arena_strdup(kit->arena, op);
            d.version = hx_arena_strdup(kit->arena, word[0] ? word : "0.0.0");
            d.is_require = !strcmp(kind, "REQUIRE");
            d.line = line;
            HX_VEC_PUSH(kit->deps, d);
        } else if (!strcmp(word, "DEFINE")) {
            hx_kit_line(&p, rest, sizeof(rest));
            if (kit->n_defines < HX_KIT_MAX) {
                kit->defines[kit->n_defines++] = hx_arena_strdup(kit->arena, rest);
            }
        } else if (!strcmp(word, "OPT")) {
            hx_kit_word(&p, word, sizeof(word));
            if (kit->n_opts < HX_KIT_MAX) {
                kit->opts[kit->n_opts++] = hx_arena_strdup(kit->arena, word);
            }
        } else if (!strcmp(word, "BENCH")) {
            hx_kit_line(&p, rest, sizeof(rest));
        } else if (!strcmp(word, "EXPECT")) {
            hx_kit_line(&p, rest, sizeof(rest));
        } else if (!strcmp(word, "LINK") || !strcmp(word, "BACKEND")) {
            hx_kit_line(&p, rest, sizeof(rest));
        } else {
            hx_kit_line(&p, rest, sizeof(rest));
            hx_error(diags, (HxSpan){0, 0}, "E0804", "línea %d: instrucción desconocida '%s'", line, word);
            diags->ctx_file = ctx_prev;
            diags->ctx_file = ctx_prev;
        return 0;
        }
        if (p == antes) break;
    }
    hx_error(diags, (HxSpan){0, 0}, "E0802", "%s: falta END KIT", file);
    return 0;
}

static char *hx_kit_dir_of(HxArena *a, const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return hx_arena_strdup(a, ".");
    return hx_arena_strndup(a, path, (size_t)(slash - path));
}

/* Busca el manifiesto de un paquete en las rutas y lo resuelve. */
static int hx_kit_visit(HxKit *kit, const char *name, const char *want_version, HxPathList *paths,
                        HxDiagBag *diags, int depth) {
    if (depth > 16) {
        hx_error(diags, (HxSpan){0, 0}, "E0805", "cadena de dependencias demasiado profunda en '%s'",
                 name);
        return 0;
    }
    for (int i = 0; i < kit->n_kits; i++) {
        HxKit *prev = kit->kits[i];
        if (strcmp(prev->name, name)) continue;
        return hx_version_cmp(prev->version, want_version) >= 0;
    }
    for (int i = 0; i < paths->len; i++) {
        char *cand = hx_arena_sprintf(kit->arena, "%s/%s/%s.hxk", paths->data[i], name, name);
        if (!hx_file_exists(cand)) {
            cand = hx_arena_sprintf(kit->arena, "%s/%s.hxk", paths->data[i], name);
            if (!hx_file_exists(cand)) continue;
        }
        HxKit *sub = (HxKit *)hx_arena_calloc(kit->arena, sizeof(HxKit));
        sub->arena = kit->arena;
        if (!hx_kit_parse(sub, cand, diags)) return 0;
        if (kit->n_kits == kit->cap_kits) {
            kit->cap_kits = kit->cap_kits ? kit->cap_kits * 2 : 8;
            kit->kits = (HxKit **)hx_arena_realloc_tmp(kit->kits,
                                                      sizeof(HxKit *) * (size_t)kit->cap_kits);
        }
        kit->kits[kit->n_kits++] = sub;
        if (hx_version_cmp(sub->version, want_version) < 0) {
            hx_error(diags, (HxSpan){0, 0}, "E0806",
                     "%s: se pidió '%s %s' pero el manifiesto encontrado es %s", cand, name,
                     want_version, sub->version);
            return 0;
        }
        /* las FEATURE y PROVIDES de la dependencia se propagan */
        for (int f = 0; f < sub->n_features; f++)
            if (!hx_kit_feature(kit, sub->features[f]) && kit->n_features < HX_KIT_MAX)
                kit->features[kit->n_features++] =
                    hx_arena_strdup(kit->arena, sub->features[f]);
        for (int f = 0; f < sub->n_provides; f++)
            if (kit->n_provides < HX_KIT_MAX)
                kit->provides[kit->n_provides++] =
                    hx_arena_strdup(kit->arena, sub->provides[f]);
        for (int d = 0; d < sub->deps.len; d++) {
            HxKitDep *dp = &sub->deps.data[d];
            if (!hx_kit_visit(kit, dp->name, dp->version, paths, diags, depth + 1)) return 0;
        }
        return 1;
    }
    hx_error(diags, (HxSpan){0, 0}, "E0807", "no se encontró el paquete '%s' en %d ruta(s)", name,
             paths->len);
    return 0;
}

int hx_kit_resolve(HxArena *arena, const char *file, HxPathList *paths, HxDiagBag *diags,
                   HxKit *out) {
    memset(out, 0, sizeof(*out));
    out->arena = arena;
    if (!hx_kit_parse(out, file, diags)) return 0;
    const char *ctx = diags->ctx_file;
    diags->ctx_file = out->file;
    for (int i = 0; i < out->deps.len; i++) {
        HxKitDep *d = &out->deps.data[i];
        if (d->is_require) {
            /* un REQUIRE se satisface con una FEATURE propia o heredada */
            int ok = hx_kit_feature(out, d->name);
            if (!ok) {
                hx_error(diags, (HxSpan){0, 0}, "E0808",
                         "'%s' requiere la capacidad '%s' y no hay FEATURE que la proporcione",
                         out->name, d->name);
                return 0;
            }
            continue;
        }
        if (!hx_kit_visit(out, d->name, d->version, paths, diags, 0)) {
            diags->ctx_file = ctx;
            return 0;
        }
    }
    diags->ctx_file = ctx;
    return 1;
}

char *hx_kit_entry_path(HxArena *arena, HxKit *kit, const char *manifest) {
    char *dir = hx_kit_dir_of(arena, manifest);
    return hx_arena_sprintf(arena, "%s/%s", dir, kit->entry);
}
static int hx_kit_has(HxKit *k, const char *key, const char *value) {
    if (!strcmp(key, "PROVIDES"))
        for (int i = 0; i < k->n_provides; i++)
            if (!strcmp(k->provides[i], value)) return 1;
    if (!strcmp(key, "FEATURE"))
        for (int i = 0; i < k->n_features; i++)
            if (!strcmp(k->features[i], value)) return 1;
    if (!strcmp(key, "CAPABILITY"))
        for (int i = 0; i < k->n_caps; i++)
            if (!strcmp(k->caps[i], value)) return 1;
    if (!strcmp(key, "DEP"))
        for (int i = 0; i < k->deps.len; i++)
            if (!k->deps.data[i].is_require && !strcmp(k->deps.data[i].name, value)) return 1;
    return 0;
}

/* --- consultas ----------------------------------------------------------- */

/* Listado de un directorio: mismo comportamiento en POSIX y Windows. */
static int hx_dir_entries(HxArena *arena, const char *dir, HxPathList *out) {
    out->len = 0;
#ifdef _WIN32
    char patron[MAX_PATH];
    snprintf(patron, sizeof(patron), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(patron, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (fd.cFileName[0] == '.') continue;
        if (out->len < (int)(sizeof(out->data) / sizeof(out->data[0])))
            out->data[out->len++] = hx_arena_strdup(arena, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (out->len < (int)(sizeof(out->data) / sizeof(out->data[0])))
            out->data[out->len++] = hx_arena_strdup(arena, ent->d_name);
    }
    closedir(d);
#endif
    for (int i = 1; i < out->len; i++) {
        const char *clave = out->data[i];
        int j = i - 1;
        while (j >= 0 && strcmp(out->data[j], clave) > 0) {
            out->data[j + 1] = out->data[j];
            j--;
        }
        out->data[j + 1] = clave;
    }
    return 1;
}

static int hx_es_operador(const char *w) {
    return !strcmp(w, ">") || !strcmp(w, "<") || !strcmp(w, ">=") || !strcmp(w, "<=") ||
           !strcmp(w, "=");
}

static int hx_ver_cumple(const char *a, const char *b, const char *op) {
    int c = hx_version_cmp(a ? a : "0", b);
    if (!strcmp(op, ">=")) return c >= 0;
    if (!strcmp(op, "<=")) return c <= 0;
    if (!strcmp(op, ">")) return c > 0;
    if (!strcmp(op, "<")) return c < 0;
    return c == 0;
}

static int hx_query_ok(HxQueryPred *pr, HxKit *k) {
    if (!strcmp(pr->key, "VERSION")) return hx_ver_cumple(k->version, pr->value, pr->op);
    if (!strcmp(pr->key, "DEP")) {
        for (int i = 0; i < k->deps.len; i++) {
            HxKitDep *d = &k->deps.data[i];
            if (d->is_require || strcmp(d->name, pr->value)) continue;
            if (!pr->version) return 1;
            return hx_ver_cumple(d->version ? d->version : "0", pr->version, pr->op);
        }
        return 0;
    }
    return hx_kit_has(k, pr->key, pr->value);
}

int hx_query_parse(HxArena *arena, const char *file, HxDiagBag *diags, HxQuery *out) {
    memset(out, 0, sizeof(*out));
    out->arena = arena;
    char *src = hx_read_file(arena, file, NULL);
    if (!src) {
        hx_error(diags, (HxSpan){0, 0}, "E0811", "no se encontró la consulta %s", file);
        return 0;
    }
    const char *ctx_prev = diags->ctx_file;
    diags->ctx_file = file;
    char *p = src;
    char word[256];
    int line = 1;
    hx_kit_word(&p, word, sizeof(word));
    if (strcmp(word, "QUERY")) {
        hx_error(diags, (HxSpan){0, 0}, "E0812", "una consulta empieza con QUERY");
        diags->ctx_file = ctx_prev;
        return 0;
    }
    /* lo que sigue a QUERY en la misma linea es texto libre */
    char descripcion[256];
    hx_kit_line(&p, descripcion, sizeof(descripcion));
    while (*p) {
        hx_kit_word(&p, word, sizeof(word));
        if (!word[0]) {
            hx_kit_line(&p, descripcion, sizeof(descripcion));
            line++;
            continue;
        }
        line++;
        if (!strcmp(word, "END")) {
            hx_kit_word(&p, word, sizeof(word));
            if (strcmp(word, "QUERY")) {
                hx_error(diags, (HxSpan){0, 0}, "E0812", "se esperaba END QUERY");
                diags->ctx_file = ctx_prev;
                return 0;
            }
            if (!out->preds.len) {
                hx_error(diags, (HxSpan){0, 0}, "E0813", "la consulta no pide nada");
                diags->ctx_file = ctx_prev;
                return 0;
            }
            diags->ctx_file = ctx_prev;
            return 1;
        }
        static const char *claves[] = {"PROVIDES", "FEATURE", "CAPABILITY", "DEP",
                                       "VERSION", NULL};
        int ok = 0;
        for (int i = 0; claves[i]; i++)
            if (!strcmp(word, claves[i])) ok = 1;
        if (!ok) {
            hx_error(diags, (HxSpan){0, 0}, "E0814",
                     "línea %d: predicado desconocido '%s' (usa PROVIDES, FEATURE, "
                     "CAPABILITY, DEP o VERSION)",
                     line, word);
            diags->ctx_file = ctx_prev;
            return 0;
        }
        char clave[64];
        snprintf(clave, sizeof(clave), "%.63s", word);
        HxQueryPred pred;
        memset(&pred, 0, sizeof(pred));
        pred.key = hx_arena_strdup(arena, clave);
        pred.op = "=";
        char op[4] = "=";
        char version[64];
        version[0] = 0;
        hx_kit_word(&p, word, sizeof(word));
        if (!word[0]) {
            hx_error(diags, (HxSpan){0, 0}, "E0814", "línea %d: falta el valor de %s", line,
                     clave);
            diags->ctx_file = ctx_prev;
            return 0;
        }
        if (hx_es_operador(word)) {
            /* VERSION >= 1.0 */
            snprintf(op, sizeof(op), "%.3s", word);
            pred.value = hx_arena_strdup(arena, "");
            hx_kit_word(&p, word, sizeof(word));
            if (!word[0]) {
                hx_error(diags, (HxSpan){0, 0}, "E0814", "línea %d: falta la version", line);
                diags->ctx_file = ctx_prev;
                return 0;
            }
            snprintf(version, sizeof(version), "%.63s", word);
        } else {
            /* CLAVE valor [op version] */
            pred.value = hx_arena_strdup(arena, word);
            char mas[256];
            char *guarda = p;
            hx_kit_word(&p, mas, sizeof(mas));
            if (hx_es_operador(mas)) {
                snprintf(op, sizeof(op), "%.3s", mas);
                hx_kit_word(&p, word, sizeof(word));
                if (!word[0]) {
                    hx_error(diags, (HxSpan){0, 0}, "E0814",
                             "línea %d: falta la version tras '%s'", line, op);
                    diags->ctx_file = ctx_prev;
                    return 0;
                }
                snprintf(version, sizeof(version), "%.63s", word);
            } else {
                p = guarda;
            }
        }
        if (!strcmp(clave, "VERSION")) {
            if (!pred.value[0]) pred.value = hx_arena_strdup(arena, version);
        } else if (version[0] && strcmp(clave, "DEP")) {
            hx_error(diags, (HxSpan){0, 0}, "E0814",
                     "línea %d: %s solo admite comparacion con VERSION o DEP", line, clave);
            diags->ctx_file = ctx_prev;
            return 0;
        }
        pred.op = hx_arena_strdup(arena, op);
        pred.version = version[0] ? hx_arena_strdup(arena, version) : NULL;
        HX_VEC_PUSH(out->preds, pred);
    }
    hx_error(diags, (HxSpan){0, 0}, "E0812", "falta END QUERY");
    diags->ctx_file = ctx_prev;
    return 0;
}

/* Recorre <ruta>/<nombre>/<nombre>.hxk y <ruta>/<nombre>.hxk una vez. */
static int hx_query_visit(HxQuery *q, const char *dir, int *cuenta) {
    HxPathList entradas;
    if (!hx_dir_entries(q->arena, dir, &entradas)) return 0;
    for (int i = 0; i < entradas.len; i++) {
        const char *name = entradas.data[i];
        size_t ln = strlen(name);
        char *plano = hx_arena_sprintf(q->arena, "%s/%s.hxk", dir, name);
        char *anidado = hx_arena_sprintf(q->arena, "%s/%s/%s.hxk", dir, name, name);
        const char *usado = NULL;
        if (ln > 4 && !strcmp(name + ln - 4, ".hxk")) {
            char *directo = hx_arena_sprintf(q->arena, "%s/%s", dir, name);
            if (hx_file_exists(directo)) usado = directo;
        }
        if (!usado && hx_file_exists(plano)) usado = plano;
        if (!usado && hx_file_exists(anidado)) usado = anidado;
        if (!usado) continue;
        HxKit k;
        memset(&k, 0, sizeof(k));
        k.arena = q->arena;
        HxDiagBag silencioso;
        hx_diag_init(&silencioso, q->arena);
        silencioso.max_errors = 0;
        if (!hx_kit_parse(&k, usado, &silencioso)) continue;
        int cumple = 1;
        for (int j = 0; j < q->preds.len && cumple; j++)
            if (!hx_query_ok(&q->preds.data[j], &k)) cumple = 0;
        if (!cumple) continue;
        (*cuenta)++;
        printf("%s %s  %s\n", k.name, k.version, usado);
    }
    return 1;
}

int hx_query_run(HxQuery *q, HxPathList *paths, HxDiagBag *diags) {
    (void)diags;
    int cuenta = 0;
    for (int i = 0; i < paths->len; i++)
        if (!hx_query_visit(q, paths->data[i], &cuenta)) {
            fprintf(stderr, "hx: no se pudo leer la ruta %s\n", paths->data[i]);
            return -1;
        }
    if (!cuenta) printf("sin resultados\n");
    return cuenta;
}
