#ifndef HX_EMIT_H
#define HX_EMIT_H

#include "hx/ast.h"

typedef enum { HX_PROFILE_FREESTANDING = 0, HX_PROFILE_LIBC = 1 } HxProfile;

typedef struct {
    HxProfile profile;
    const char *dir_gen;
    const char *dir_runtime;
    int keep_asm;
    const char *keep_asm_path;
} HxEmitOptions;

int hx_emit_unit(HxArena *arena, HxUnit *unit, HxEmitOptions *opt);

#endif
