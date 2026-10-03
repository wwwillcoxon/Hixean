#ifndef HX_MONO_H
#define HX_MONO_H

#include "hx/ast.h"

int hx_ty_has_tparam(const HxTy *t, const HxSym *tps, int n);
HxTy *hx_ty_subst(HxArena *a, const HxTy *t, const HxSym *tps, HxTy **subs, int n);
void hx_ty_mangle(const HxTy *t, char *out, int cap);

struct HxTypeDecl *hx_type_instantiate(HxArena *a, HxIntern *intern, HxUnit *unit, HxTypeDecl *g,
                                 HxTy **targs, int n, HxDiagBag *diags, HxSpan sp);

struct HxFunc *hx_func_instantiate(HxArena *a, HxIntern *intern, struct HxFunc *g, HxTy **targs, int n,
                            int module, const char *module_name, HxSpan sp, HxDiagBag *diags);

#endif
