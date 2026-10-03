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

int hx_version_cmp(const char *a, const char *b);
int hx_kit_resolve(HxArena *arena, const char *file, HxPathList *paths, HxDiagBag *diags,
                   HxKit *out);
char *hx_kit_entry_path(HxArena *arena, HxKit *kit, const char *manifest);

#endif
