#ifndef HX_DIAG_H
#define HX_DIAG_H

#include "hx/common.h"

typedef enum {
    HX_SEV_ERROR,
    HX_SEV_WARN,
    HX_SEV_NOTE,
    HX_SEV_HELP
} HxSeverity;

typedef struct {
    HxSeverity sev;
    const char *code;
    HxSpan span;
    const char *msg;
    const char *note;
    const char *help;
    const char *file;
    const char *src;
} HxDiag;

typedef struct {
    HxArena *arena;
    HX_VEC_ANON(HxDiag) items;
    int errors;
    int warnings;
    int max_errors;
    int json;
    const char *ctx_file;
    const char *ctx_src;
} HxDiagBag;

void hx_diag_init(HxDiagBag *d, HxArena *arena);
HxSpan hx_diag_span(const char *file, const char *src, const char *start, const char *end);
HX_PRINTF(4, 5) void hx_error(HxDiagBag *d, HxSpan span, const char *code,
                           const char *fmt, ...);
HX_PRINTF(4, 5) void hx_warn(HxDiagBag *d, HxSpan span, const char *code,
                         const char *fmt, ...);
void hx_diag_note(HxDiagBag *d, HxSpan span, const char *code, const char *msg,
                  const char *note, const char *help);
void hx_diag_render(const HxDiagBag *d, const char *src, FILE *out);
void hx_diag_render_json(const HxDiagBag *d, const char *src, FILE *out);

#endif