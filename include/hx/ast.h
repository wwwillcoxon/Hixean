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
    TY_PTR,
    TY_VEC2,
    TY_VEC3,
    TY_VEC4,
    TY_MAT4,
    TY_QUAT,
    TY_ITER /* ITER<T>: elem es el tipo del elemento */
} HxTyKind;

typedef struct HxTypeDecl HxTypeDecl;

typedef struct HxTy {
    HxTyKind kind;
    HxSym name;
    struct HxTy *elem;
    struct HxTy *inner;
    int64_t size;
    int n_targs;
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
    EX_MEMB,
    EX_DEREF, /* p^ */
    EX_FUNC /* FUNC(...) ... END: se eleva a una funcion del modulo */
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

typedef enum { UOP_NEG, UOP_NOT, UOP_ADDR } HxUnOp;

typedef struct HxExpr HxExpr;

typedef struct HxFunc HxFunc;

#define HX_MAX_TPARAMS 8

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
    int deref;
    int vec_component;
    int *ret_arg_refs;
    int n_arg_refs;
    HxTy *payload_ty;
    int prefix_len;
    HxSym method;
    int is_intrin;
    HxExpr *recv;
    struct HxFunc *fn;
    struct HxFunc *lit; /* EX_FUNC: la funcion anonima */
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
    int is_enum; /* ENUM: sus campos son variantes y valen 0, 1, 2... */
    HxSym tparams[HX_MAX_TPARAMS];
    int n_tparams;
    int module;
    int is_generic;
    int is_instance;
    int is_placeholder;
    int is_export;
    int index;
};

struct HxStmt;
typedef struct {
    struct HxStmt *data;
    int len;
    int cap;
} HxStmtVec;

struct HxFunc {
    HxSym name;
    HxSym orig_name; /* nombre tal y como se escribio, antes de manglear */
    HX_VEC_ANON(HxParam) params;
    HxTy *ret;
    HxStmtVec body;
    HxSpan span;
    HxSpan name_span;
    HxSym tparams[HX_MAX_TPARAMS];
    int n_tparams;
    /* DONDE T: Trait, U: Trait -- se comprueba en cada instancia */
    HxSym constrained[HX_MAX_TPARAMS];
    HxSym ctraits[HX_MAX_TPARAMS];
    int n_constraints;
    int module;
    int is_generic;
    int is_instance;
    int is_placeholder;
    int is_comptime_only;
    int is_export;
    int index;
    int is_tail_loop;
};

typedef struct {
    HxSym path;
    HxSym alias;
    HxSpan span;
    HxSpan path_span;
    const char *resolved_file;
} HxImport;

/* TRAIT T: METHOD f(a AS T, ...) AS R ; ... END TRAIT
   IMPLEMENTAR <tipo> PARA <trait> : METHOD ... END METHOD  */
typedef struct {
    HxSym name;
    HX_VEC_ANON(HxSym) methods;
    HxSpan span;
    int module;
    int is_export;
} HxTraitDecl;

typedef struct {
    HxSym type_name;
    HxSym trait_name;
    HX_VEC_ANON(HxFunc) methods;
    HxSpan span;
    int module;
    int export_type;
} HxImplDecl;

typedef struct {
    HxSym name;
    HxExpr *value;
    HxTy *ty;
    HxSpan span;
    int is_export;
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
    ST_MATCH,
    ST_FORIN
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
        /* FOR x IN expr: la variable, la cadena y el cuerpo */
        struct {
            HxSym var;
            HxSpan var_span;
            HxExpr *iter;
            HxStmtVec body;
            int is_arena;
            int depth;
        } forin_;
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
            HxExpr *iter;
            HxExpr *init; /* inicializacion perezosa de la cadena, o NULL */
        } forin;
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
    int deferred;
    int is_export;
};


typedef struct HxModule {
    HxSym name;
    const char *file;
    const char *src;
    HX_VEC_ANON(HxImport) imports;
    HX_VEC_ANON(HxTraitDecl) traits;
    HX_VEC_ANON(HxImplDecl) impls;
    HX_VEC_ANON(HxFunc) funcs;
    HX_VEC_ANON(HxTypeDecl) types;
    HX_VEC_ANON(HxConst) consts;
    HxStmtVec top;
    int is_entry;
    int from_hxc;
    int index;
} HxModule;

typedef struct {
    HxArena *arena;
    HxIntern *intern;
    HxDiagBag *diags;
    HX_VEC_ANON(HxModule) modules;
    /* instancias monomorfizadas de funciones genericas, una por combinacion
       de argumentos de tipo que aparece en el programa. Son punteros porque
       el vector crece y los puntos de llamada guardan la referencia. */
    HxFunc **instances;
    int n_instances;
    int cap_instances;
    /* funciones anonimas (FUNC ... END) elevadas a funciones del modulo */
    HxFunc **lambdas;
    int n_lambdas;
    int cap_lambdas;
    HxTypeDecl **type_instances;
    int n_type_instances;
    int cap_type_instances;
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