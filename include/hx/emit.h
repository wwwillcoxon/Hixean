#ifndef HX_EMIT_H
#define HX_EMIT_H

#include "hx/ast.h"

typedef enum { HX_PROFILE_FREESTANDING = 0, HX_PROFILE_LIBC = 1 } HxProfile;

int hx_emit_unit(HxArena *arena, HxUnit *unit, const char *out_path, HxProfile profile,
                 const char *out_bin, int keep_asm, const char *keep_asm_path);

#endif