#ifndef HX_KIT_H
#define HX_KIT_H

#include "hx/ast.h"

#define HX_KIT_MAX 32

typedef struct HxKit HxKit;

typedef struct {
    const char *name;
    const char *op;
    const char *version;
    int is_require;
    int line;
} HxKitDep;

typedef struct {
    const char *data[64];
    int len;
} HxPathList;

struct HxKit {
    HxArena *arena;
    const char *file;
    const char *name;
    const char *version;
    const char *entry;
    const char *target;
    const char *profile;
    const char *caps[HX_KIT_MAX];
    int n_caps;
    const char *features[HX_KIT_MAX];
    int n_features;
    const char *provides[HX_KIT_MAX];
    int n_provides;
    const char *assets[HX_KIT_MAX];
    int n_assets;
    const char *defines[HX_KIT_MAX];
    int n_defines;
    const char *opts[HX_KIT_MAX];
    int n_opts;
    HX_VEC_ANON(HxKitDep) deps;
    HxKit **kits; /* dependencias resueltas */
    int n_kits;
    int cap_kits;
};

/* Una consulta `.hxq` busca paquetes en las rutas por sus PROVIDES, FEATURE,
   CAPABILITY, DEP o VERSION. Cada predicado es una linea `CLAVE valor` y, si el
   valor lleva restriccion de version, `CLAVE valor op version`. */

typedef struct {
    const char *key;   /* PROVIDES, FEATURE, CAPABILITY, DEP, VERSION */
    const char *value; /* nombre, o version si key es VERSION */
    const char *op;    /* =, >, >=, <, <= */
    const char *version; /* segunda palabra, solo con operador */
} HxQueryPred;

typedef struct {
    HxArena *arena;
    HX_VEC_ANON(HxQueryPred) preds;
} HxQuery;

int hx_query_parse(HxArena *arena, const char *file, HxDiagBag *diags, HxQuery *out);
/* Devuelve el numero de paquetes que cumplen la consulta. */
int hx_query_run(HxQuery *q, HxPathList *paths, HxDiagBag *diags);

/* Un paquete publicado es un directorio `<registro>/<nombre>/` con el manifiesto
   `<nombre>.hxk` al lado de sus fuentes. `hxc pack` lo arma y `hxc install` lo
   copia desde un registro (un directorio o un repositorio git clonado). */
/* Publicar un paquete: copia el manifiesto y los modulos que importa al layout
   `<registro>/<nombre>/<nombre>.hxk`. */
int hx_kit_parse(HxKit *kit, const char *file, HxDiagBag *diags);
int hx_kit_install(HxArena *arena, const char *name, const char *registry, const char *into,
                   HxDiagBag *diags, char *installed_dir, size_t installed_cap);

int hx_version_cmp(const char *a, const char *b);
int hx_kit_resolve(HxArena *arena, const char *file, HxPathList *paths, HxDiagBag *diags,
                   HxKit *out);
char *hx_kit_entry_path(HxArena *arena, HxKit *kit, const char *manifest);

#endif
