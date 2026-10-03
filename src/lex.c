#include "hx/lex.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *text;
    int kind;
} HxKeywordEntry;

static const HxKeywordEntry hx_keywords[] = {
#define HX_KEYWORD(id, text) {text, TK_KW_##id},
#include "hx/keywords.def"
#undef HX_KEYWORD
    {NULL, 0},
};

static int hx_lookup_keyword(HxSym folded) {
    for (int i = 0; hx_keywords[i].text; i++) {
        if (hx_keywords[i].text[0] == folded[0] && strcmp(hx_keywords[i].text, folded) == 0)
            return hx_keywords[i].kind;
    }
    return TK_IDENT;
}

void hx_lex_init(HxLexer *lx, HxArena *a, HxIntern *intern, HxDiagBag *diags,
                 const char *file, const char *src) {
    memset(lx, 0, sizeof(*lx));
    lx->src = src;
    lx->file = file;
    lx->p = src;
    lx->arena = a;
    lx->intern = intern;
    lx->diags = diags;
    lx->line = 1;
}

static HxSpan hx_span(HxLexer *lx, const char *start) {
    HxSpan s;
    s.start = (uint32_t)(start - lx->src);
    s.len = (uint32_t)(lx->p - start);
    return s;
}

static int hx_is_ident_start(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

static int hx_is_ident_cont(int c) { return hx_is_ident_start(c) || (c >= '0' && c <= '9'); }

static int hx_is_ascii_alpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int hx_is_digit(int c) { return c >= '0' && c <= '9'; }

static int hx_is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; }

static const struct {
    const char *name;
    int64_t scale;
} hx_units[] = {
    {"ns", 1LL},
    {"us", 1000LL},
    {"ms", 1000000LL},
    {"s", 1000000000LL},
    {"m", 60000000000LL},
    {"h", 3600000000000LL},
    {"d", 86400000000000LL},
    {NULL, 0},
};

static void hx_push(HxLexer *lx, HxTokKind kind, HxSpan span) {
    HxToken t;
    memset(&t, 0, sizeof(t));
    t.kind = kind;
    t.span = span;
    HX_VEC_PUSH(lx->tokens, t);
}

static void hx_push_punct_len(HxLexer *lx, const char *s, size_t len) {
    HxSpan sp = {hx_span(lx, s).start, (uint32_t)len};
    HX_VEC_PUSH(lx->tokens, ((HxToken){.kind = TK_PUNCT, .span = sp, .str_raw = s,
                                       .str_len = (int)len}));
}

static void hx_skip_trivia(HxLexer *lx) {
    for (;;) {
        if (hx_is_space((unsigned char)*lx->p)) {
            lx->p++;
            continue;
        }
        if (*lx->p == '\'') {
            while (*lx->p && *lx->p != '\n') lx->p++;
            continue;
        }
        if (*lx->p == '#') {
            while (*lx->p && *lx->p != '\n') lx->p++;
            continue;
        }
        if (lx->p[0] == '/' && lx->p[1] == '/') {
            while (*lx->p && *lx->p != '\n') lx->p++;
            continue;
        }
        if (lx->p[0] == '/' && lx->p[1] == '*') {
            const char *start = lx->p;
            lx->p += 2;
            while (*lx->p && !(lx->p[0] == '*' && lx->p[1] == '/')) lx->p++;
            if (*lx->p) lx->p += 2;
            HX_VEC_PUSH(lx->tokens,
                        ((HxToken){.kind = TK_COMMENT,
                                   .span = hx_span(lx, start),
                                   .str_raw = start + 2,
                                   .str_len = (int)(lx->p - start - 2)}));
            continue;
        }
        break;
    }
}

static int64_t hx_parse_int_text(const char *s, size_t n, int *ok) {
    char buf[96];
    size_t w = 0;
    for (size_t i = 0; i < n && w + 1 < sizeof(buf); i++)
        if (s[i] != '_') buf[w++] = s[i];
    if (w + 1 >= sizeof(buf)) {
        *ok = 0;
        return 0;
    }
    n = w;
    buf[n] = 0;
    char *end = NULL;
    int base = 10;
    const char *q = buf;
    if (n > 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) {
        base = 16;
        q = buf + 2;
    } else if (n > 2 && buf[0] == '0' && (buf[1] == 'o' || buf[1] == 'O')) {
        base = 8;
        q = buf + 2;
    } else if (n > 2 && buf[0] == '0' && (buf[1] == 'b' || buf[1] == 'B')) {
        base = 2;
        q = buf + 2;
    }
    errno = 0;
    long long v = strtoll(q, &end, base);
    while (end && *end == '_') end++;
    *ok = (end && *end == 0 && end != q);
    return (int64_t)v;
}

static double hx_parse_float_text(const char *s, size_t n, int *ok) {
    char buf[96];
    if (n >= sizeof(buf)) {
        *ok = 0;
        return 0;
    }
    size_t w = 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] != '_') buf[w++] = s[i];
    buf[w] = 0;
    char *end = NULL;
    double v = strtod(buf, &end);
    *ok = (end && *end == 0);
    return v;
}

static int hx_try_unit(HxLexer *lx, int64_t *out_ns) {
    static const char *names[] = {"ns", "us", "ms", "s", "m", "h", "d", NULL};
    for (int i = 0; names[i]; i++) {
        size_t n = strlen(names[i]);
        if (strncmp(lx->p, names[i], n) != 0) continue;
        char after = lx->p[n];
        if (hx_is_ident_start((unsigned char)after) || hx_is_ascii_alpha((unsigned char)after))
            continue;
        *out_ns = hx_units[i].scale;
        lx->p += n;
        return 1;
    }
    return 0;
}

void hx_lex(HxLexer *lx) {
    for (;;) {
        hx_skip_trivia(lx);
        const char *start = lx->p;
        unsigned char c = (unsigned char)*lx->p;

        if (c == 0) {
            if (lx->tokens.len > 0) {
                HX_VEC_PUSH(lx->tokens,
                            ((HxToken){.kind = TK_NL, .span = hx_span(lx, start)}));
            }
            hx_push(lx, TK_EOF, hx_span(lx, start));
            return;
        }

        if (c == '\n') {
            lx->p++;
            lx->line++;
            hx_push(lx, TK_NL, hx_span(lx, start));
            continue;
        }

        if (hx_is_ident_start(c)) {
            while (hx_is_ident_cont((unsigned char)*lx->p)) lx->p++;
            HxSpan sp = hx_span(lx, start);
            HxSym sym = hx_intern(lx->intern, start, (size_t)sp.len);
            int kind = hx_lookup_keyword(hx_intern_fold_ascii(lx->intern, start, sp.len));
            if (kind == TK_KW_TRUE || kind == TK_KW_FALSE) {
                HX_VEC_PUSH(lx->tokens, ((HxToken){.kind = kind, .span = sp, .sym = sym,
                                                  .ival = kind == TK_KW_TRUE}));
                continue;
            }
            if (kind != TK_IDENT) {
                HX_VEC_PUSH(lx->tokens, ((HxToken){.kind = (HxTokKind)kind, .span = sp, .sym = sym}));
                continue;
            }
            HX_VEC_PUSH(lx->tokens, ((HxToken){.kind = TK_IDENT, .span = sp, .sym = sym}));
            continue;
        }

        if (hx_is_digit(c) || (c == '.' && hx_is_digit((unsigned char)lx->p[1]))) {
            int is_float = 0;
            if (c == '0' && (lx->p[1] == 'x' || lx->p[1] == 'X' || lx->p[1] == 'o' ||
                             lx->p[1] == 'O' || lx->p[1] == 'b' || lx->p[1] == 'B')) {
                lx->p += 2;
                while (hx_is_ident_cont((unsigned char)*lx->p)) lx->p++;
            } else {
                while (hx_is_digit((unsigned char)*lx->p) || *lx->p == '_') lx->p++;
                if (*lx->p == '.' && hx_is_digit((unsigned char)lx->p[1])) {
                    is_float = 1;
                    lx->p++;
                    while (hx_is_digit((unsigned char)*lx->p) || *lx->p == '_') lx->p++;
                }
                if (*lx->p == 'e' || *lx->p == 'E') {
                    const char *save = lx->p;
                    lx->p++;
                    if (*lx->p == '+' || *lx->p == '-') lx->p++;
                    if (hx_is_digit((unsigned char)*lx->p)) {
                        is_float = 1;
                        while (hx_is_digit((unsigned char)*lx->p)) lx->p++;
                    } else {
                        lx->p = save;
                    }
                }
            }
            HxSpan sp = hx_span(lx, start);
            int64_t ns_scale = 0;
            int has_unit = 0;
            while (hx_try_unit(lx, &ns_scale)) has_unit++;
            if (has_unit) {
                int ok = 1;
                double v = is_float ? hx_parse_float_text(start, sp.len, &ok)
                                    : (double)hx_parse_int_text(start, sp.len, &ok);
                if (!ok) hx_error(lx->diags, sp, "E0101", "literal numérico inválido");
                double total = v * (double)ns_scale;
                for (;;) {
                    const char *rollback = lx->p;
                    if (!hx_is_digit((unsigned char)*lx->p)) break;
                    const char *ns = lx->p;
                    while (hx_is_digit((unsigned char)*lx->p) || *lx->p == '_') lx->p++;
                    int ok2 = 1;
                    double v2 = hx_parse_float_text(ns, (size_t)(lx->p - ns), &ok2);
                    int64_t scale2 = 0;
                    if (!ok2 || !hx_try_unit(lx, &scale2)) {
                        lx->p = rollback;
                        break;
                    }
                    total += v2 * (double)scale2;
                }
                HX_VEC_PUSH(lx->tokens,
                            ((HxToken){.kind = TK_DURATION, .span = hx_span(lx, start),
                                       .fval = total}));
                continue;
            }
            if (is_float) {
                int ok = 1;
                double v = hx_parse_float_text(start, sp.len, &ok);
                if (!ok) hx_error(lx->diags, sp, "E0101", "literal flotante inválido");
                HX_VEC_PUSH(lx->tokens, ((HxToken){.kind = TK_FLOAT, .span = sp, .fval = v}));
            } else {
                int ok = 1;
                int64_t v = hx_parse_int_text(start, sp.len, &ok);
                if (!ok) hx_error(lx->diags, sp, "E0101", "literal entero inválido");
                HX_VEC_PUSH(lx->tokens, ((HxToken){.kind = TK_INT, .span = sp, .ival = v}));
            }
            continue;
        }

        if (c == '"') {
            const char *content_start = lx->p + 1;
            lx->p++;
            while (*lx->p && *lx->p != '"') {
                if (*lx->p == '\\' && lx->p[1]) lx->p++;
                lx->p++;
            }
            const char *end = lx->p;
            int cerrada = *lx->p == '"';
            if (cerrada) lx->p++;
            if (!cerrada)
                hx_error(lx->diags, hx_span(lx, start), "E0103",
                         "cadena sin cerrar hasta el final del archivo");
            HX_VEC_PUSH(lx->tokens,
                        ((HxToken){.kind = TK_STRING,
                                   .span = hx_span(lx, start),
                                   .str_raw = content_start,
                                   .str_len = (int)(end - content_start)}));
            continue;
        }

        {
            static const char *puncts[] = {
                "<->", "**", "+%", "-%", "*%", "+|", "-|", "*|", "<>", "!=", "==",
                "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "++", "..", "->",
                "+", "-", "*", "/", "<", ">", "=", "(", ")", "[", "]", "{", "}",
                ",", ".", ":", ";", "?", "&", "|", "^", "~", "@", "$", "#", "!",
                NULL};
            int matched = 0;
            for (int i = 0; puncts[i]; i++) {
                size_t n = strlen(puncts[i]);
                if (strncmp(lx->p, puncts[i], n) == 0) {
                    const char *s0 = lx->p;
                    lx->p += n;
                    hx_push_punct_len(lx, s0, n);
                    matched = 1;
                    break;
                }
            }
            if (matched) continue;
            lx->p++;
            hx_error(lx->diags, hx_span(lx, start), "E0102", "carácter inesperado '%c'", c);
            hx_push_punct_len(lx, start, 1);
        }
    }
}

const char *hx_tok_kind_name(HxTokKind k) {
    if (k == TK_EOF) return "fin de archivo";
    if (k == TK_NL) return "fin de línea";
    if (k == TK_IDENT) return "identificador";
    if (k == TK_INT) return "entero";
    if (k == TK_FLOAT) return "flotante";
    if (k == TK_DURATION) return "duración";
    if (k == TK_STRING) return "cadena";
    if (k == TK_COMMENT) return "comentario";
    for (int i = 0; hx_keywords[i].text; i++)
        if (hx_keywords[i].kind == (int)k) return hx_keywords[i].text;
    if (k == TK_PUNCT) return "operador";
    return "token";
}

const char *hx_tok_text(HxTokKind k) {
    for (int i = 0; hx_keywords[i].text; i++)
        if (hx_keywords[i].kind == (int)k) return hx_keywords[i].text;
    return NULL;
}

int hx_tok_is_kw(HxTokKind k) { return k >= TK_KW_FIRST && k <= TK_KW_LAST; }

int hx_tok_is_eof(HxTokKind k) { return k == TK_EOF; }