#ifndef HX_AST_H
#define HX_AST_H

#include "hx/common.h"
#include "hx/diag.h"

typedef enum {
    TY_UNKNOWN,
    TY_VOID,
    TY_BOOL,
    TY_INT,
    TY_I64,
    TY_FLOAT,
    TY_STRING,
    TY_DURATION,
    TY_NAMED,
    TY_ARRAY,
    TY_REF,
    TY_PTR
} HxTyKind;

typedef struct HxTypeDecl HxTypeDecl;

typedef struct HxTy {
    HxTyKind kind;
    HxSym name;
    struct HxTy *elem;
    struct HxTy *inner;
    int64_t size;
    HxTypeDecl *decl;
} HxTy;

typedef enum {
    EX_INT,
    EX_FLOAT,
    EX_STR,
    EX_DURATION,
    EX_BOOL,
    EX_NIL,
    EX_PATH,
    EX_CALL,
    EX_BIN,
    EX_UN,
    EX_INDEX,
    EX_MEMBER,
    EX_TRY,
    EX_VEC,
    EX_FIELD_ACCESS
} HxExprKind;

typedef enum {
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_CONCAT,
    OP_ADDW,
    OP_SUBW,
    OP_ADDS,
    OP_SUBS,
    OP_MULS,
    OP_EQ,
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_AND,
    OP_OR,
    OP_XOR
} HxBinOp;

typedef enum { UOP_NEG, UOP_NOT } HxUnOp;

typedef struct HxExpr HxExpr;
typedef struct HxStmt HxStmt;

typedef struct {
    const char *text;
    int len;
    HxExpr *hole;
} HxStrSeg;

typedef struct {
    HxSym name;
    HxExpr *value;
    HxSpan span;
    HxSpan name_span;
} HxArg;

typedef struct {
    HxSym name;
    HxSpan span;
} HxPathPart;

struct HxExpr {
    HxExprKind kind;
    HxSpan span;
    HxTy *ty;
    HxStrSeg *segs;
    int n_segs;
    int has_holes;
    int is_ok_ctor;
    int is_err_ctor;
    int propagate;
    HxTy *payload_ty;
    int prefix_len;
    HxSym method;
    int is_intrin;
    HxExpr *recv;
    union {
        int64_t ival;
        double fval;
        struct {
            const char *raw;
            int len;
        } str;
        struct {
            HxExpr *base;
            HX_VEC_ANON(HxPathPart) parts;
        } path;
        struct {
            HxExpr *callee;
            HX_VEC_ANON(HxArg) args;
        } call;
        struct {
            HxBinOp op;
            HxExpr *lhs;
            HxExpr *rhs;
        } bin;
        struct {
            HxUnOp op;
            HxExpr *operand;
        } un;
        struct {
            HxExpr *base;
            HxExpr *start;
            HxExpr *end;
        } index;
        struct {
            HxExpr *base;
            HxSym name;
            HxSpan name_span;
        } member;
        struct {
            HxExpr *inner;
        } try;
        struct {
            HxExpr **items;
            int len;
        } vec;
    };
};

typedef struct {
    HxSym name;
    HxTy *ty;
    HxSpan span;
    int is_ref;
    HxExpr *default_value;
} HxParam;

typedef struct {
    HxSym name;
    HxTy *ty;
    HxSpan span;
    int is_unique;
} HxField;

struct HxTypeDecl {
    HxSym name;
    HX_VEC_ANON(HxField) fields;
    HxSpan span;
    int is_export;
    int index;
};

struct HxStmt;
typedef struct {
    struct HxStmt *data;
    int len;
    int cap;
} HxStmtVec;

typedef struct {
    HxSym name;
    HX_VEC_ANON(HxParam) params;
    HxTy *ret;
    HxStmtVec body;
    HxSpan span;
    HxSpan name_span;
    int is_export;
    int index;
    int is_comptime_only;
    int is_tail_loop;
} HxFunc;

typedef struct {
    HxSym path;
    HxSym alias;
    HxSpan span;
    HxSpan path_span;
    const char *resolved_file;
} HxImport;

typedef struct {
    HxSym name;
    HxExpr *value;
    HxTy *ty;
    HxSpan span;
} HxConst;

typedef enum {
    ST_EXPR,
    ST_ASSIGN,
    ST_DIM,
    ST_PRINT,
    ST_IF,
    ST_WHILE,
    ST_FOR,
    ST_RETURN,
    ST_BREAK,
    ST_CONTINUE,
    ST_EXIT,
    ST_BLOCK,
    ST_ARENA,
    ST_DEFER,
    ST_CONST,
    ST_NOP,
    ST_MATCH
} HxStmtKind;

typedef enum { PS_EXPR, PS_SEP_SEMI, PS_SEP_COMMA } HxPrintSep;

typedef enum {
    PAT_LITERAL,
    PAT_BIND,
    PAT_CONSTRUCTOR,
    PAT_WILDCARD,
    PAT_RANGE
} HxPatKind;

typedef struct HxPattern HxPattern;

struct HxPattern {
    HxPatKind kind;
    HxSym name;
    HxExpr *lit;
    HxSym ctor;
    HX_VEC_ANON(HxPattern) args;
    HxExpr *lo;
    HxExpr *hi;
    HxSpan span;
};

typedef struct {
    HxPattern *pattern;
    HxExpr *guard;
    HxStmtVec body;
} HxMatchCase;

typedef struct {
    HxExpr *subject;
    HxSym subject_name;
    HX_VEC_ANON(HxMatchCase) cases;
    HxStmtVec else_body;
    int has_else;
} HxMatch;

typedef struct {
    HxExpr *expr;
    HxPrintSep sep;
} HxPrintItem;

typedef struct {
    HxSym name;
    HxTy *ty;
    HxExpr *value;
    HxSpan span;
    HxSpan name_span;
    int is_const;
} HxBind;

HX_VEC_T(HxBindVec, HxBind);

struct HxStmt {
    HxStmtKind kind;
    HxSpan span;
    union {
        HxExpr *expr;
        struct {
            HxExpr *target;
            HxBinOp op;
            HxExpr *value;
            int compound;
        } assign;
        struct {
            HxSym name;
            HxTy *ty;
            HxExpr *init;
            HxSpan name_span;
            int is_const;
        } dim;
        struct {
            HX_VEC_ANON(HxPrintItem) items;
        } print;
        struct {
            HxExpr *cond;
            HxStmtVec then;
            struct {
                HxExpr *cond;
                HxStmtVec then;
            } elifs[16];
            int n_elifs;
            HxStmtVec else_;
            int has_else;
        } if_;
        struct {
            HxExpr *cond;
            HxStmtVec body;
        } while_;
        struct {
            HxSym var;
            HxExpr *start;
            HxExpr *end;
            HxExpr *step;
            HxStmtVec body;
            HxSpan var_span;
        } for_;
        struct {
            HxExpr *value;
        } ret;
        struct {
            HxExpr *code;
        } exit_;
        struct {
            HxStmtVec stmts;
        } block;
        struct {
            HxSym name;
            HxStmtVec body;
        } arena;
        HxStmtVec inner;
        struct {
            HxSym name;
            HxExpr *value;
            HxTy *ty;
            HxSpan name_span;
        } konst;
        HxMatch match;
    };
};


typedef struct HxModule {
    HxSym name;
    const char *file;
    const char *src;
    HX_VEC_ANON(HxImport) imports;
    HX_VEC_ANON(HxFunc) funcs;
    HX_VEC_ANON(HxTypeDecl) types;
    HX_VEC_ANON(HxConst) consts;
    HxStmtVec top;
    int is_entry;
    int index;
} HxModule;

typedef struct {
    HxArena *arena;
    HxIntern *intern;
    HxDiagBag *diags;
    HX_VEC_ANON(HxModule) modules;
} HxUnit;

HxTy *hx_ty_new(HxArena *a, HxTyKind kind);
HxTy *hx_ty_builtin(HxArena *a, HxTyKind kind);
const char *hx_ty_name(const HxTy *t);
int hx_ty_equal(const HxTy *a, const HxTy *b);
HxTy *hx_ty_lookup_builtin(HxArena *a, HxSym name);
int hx_ty_is_numeric(const HxTy *t);
int hx_ty_is_result(const HxTy *t);
int hx_ty_is_scalar(const HxTy *t);
int hx_ty_rank(const HxTy *t);
const char *hx_binop_symbol(HxBinOp op);
const char *hx_binop_spelling(HxBinOp op);

#endif