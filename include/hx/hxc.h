#ifndef HX_HXC_H
#define HX_HXC_H

#include "hx/ast.h"

int hx_hxc_write(HxArena *arena, HxUnit *unit, HxModule *m, const char *path);
HxModule *hx_hxc_read(HxArena *arena, HxIntern *intern, HxDiagBag *diags, const char *path);

#endif
