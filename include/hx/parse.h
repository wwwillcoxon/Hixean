#ifndef HX_PARSE_H
#define HX_PARSE_H

#include "hx/ast.h"

void hx_parse_module(HxUnit *unit, HxModule *m, const char *src, const char *file);
HxExpr *hx_parse_subexpr(HxUnit *unit, const char *src, const char *file);

#endif