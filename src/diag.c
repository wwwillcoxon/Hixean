#include "hx/diag.h"

#include <string.h>

void hx_diag_init(HxDiagBag *d, HxArena *arena) {
    memset(d, 0, sizeof(*d));
    d->arena = arena;
    d->max_errors = 40;
}

HxSpan hx_diag_span(const char *file, const char *src, const char *start, const char *end) {
    HxSpan s;
    s.start = (uint32_t)(start - src);
    s.len = (uint32_t)(end > start ? end - start : 0);
    (void)file;
    return s;
}

static void hx_push_diag(HxDiagBag *d, HxSeverity sev, HxSpan span, const char *code,
                         const char *msg, const char *note, const char *help) {
    HxDiag diag;
    memset(&diag, 0, sizeof(diag));
    diag.sev = sev;
    diag.span = span;
    diag.code = code;
    diag.msg = msg;
    diag.note = note;
    diag.help = help;
    diag.file = d->ctx_file;
    diag.src = d->ctx_src;
    HX_VEC_PUSH(d->items, diag);
    if (sev == HX_SEV_ERROR) {
        d->errors++;
        if (d->errors >= d->max_errors) {
            HxDiag last;
            memset(&last, 0, sizeof(last));
            last.sev = HX_SEV_NOTE;
            last.code = "E0000";
            last.msg = "demasiados errores, se detiene la compilación";
            HX_VEC_PUSH(d->items, last);
            d->max_errors = 1 << 30;
        }
    } else if (sev == HX_SEV_WARN) {
        d->warnings++;
    }
}

void hx_error(HxDiagBag *d, HxSpan span, const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *msg = hx_arena_vsprintf(d->arena, fmt, ap);
    va_end(ap);
    hx_push_diag(d, HX_SEV_ERROR, span, code, msg, NULL, NULL);
}

void hx_warn(HxDiagBag *d, HxSpan span, const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *msg = hx_arena_vsprintf(d->arena, fmt, ap);
    va_end(ap);
    hx_push_diag(d, HX_SEV_WARN, span, code, msg, NULL, NULL);
}

void hx_diag_note(HxDiagBag *d, HxSpan span, const char *code, const char *msg,
                  const char *note, const char *help) {
    hx_push_diag(d, HX_SEV_ERROR, span, code, msg, note, help);
}

static const char *hx_sev_label(HxSeverity s) {
    switch (s) {
        case HX_SEV_ERROR: return "error";
        case HX_SEV_WARN: return "aviso";
        case HX_SEV_NOTE: return "nota";
        default: return "ayuda";
    }
}

static void hx_render_one(const HxDiag *d, const char *fallback, FILE *out) {
    const char *src = d->src ? d->src : fallback;
    const char *file = d->file ? d->file : "";
    uint32_t line = 1, col = 1, end_line = 1;
    const char *line_start = src;
    if (src) {
        for (uint32_t i = 0; i < d->span.start && src[i]; i++) {
            if (src[i] == '\n') {
                line++;
                col = 1;
                line_start = src + i + 1;
            } else {
                col++;
            }
        }
        /* end_line si se usa (el caret se ensancha si el span salta de linea);
           end_col no lo era, y clang con -Werror no deja variables puestas y
           sin usar */
        end_line = line;
        for (uint32_t i = d->span.start; i < d->span.start + d->span.len && src[i]; i++)
            if (src[i] == '\n') end_line++;
    }
    fprintf(out, "%s:%u:%u: %s[%s]: %s\n", file, line, col, hx_sev_label(d->sev), d->code,
            d->msg ? d->msg : "");
    if (src && d->span.len) {
        uint32_t len = (uint32_t)strlen(line_start);
        uint32_t vis = 0;
        while (vis < len && line_start[vis] != '\n' && vis < 200) vis++;
        fprintf(out, "   %5u | %.*s\n", line, (int)vis, line_start);
        fprintf(out, "   %5s | ", "");
        for (uint32_t i = 0; i < col - 1; i++) fputc(line_start[i] == '\t' ? '\t' : ' ', out);
        uint32_t w = d->span.start + d->span.len <= d->span.start + 1 ? 1 : d->span.len;
        if (end_line != line) w = vis > col - 1 ? vis - col + 1 : 1;
        for (uint32_t i = 0; i < w && i < 200; i++) fputc('^', out);
        fputc('\n', out);
    }
    if (d->note) fprintf(out, "   nota: %s\n", d->note);
    if (d->help) fprintf(out, "   ayuda: %s\n", d->help);
    fputc('\n', out);
}

void hx_diag_render(const HxDiagBag *d, const char *src, FILE *out) {
    for (int i = 0; i < d->items.len; i++) hx_render_one(&d->items.data[i], src, out);
}

static void hx_json_str(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
            case '"': fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (*p < 0x20) fprintf(out, "\\u%04x", *p);
                else fputc(*p, out);
        }
    }
    fputc('"', out);
}

/* El formato que consumen el editor, el LSP y el CI: un objeto por diagnostico
   con la posicion real, el codigo estable y el mensaje entero. */
void hx_diag_render_json(const HxDiagBag *d, const char *src, FILE *out) {
    fputs("{\"diagnostics\":[", out);
    for (int i = 0; i < d->items.len; i++) {
        const HxDiag *x = &d->items.data[i];
        const char *fuente = x->src ? x->src : src;
        uint32_t line = 1, col = 1, end_line = 1, end_col = 1;
        if (fuente) {
            for (uint32_t k = 0; k < x->span.start && fuente[k]; k++) {
                if (fuente[k] == '\n') {
                    line++;
                    col = 1;
                } else {
                    col++;
                }
            }
            end_line = line;
            end_col = col;
            for (uint32_t k = x->span.start; k < x->span.start + x->span.len && fuente[k];
                 k++) {
                if (fuente[k] == '\n') {
                    end_line++;
                    end_col = 1;
                } else {
                    end_col++;
                }
            }
        }
        if (i) fputc(',', out);
        fputs("{\"file\":", out);
        hx_json_str(out, x->file ? x->file : "");
        fprintf(out, ",\"line\":%u,\"col\":%u,\"endLine\":%u,\"endCol\":%u", line, col, end_line,
                end_col);
        fputs(",\"severity\":", out);
        hx_json_str(out, hx_sev_label(x->sev));
        fputs(",\"code\":", out);
        hx_json_str(out, x->code ? x->code : "");
        fputs(",\"message\":", out);
        hx_json_str(out, x->msg ? x->msg : "");
        if (x->note) {
            fputs(",\"note\":", out);
            hx_json_str(out, x->note);
        }
        if (x->help) {
            fputs(",\"help\":", out);
            hx_json_str(out, x->help);
        }
        fputc('}', out);
    }
    fprintf(out, "],\"errors\":%d,\"warnings\":%d}\n", d->errors, d->warnings);
}
