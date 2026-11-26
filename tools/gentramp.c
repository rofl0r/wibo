/*
 * gentramp: generates guest<->host calling-convention thunks for the wibo DLL shims.
 *
 * Replaces tools/gen_trampolines.py, which used libclang. Inputs are C headers. Only
 * declarations annotated with _CC_STDCALL / _CC_CDECL (annotate("CC:...")) produce thunks.
 * Function-pointer typedefs with a CC annotation produce call_<Typedef> host-to-guest thunks.
 * WINAPI/CDECL also add x86 fastcall/cdecl attributes, which select the host-side convention.
 * Extern variables are listed in <dll>ThunkByName.
 *
 * The headers are preprocessed as one translation unit by running `$(CC) -E` (the --cc
 * command plus -I/-D flags) with the TU on stdin. A small declaration parser then reads
 * the result. Declarations it cannot parse are skipped, unless they carry an annotation,
 * in which case generation fails.
 *
 * Export names: if a C identifier starts with "<dll>_", that prefix is stripped to get the
 * Win32 export name. The thunk symbol is thunk_<dll>_<export> and the ByName string is
 * <export>, while the host call uses the C identifier. So ws2_select is exported as "select"
 * via thunk_ws2_select, and entry_stubBase is exported as "stubBase" via thunk_entry_stubBase.
 * Identifiers without the prefix are exported unchanged. Typedef names are not remapped.
 *
 * --guest-abi win64 (x86_64 only) instead emits ms_abi guest -> SysV host thunks named
 * thunk_<dll>_<export>, plus <dll>ThunkByName in the header. Win64 guests run native 64-bit code,
 * so no mode or segment switching is needed. Host-to-guest typedefs are not generated in this mode.
 */

#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum { ARCH_X86, ARCH_X86_64 } Arch;

typedef enum { CC_DEFAULT, CC_C, CC_STDCALL, CC_FASTCALL } CallConv;

static Arch g_arch;
static int g_win64;
static const char *g_dll;
static int g_darwin;
static long g_ptrSize;
static long g_longSize;

static const char *ccName(CallConv cc) {
    switch (cc) {
    case CC_DEFAULT:
        return "DEFAULT";
    case CC_C:
        return "C";
    case CC_STDCALL:
        return "X86_STDCALL";
    case CC_FASTCALL:
        return "X86_FASTCALL";
    }
    return "?";
}

static void fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void fatal(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "gentramp: error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

/* Growable output buffer. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

static void bufVprintf(Buf *b, const char *fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0)
        fatal("formatting output failed");
    if (b->len + (size_t)n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + (size_t)n + 1)
            cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p)
            fatal("out of memory");
        b->data = p;
        b->cap = cap;
    }
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

static void bprintf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bprintf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    bufVprintf(b, fmt, ap);
    va_end(ap);
}

/* Appends a formatted line followed by a newline. */
static void bline(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bline(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    bufVprintf(b, fmt, ap);
    va_end(ap);
    bprintf(b, "\n");
}

/* ---------- Tokens ---------- */

enum { TK_IDENT, TK_NUM, TK_STR, TK_PUNCT, TK_EOF };

typedef struct {
    int kind;
    char *text;
} Tok;

static Tok *g_toks;
static size_t g_ntoks;
static size_t g_pos;

static char *copyText(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (!p)
        fatal("out of memory");
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static void lexSource(const char *src, size_t len) {
    size_t cap = 1024;
    size_t n = 0;
    size_t i = 0;
    int bol = 1;
    g_toks = malloc(cap * sizeof *g_toks);
    if (!g_toks)
        fatal("out of memory");
    while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\n') {
            bol = 1;
            i++;
            continue;
        }
        if (isspace(c)) {
            i++;
            continue;
        }
        if (bol && c == '#') {
            while (i < len && src[i] != '\n')
                i++;
            continue;
        }
        bol = 0;
        size_t start = i;
        int kind;
        if (isalpha(c) || c == '_') {
            kind = TK_IDENT;
            while (i < len && (isalnum((unsigned char)src[i]) || src[i] == '_'))
                i++;
        } else if (isdigit(c) || (c == '.' && i + 1 < len && isdigit((unsigned char)src[i + 1]))) {
            kind = TK_NUM;
            while (i < len) {
                unsigned char d = (unsigned char)src[i];
                int exponentSign = (d == '+' || d == '-') && i > start && strchr("eEpP", src[i - 1]) != NULL;
                if (!(isalnum(d) || d == '_' || d == '.' || exponentSign))
                    break;
                i++;
            }
        } else if (c == '"' || c == '\'') {
            kind = TK_STR;
            i++;
            while (i < len && src[i] != (char)c && src[i] != '\n') {
                if (src[i] == '\\')
                    i++;
                i++;
            }
            if (i < len && src[i] == (char)c)
                i++;
        } else if (c == '.' && i + 2 < len && src[i + 1] == '.' && src[i + 2] == '.') {
            kind = TK_PUNCT;
            i += 3;
        } else {
            kind = TK_PUNCT;
            i++;
        }
        if (i > len)
            i = len;
        if (n + 1 >= cap) {
            cap *= 2;
            g_toks = realloc(g_toks, cap * sizeof *g_toks);
            if (!g_toks)
                fatal("out of memory");
        }
        g_toks[n].kind = kind;
        g_toks[n].text = copyText(src + start, i - start);
        n++;
    }
    g_toks[n].kind = TK_EOF;
    g_toks[n].text = copyText("", 0);
    g_ntoks = n + 1;
}

/* ---------- Types ---------- */

typedef enum {
    TY_VOID,
    TY_BOOL,
    TY_CHAR,
    TY_SCHAR,
    TY_UCHAR,
    TY_SHORT,
    TY_USHORT,
    TY_INT,
    TY_UINT,
    TY_LONG,
    TY_ULONG,
    TY_LLONG,
    TY_ULLONG,
    TY_INT128,
    TY_UINT128,
    TY_FLOAT,
    TY_DOUBLE,
    TY_LDOUBLE,
    TY_ENUM,
    TY_PTR,
    TY_ARRAY,
    TY_FUNC,
    TY_RECORD,
    TY_UNKNOWN
} TyKind;

typedef struct Ty Ty;
struct Ty {
    TyKind kind;
    long size; /* bytes; -1 unknown, -2 incomplete (void) */
    int isConst;
    int handle;      /* a typedef in the chain is HANDLE-like (see isHandleName) */
    int viaTypedef;  /* reached through the typedef name `name` */
    const char *name;
    Ty *inner; /* pointee, element type, or FUNC return type */
    Ty **params;
    int nparams;
    int variadic;
};

static long builtinSize(TyKind k) {
    switch (k) {
    case TY_VOID:
        return -2;
    case TY_BOOL:
    case TY_CHAR:
    case TY_SCHAR:
    case TY_UCHAR:
        return 1;
    case TY_SHORT:
    case TY_USHORT:
        return 2;
    case TY_INT:
    case TY_UINT:
    case TY_FLOAT:
    case TY_ENUM:
        return 4;
    case TY_LONG:
    case TY_ULONG:
        return g_longSize;
    case TY_LLONG:
    case TY_ULLONG:
    case TY_DOUBLE:
        return 8;
    case TY_INT128:
    case TY_UINT128:
        return 16;
    case TY_LDOUBLE:
        return g_arch == ARCH_X86 ? 12 : 16;
    case TY_PTR:
        return g_ptrSize;
    default:
        return -1;
    }
}

static const char *builtinName(TyKind k) {
    switch (k) {
    case TY_VOID:
        return "void";
    case TY_BOOL:
        return "_Bool";
    case TY_CHAR:
        return "char";
    case TY_SCHAR:
        return "signed char";
    case TY_UCHAR:
        return "unsigned char";
    case TY_SHORT:
        return "short";
    case TY_USHORT:
        return "unsigned short";
    case TY_INT:
        return "int";
    case TY_UINT:
        return "unsigned int";
    case TY_LONG:
        return "long";
    case TY_ULONG:
        return "unsigned long";
    case TY_LLONG:
        return "long long";
    case TY_ULLONG:
        return "unsigned long long";
    case TY_INT128:
        return "__int128";
    case TY_UINT128:
        return "unsigned __int128";
    case TY_FLOAT:
        return "float";
    case TY_DOUBLE:
        return "double";
    case TY_LDOUBLE:
        return "long double";
    default:
        return NULL;
    }
}

static Ty *newTy(TyKind kind) {
    Ty *t = calloc(1, sizeof *t);
    if (!t)
        fatal("out of memory");
    t->kind = kind;
    t->size = builtinSize(kind);
    return t;
}

static Ty *copyTy(const Ty *t) {
    Ty *c = malloc(sizeof *c);
    if (!c)
        fatal("out of memory");
    *c = *t;
    return c;
}

static Ty *withConst(Ty *t) {
    if (t->isConst)
        return t;
    Ty *c = copyTy(t);
    c->isConst = 1;
    return c;
}

static Ty *stripConst(Ty *t) {
    if (!t->isConst)
        return t;
    Ty *c = copyTy(t);
    c->isConst = 0;
    return c;
}

static Ty *ptrTo(Ty *inner) {
    Ty *p = newTy(TY_PTR);
    p->inner = inner;
    return p;
}

/* Matches the Python generator's HANDLE heuristic: an all-uppercase name starting with 'H'. */
static int isHandleName(const char *s) {
    if (s[0] != 'H')
        return 0;
    for (; *s; s++) {
        if (islower((unsigned char)*s))
            return 0;
    }
    return 1;
}

static int isOpaque(const Ty *t) {
    if (t->kind == TY_RECORD || t->kind == TY_ENUM || t->kind == TY_FUNC)
        return 1;
    if (t->kind == TY_PTR)
        return isOpaque(t->inner);
    return 0;
}

static int signExtended(const Ty *t) {
    switch (t->kind) {
    case TY_SCHAR:
    case TY_CHAR:
    case TY_SHORT:
    case TY_INT:
    case TY_LONG:
    case TY_LLONG:
    case TY_INT128:
        return 1;
    default:
        return t->handle;
    }
}

static void writeCanonical(Buf *b, const Ty *t) {
    if (t->kind == TY_PTR) {
        writeCanonical(b, t->inner);
        if (t->isConst)
            bprintf(b, " *const");
        else
            bprintf(b, " *");
        return;
    }
    const char *name = builtinName(t->kind);
    if (!name)
        fatal("unsupported type in generated header");
    if (t->isConst)
        bprintf(b, "const ");
    bprintf(b, "%s", name);
}

static void writeSpelled(Buf *b, const Ty *t) {
    if (t->viaTypedef && t->name) {
        if (t->isConst)
            bprintf(b, "const ");
        bprintf(b, "%s", t->name);
        return;
    }
    if (t->kind == TY_PTR) {
        writeSpelled(b, t->inner);
        bprintf(b, " *");
        return;
    }
    if ((t->kind == TY_RECORD || t->kind == TY_ENUM || t->kind == TY_UNKNOWN) && t->name) {
        if (t->isConst)
            bprintf(b, "const ");
        bprintf(b, "%s", t->name);
        return;
    }
    const char *name = builtinName(t->kind);
    if (!name)
        fatal("unsupported type in generated header");
    if (t->isConst)
        bprintf(b, "const ");
    bprintf(b, "%s", name);
}

/* Canonical spelling for guest-to-host prototypes; opaque types become void *. */
static void writeTypeForHeader(Buf *b, const Ty *t, int canonical) {
    if (!canonical) {
        writeSpelled(b, t);
    } else if (isOpaque(t)) {
        bprintf(b, "void *");
    } else {
        writeCanonical(b, t);
    }
}

/* ---------- Typedef table ---------- */

typedef struct TdEntry {
    const char *name;
    Ty *ty;
    struct TdEntry *next;
} TdEntry;

#define kTdBuckets 4096
static TdEntry *g_tdBuckets[kTdBuckets];

static unsigned hashName(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h % kTdBuckets;
}

static Ty *tdLookup(const char *name) {
    for (TdEntry *e = g_tdBuckets[hashName(name)]; e; e = e->next) {
        if (!strcmp(e->name, name))
            return e->ty;
    }
    return NULL;
}

static void tdAdd(const char *name, Ty *ty) {
    unsigned h = hashName(name);
    for (TdEntry *e = g_tdBuckets[h]; e; e = e->next) {
        if (!strcmp(e->name, name)) {
            e->ty = ty;
            return;
        }
    }
    TdEntry *e = malloc(sizeof *e);
    if (!e)
        fatal("out of memory");
    e->name = name;
    e->ty = ty;
    e->next = g_tdBuckets[h];
    g_tdBuckets[h] = e;
}

/* ---------- Parser ---------- */

static jmp_buf g_recover;
static char g_errmsg[256];

static void fail(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_errmsg, sizeof g_errmsg, fmt, ap);
    va_end(ap);
    longjmp(g_recover, 1);
}

/* Attributes gathered for the declaration being parsed. Parameter lists use a discarded sink. */
typedef struct {
    CallConv annot;
    int fastcall;
    int stdcall;
    int cdecl_;
} Attrs;

static Attrs g_declAttrs;
static Attrs g_paramAttrs;
static Attrs *g_sink = &g_declAttrs;

typedef struct {
    int isTypedef;
    int isExtern;
    Ty *base;
} DeclSpec;

static const Tok *cur(void) {
    return g_pos < g_ntoks ? &g_toks[g_pos] : &g_toks[g_ntoks - 1];
}

static int tokIs(const char *s) {
    return strcmp(cur()->text, s) == 0;
}

static int tokIsAt(size_t i, const char *s) {
    return i < g_ntoks && strcmp(g_toks[i].text, s) == 0;
}

static void expectTok(const char *s) {
    if (!tokIs(s))
        fail("expected '%s' before '%s'", s, cur()->text);
    g_pos++;
}

static int isAttrKw(const Tok *t) {
    return t->kind == TK_IDENT && (!strcmp(t->text, "__attribute__") || !strcmp(t->text, "__attribute"));
}

static int isStorageKw(const char *s) {
    static const char *const kWords[] = {"static",     "inline",  "__inline", "__inline__", "__forceinline",
                                         "_Noreturn",  "auto",    "register", "__extension__", "_Thread_local",
                                         "__thread"};
    for (size_t i = 0; i < sizeof kWords / sizeof kWords[0]; i++) {
        if (!strcmp(s, kWords[i]))
            return 1;
    }
    return 0;
}

static int isQualKw(const char *s) {
    static const char *const kWords[] = {"volatile", "__volatile", "__volatile__", "restrict",
                                         "__restrict", "__restrict__", "_Atomic"};
    for (size_t i = 0; i < sizeof kWords / sizeof kWords[0]; i++) {
        if (!strcmp(s, kWords[i]))
            return 1;
    }
    return 0;
}

static int isTypeKw(const char *s) {
    static const char *const kWords[] = {"void",     "char",     "short",      "int",       "long",
                                         "float",    "double",   "signed",     "unsigned",  "_Bool",
                                         "bool",     "struct",   "union",      "enum",      "__int128",
                                         "__int128_t", "_Complex", "__complex__", "__signed__", "__signed",
                                         "typeof",   "__typeof__", "const",    "__const"};
    for (size_t i = 0; i < sizeof kWords / sizeof kWords[0]; i++) {
        if (!strcmp(s, kWords[i]))
            return 1;
    }
    return isStorageKw(s) || isQualKw(s);
}

static void skipBalanced(void) {
    const Tok *open = cur();
    if (open->kind != TK_PUNCT || !strchr("([{", open->text[0]))
        fail("expected an opening bracket before '%s'", open->text);
    int depth = 0;
    do {
        const Tok *t = cur();
        if (t->kind == TK_EOF)
            fail("unbalanced brackets");
        if (t->kind == TK_PUNCT) {
            char c = t->text[0];
            if (c == '(' || c == '[' || c == '{')
                depth++;
            else if (c == ')' || c == ']' || c == '}')
                depth--;
        }
        g_pos++;
    } while (depth > 0);
}

static int strEqQuoted(const char *s, const char *want) {
    size_t n = strlen(want);
    return s[0] == '"' && strlen(s) == n + 2 && !strncmp(s + 1, want, n);
}

static void setAnnot(CallConv cc) {
    if (g_sink->annot == CC_DEFAULT)
        g_sink->annot = cc;
}

/* Parses an __attribute__((...)) group at the current position, recording CC annotations and
 * x86 calling-convention attributes into g_sink. */
static void parseAttributes(void) {
    g_pos++;
    if (!tokIs("("))
        fail("expected '(' after __attribute__");
    size_t start = g_pos;
    skipBalanced();
    for (size_t i = start; i + 1 < g_pos; i++) {
        const Tok *t = &g_toks[i];
        if (t->kind != TK_IDENT)
            continue;
        if (!strcmp(t->text, "annotate") && tokIsAt(i + 1, "(") && i + 2 < g_pos && g_toks[i + 2].kind == TK_STR) {
            const char *s = g_toks[i + 2].text;
            if (strEqQuoted(s, "CC:stdcall"))
                setAnnot(CC_STDCALL);
            else if (strEqQuoted(s, "CC:fastcall"))
                setAnnot(CC_FASTCALL);
            else if (strEqQuoted(s, "CC:cdecl"))
                setAnnot(CC_C);
        } else if (!strcmp(t->text, "fastcall") || !strcmp(t->text, "__fastcall") ||
                   !strcmp(t->text, "__fastcall__")) {
            g_sink->fastcall = 1;
        } else if (!strcmp(t->text, "stdcall") || !strcmp(t->text, "__stdcall") ||
                   !strcmp(t->text, "__stdcall__")) {
            g_sink->stdcall = 1;
        } else if (!strcmp(t->text, "cdecl") || !strcmp(t->text, "__cdecl") || !strcmp(t->text, "__cdecl__")) {
            g_sink->cdecl_ = 1;
        }
    }
}

static void parseTrailing(void) {
    for (;;) {
        if (isAttrKw(cur())) {
            parseAttributes();
        } else if (tokIs("__asm__") || tokIs("__asm") || tokIs("asm")) {
            g_pos++;
            if (tokIs("("))
                skipBalanced();
        } else {
            break;
        }
    }
}

static Ty *typedefUse(const char *name) {
    const Ty *orig = tdLookup(name);
    Ty *t = copyTy(orig);
    t->viaTypedef = 1;
    t->name = name;
    t->handle = isHandleName(name) || orig->handle;
    return t;
}

static Ty *parseTagSpec(void) {
    int isEnum = !strcmp(cur()->text, "enum");
    g_pos++;
    while (isAttrKw(cur()))
        parseAttributes();
    const char *name = NULL;
    if (cur()->kind == TK_IDENT) {
        name = cur()->text;
        g_pos++;
    }
    while (isAttrKw(cur()))
        parseAttributes();
    if (tokIs("{"))
        skipBalanced();
    Ty *t = newTy(isEnum ? TY_ENUM : TY_RECORD);
    t->name = name;
    return t;
}

static void parseDeclSpecs(DeclSpec *ds) {
    int nLong = 0;
    int isShort = 0;
    int isSigned = 0;
    int isUnsigned = 0;
    int isConst = 0;
    int sawInt = 0;
    int isComplex = 0;
    TyKind kw = TY_UNKNOWN;
    Ty *named = NULL;
    memset(ds, 0, sizeof *ds);
    for (;;) {
        const Tok *t = cur();
        if (t->kind != TK_IDENT)
            break;
        const char *s = t->text;
        int haveType = named || kw != TY_UNKNOWN || nLong || isShort || isSigned || isUnsigned || sawInt ||
                       isComplex;
        if (isAttrKw(t)) {
            parseAttributes();
            continue;
        }
        if (!strcmp(s, "struct") || !strcmp(s, "union") || !strcmp(s, "enum")) {
            if (named)
                break;
            named = parseTagSpec();
            continue;
        }
        if (!strcmp(s, "__declspec")) {
            g_pos++;
            if (tokIs("("))
                skipBalanced();
            continue;
        }
        if (!strcmp(s, "typedef")) {
            ds->isTypedef = 1;
        } else if (!strcmp(s, "extern")) {
            ds->isExtern = 1;
        } else if (isStorageKw(s)) {
        } else if (!strcmp(s, "const") || !strcmp(s, "__const")) {
            isConst = 1;
        } else if (isQualKw(s)) {
        } else if (!strcmp(s, "void")) {
            kw = TY_VOID;
        } else if (!strcmp(s, "char")) {
            kw = TY_CHAR;
        } else if (!strcmp(s, "short")) {
            isShort = 1;
        } else if (!strcmp(s, "int")) {
            sawInt = 1;
        } else if (!strcmp(s, "long")) {
            nLong++;
        } else if (!strcmp(s, "float")) {
            kw = TY_FLOAT;
        } else if (!strcmp(s, "double")) {
            kw = TY_DOUBLE;
        } else if (!strcmp(s, "_Bool") || !strcmp(s, "bool")) {
            kw = TY_BOOL;
        } else if (!strcmp(s, "__int128") || !strcmp(s, "__int128_t")) {
            kw = TY_INT128;
        } else if (!strcmp(s, "signed") || !strcmp(s, "__signed__") || !strcmp(s, "__signed")) {
            isSigned = 1;
        } else if (!strcmp(s, "unsigned")) {
            isUnsigned = 1;
        } else if (!strcmp(s, "_Complex") || !strcmp(s, "__complex__")) {
            isComplex = 1;
        } else if (!haveType && tdLookup(s)) {
            named = typedefUse(s);
        } else if (!haveType && !isTypeKw(s)) {
            named = newTy(TY_UNKNOWN);
            named->name = s;
        } else {
            break;
        }
        g_pos++;
    }

    Ty *base;
    if (named) {
        base = named;
    } else if (isComplex) {
        base = newTy(TY_UNKNOWN);
        base->name = "_Complex";
    } else {
        TyKind k;
        if (kw == TY_VOID)
            k = TY_VOID;
        else if (kw == TY_BOOL)
            k = TY_BOOL;
        else if (kw == TY_FLOAT)
            k = TY_FLOAT;
        else if (kw == TY_DOUBLE)
            k = nLong ? TY_LDOUBLE : TY_DOUBLE;
        else if (kw == TY_INT128)
            k = isUnsigned ? TY_UINT128 : TY_INT128;
        else if (kw == TY_CHAR)
            k = isUnsigned ? TY_UCHAR : isSigned ? TY_SCHAR : TY_CHAR;
        else if (isShort)
            k = isUnsigned ? TY_USHORT : TY_SHORT;
        else if (nLong >= 2)
            k = isUnsigned ? TY_ULLONG : TY_LLONG;
        else if (nLong == 1)
            k = isUnsigned ? TY_ULONG : TY_LONG;
        else
            k = isUnsigned ? TY_UINT : TY_INT;
        base = newTy(k);
    }
    if (isConst)
        base = withConst(base);
    ds->base = base;
}

static int isNestedDeclaratorStart(size_t i) {
    const Tok *t = &g_toks[i];
    if (t->kind == TK_PUNCT)
        return t->text[0] == '*' || t->text[0] == '(';
    if (t->kind == TK_IDENT) {
        if (isAttrKw(t))
            return 1;
        return !isTypeKw(t->text) && !tdLookup(t->text);
    }
    return 0;
}

static Ty *parseDeclarator(Ty *base, const char **name);

static Ty *adjustParam(Ty *t) {
    t = stripConst(t);
    if (t->kind == TY_ARRAY) {
        Ty *p = ptrTo(t->inner);
        return p;
    }
    if (t->kind == TY_FUNC)
        return ptrTo(t);
    return t;
}

static Ty *parseParams(Ty *ret) {
    Ty *f = newTy(TY_FUNC);
    f->inner = ret;
    Attrs *saved = g_sink;
    memset(&g_paramAttrs, 0, sizeof g_paramAttrs);
    g_sink = &g_paramAttrs;

    Ty **params = NULL;
    int np = 0;
    int cap = 0;
    if (tokIs(")")) {
        g_pos++;
    } else if (tokIs("void") && tokIsAt(g_pos + 1, ")")) {
        g_pos += 2;
    } else {
        for (;;) {
            if (tokIs("...")) {
                f->variadic = 1;
                g_pos++;
                expectTok(")");
                break;
            }
            DeclSpec ds;
            parseDeclSpecs(&ds);
            const char *pname = NULL;
            Ty *pt = adjustParam(parseDeclarator(ds.base, &pname));
            if (np == cap) {
                cap = cap ? cap * 2 : 4;
                params = realloc(params, (size_t)cap * sizeof *params);
                if (!params)
                    fatal("out of memory");
            }
            params[np++] = pt;
            if (tokIs(",")) {
                g_pos++;
                continue;
            }
            expectTok(")");
            break;
        }
    }
    f->params = params;
    f->nparams = np;
    g_sink = saved;
    return f;
}

static Ty *parseSuffixes(Ty *base) {
    if (tokIs("[")) {
        skipBalanced();
        Ty *arr = newTy(TY_ARRAY);
        arr->inner = parseSuffixes(base);
        return arr;
    }
    if (tokIs("(")) {
        g_pos++;
        return parseParams(base);
    }
    return base;
}

static Ty *parseDeclarator(Ty *base, const char **name) {
    for (;;) {
        if (isAttrKw(cur())) {
            parseAttributes();
            continue;
        }
        if (!tokIs("*"))
            break;
        g_pos++;
        base = ptrTo(base);
        for (;;) {
            if (isAttrKw(cur())) {
                parseAttributes();
            } else if (tokIs("const") || tokIs("__const")) {
                base->isConst = 1;
                g_pos++;
            } else if (isQualKw(cur()->text)) {
                g_pos++;
            } else {
                break;
            }
        }
    }

    if (tokIs("(") && isNestedDeclaratorStart(g_pos + 1)) {
        size_t inner = g_pos + 1;
        skipBalanced();
        base = parseSuffixes(base);
        size_t after = g_pos;
        g_pos = inner;
        base = parseDeclarator(base, name);
        expectTok(")");
        g_pos = after;
        return base;
    }

    if (cur()->kind == TK_IDENT && !isTypeKw(cur()->text)) {
        *name = cur()->text;
        g_pos++;
    }
    parseTrailing();
    return parseSuffixes(base);
}

/* ---------- Declarations ---------- */

typedef struct {
    const char *name;  /* C identifier */
    const char *xname; /* exported Win32 name */
    CallConv sourceCc; /* guest-facing convention */
    CallConv targetCc; /* host-facing convention */
    int variadic;
    Ty *ret;
    Ty **args;
    int nargs;
} Sig;

typedef struct {
    const char *name;
    const char *xname;
} Var;

static Sig *g_funcs;
static size_t g_nfuncs;
static size_t g_capFuncs;
static Sig *g_tds;
static size_t g_ntds;
static size_t g_capTds;
static Var *g_vars;
static size_t g_nvars;
static size_t g_capVars;

static void sigAdd(Sig **arr, size_t *n, size_t *cap, Sig s) {
    for (size_t i = 0; i < *n; i++) {
        if (!strcmp((*arr)[i].name, s.name)) {
            (*arr)[i] = s;
            return;
        }
    }
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *arr = realloc(*arr, *cap * sizeof **arr);
        if (!*arr)
            fatal("out of memory");
    }
    (*arr)[(*n)++] = s;
}

static const char *exportName(const char *ident) {
    size_t n = strlen(g_dll);
    if (!strncmp(ident, g_dll, n) && ident[n] == '_' && ident[n + 1])
        return ident + n + 1;
    return ident;
}

/* Convention for a function type on the current arch; x86_64 always uses the SysV convention. */
static CallConv fnTypeCc(const Attrs *a) {
    if (g_arch != ARCH_X86)
        return CC_C;
    if (a->fastcall)
        return CC_FASTCALL;
    if (a->stdcall)
        return CC_STDCALL;
    return CC_C;
}

static void addFunction(const char *name, const Ty *ft) {
    if (g_declAttrs.annot == CC_DEFAULT)
        return;
    Sig s;
    memset(&s, 0, sizeof s);
    s.name = name;
    s.xname = exportName(name);
    s.sourceCc = g_declAttrs.annot;
    s.targetCc = fnTypeCc(&g_declAttrs);
    s.variadic = ft->variadic;
    s.ret = ft->inner;
    s.args = ft->params;
    s.nargs = ft->nparams;
    sigAdd(&g_funcs, &g_nfuncs, &g_capFuncs, s);
}

static void addTypedef(const char *name, const Ty *ptrFn) {
    if (g_declAttrs.annot == CC_DEFAULT)
        return;
    Sig s;
    memset(&s, 0, sizeof s);
    s.name = name;
    s.xname = name;
    s.sourceCc = fnTypeCc(&g_declAttrs);
    s.targetCc = g_declAttrs.annot;
    s.variadic = ptrFn->variadic;
    s.ret = ptrFn->inner;
    s.args = ptrFn->params;
    s.nargs = ptrFn->nparams;
    sigAdd(&g_tds, &g_ntds, &g_capTds, s);
}

static void addVar(const char *name) {
    for (size_t i = 0; i < g_nvars; i++) {
        if (!strcmp(g_vars[i].name, name))
            return;
    }
    if (g_nvars == g_capVars) {
        g_capVars = g_capVars ? g_capVars * 2 : 64;
        g_vars = realloc(g_vars, g_capVars * sizeof *g_vars);
        if (!g_vars)
            fatal("out of memory");
    }
    g_vars[g_nvars].name = name;
    g_vars[g_nvars].xname = exportName(name);
    g_nvars++;
}

static void recordDeclarator(const DeclSpec *ds, Ty *t, const char *name, int hasInit) {
    if (!name)
        return;
    if (ds->isTypedef) {
        tdAdd(name, t);
        if (t->kind == TY_PTR && !t->viaTypedef && t->inner->kind == TY_FUNC)
            addTypedef(name, t->inner);
        return;
    }
    if (t->kind == TY_FUNC) {
        addFunction(name, t);
        return;
    }
    if (ds->isExtern && !hasInit)
        addVar(name);
}

static void skipInitializer(void) {
    int depth = 0;
    while (cur()->kind != TK_EOF) {
        const Tok *t = cur();
        if (t->kind == TK_PUNCT) {
            char c = t->text[0];
            if (depth == 0 && (c == ',' || c == ';'))
                return;
            if (c == '(' || c == '[' || c == '{')
                depth++;
            else if (c == ')' || c == ']' || c == '}')
                depth--;
        }
        g_pos++;
    }
}

/* Returns the index just past a declaration that starts at `start`, for error recovery. */
static size_t skipDeclarationFrom(size_t start) {
    int depth = 0;
    size_t i = start;
    for (; i < g_ntoks; i++) {
        const Tok *t = &g_toks[i];
        if (t->kind == TK_EOF)
            break;
        if (t->kind != TK_PUNCT)
            continue;
        char c = t->text[0];
        if (c == '(' || c == '[' || c == '{') {
            depth++;
        } else if (c == ')' || c == ']') {
            if (depth > 0)
                depth--;
        } else if (c == '}') {
            if (depth > 0)
                depth--;
            if (depth == 0) {
                i++;
                if (i < g_ntoks && tokIsAt(i, ";"))
                    i++;
                return i;
            }
        } else if (c == ';' && depth == 0) {
            return i + 1;
        }
    }
    return i > start ? i : start + 1;
}

static void parseExternalDeclaration(void) {
    if (tokIs(";")) {
        g_pos++;
        return;
    }
    if (tokIs("_Static_assert") || tokIs("static_assert")) {
        g_pos = skipDeclarationFrom(g_pos);
        return;
    }
    memset(&g_declAttrs, 0, sizeof g_declAttrs);
    g_sink = &g_declAttrs;

    DeclSpec ds;
    parseDeclSpecs(&ds);
    if (tokIs(";")) {
        g_pos++;
        return;
    }
    for (;;) {
        const char *name = NULL;
        Ty *t = parseDeclarator(ds.base, &name);
        parseTrailing();
        int hasInit = 0;
        if (tokIs("=")) {
            hasInit = 1;
            skipInitializer();
        }
        recordDeclarator(&ds, t, name, hasInit);
        if (tokIs(",")) {
            g_pos++;
            continue;
        }
        if (tokIs("{")) {
            skipBalanced();
            return;
        }
        expectTok(";");
        return;
    }
}

static void parseTranslationUnit(void) {
    while (cur()->kind != TK_EOF) {
        size_t start = g_pos;
        if (setjmp(g_recover)) {
            g_sink = &g_declAttrs;
            size_t end = skipDeclarationFrom(start);
            for (size_t i = start; i < end && i < g_ntoks; i++) {
                if (g_toks[i].kind == TK_IDENT && !strcmp(g_toks[i].text, "annotate"))
                    fatal("cannot parse annotated declaration (%s)", g_errmsg);
            }
            g_pos = end;
            continue;
        }
        parseExternalDeclaration();
    }
}

/* ---------- Layout ---------- */

typedef struct {
    const char *reg;
    int slot;
    int onStack;
    long stackOff;
} Place;

typedef struct {
    Place *places;
    long stackSize;
} Layout;

static long probeRecordSize(const char *spelling);

/* Records passed or returned by value have no size in their declarations; the compiler supplies it. */
static long typeSize(const Ty *t) {
    if (t->size >= 0)
        return t->size;
    if (t->kind == TY_RECORD && t->viaTypedef && t->name)
        return probeRecordSize(t->name);
    return t->size;
}

static int slotSize(const Ty *t, Arch arch) {
    if (t->kind == TY_PTR)
        return arch == ARCH_X86_64 ? 8 : 4;
    long size = typeSize(t);
    if (size < 0)
        fatal("argument of unknown size is not supported");
    if (arch == ARCH_X86) {
        if (size <= 4)
            return 4;
        if (size <= 8)
            return 8;
    } else if (size <= 8) {
        return 8;
    }
    fatal("argument size %ld is not supported on %s", size, arch == ARCH_X86 ? "x86" : "x86_64");
}

typedef struct {
    Layout *out;
    Arch arch;
    long stackOff;
    const char *const *gpr;
    int ngpr;
    int gprIndex;
    int count;
} LayoutBuilder;

static void pushStack(LayoutBuilder *b, const Ty *t) {
    Place *p = &b->out->places[b->count++];
    p->reg = NULL;
    p->slot = slotSize(t, b->arch);
    p->onStack = 1;
    p->stackOff = b->stackOff;
    b->stackOff += p->slot;
    b->out->stackSize += p->slot;
}

static void pushReg(LayoutBuilder *b, const Ty *t) {
    Place *p = &b->out->places[b->count++];
    p->slot = slotSize(t, b->arch);
    p->onStack = 0;
    p->stackOff = 0;
    p->reg = b->gpr[b->gprIndex++];
}

/* Mirrors compute_arg_layout(): x86 fastcall uses ecx/edx until one spills; x86_64 C uses SysV. */
static Layout computeLayout(Ty **args, int nargs, CallConv cc, Arch arch, long stackOffset, int skipArgs) {
    static const char *const kFastcallRegs[] = {"ecx", "edx"};
    static const char *const kSysVRegs[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    Layout out;
    out.places = calloc(nargs > 0 ? (size_t)nargs : 1, sizeof(Place));
    out.stackSize = 0;
    if (!out.places)
        fatal("out of memory");

    LayoutBuilder b;
    b.out = &out;
    b.arch = arch;
    b.gpr = NULL;
    b.ngpr = 0;
    if (arch == ARCH_X86 && cc == CC_FASTCALL) {
        b.gpr = kFastcallRegs;
        b.ngpr = 2;
    } else if (arch == ARCH_X86_64 && cc == CC_C) {
        b.gpr = kSysVRegs;
        b.ngpr = 6;
    }
    long regSize = arch == ARCH_X86_64 ? 8 : 4;
    b.gprIndex = skipArgs;
    b.count = 0;
    long consumed = (skipArgs > b.ngpr ? skipArgs - b.ngpr : 0) * regSize;
    b.stackOff = stackOffset + consumed;
    out.stackSize += consumed;

    if (arch == ARCH_X86 && cc == CC_FASTCALL) {
        int argsStart = 0;
        for (int i = 0; i < nargs && i < b.ngpr; i++) {
            if (b.gprIndex >= b.ngpr)
                break;
            if (slotSize(args[i], arch) != 4)
                break;
            pushReg(&b, args[i]);
            argsStart++;
        }
        for (int i = argsStart; i < nargs; i++)
            pushStack(&b, args[i]);
    } else {
        for (int i = 0; i < nargs; i++) {
            int slot = slotSize(args[i], arch);
            if (slot <= regSize && b.gprIndex < b.ngpr)
                pushReg(&b, args[i]);
            else
                pushStack(&b, args[i]);
        }
    }
    return out;
}

static void describePlace(char *out, size_t n, const Place *p) {
    if (p->reg)
        snprintf(out, n, "%s[%d]", p->reg, p->slot);
    else
        snprintf(out, n, "stack+%ld[%d]", p->stackOff, p->slot);
}

/* ---------- x86_64 register helpers ---------- */

static void x64Reg(char *out, const char *reg, int slot) {
    static const char *const kGp[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp"};
    int isGp = 0;
    for (size_t i = 0; i < sizeof kGp / sizeof kGp[0]; i++) {
        if (!strcmp(reg, kGp[i]))
            isGp = 1;
    }
    if (slot == 8) {
        snprintf(out, 16, "%s", reg);
    } else if (isGp) {
        if (slot == 4) {
            snprintf(out, 16, "e%s", reg + 1);
        } else if (slot == 2) {
            snprintf(out, 16, "%s", reg + 1);
        } else if (slot == 1) {
            if (!strcmp(reg, "rax") || !strcmp(reg, "rbx") || !strcmp(reg, "rcx") || !strcmp(reg, "rdx"))
                snprintf(out, 16, "%cl", reg[1]);
            else if (!strcmp(reg, "rsi") || !strcmp(reg, "rdi"))
                snprintf(out, 16, "%cil", reg[1]);
            else
                snprintf(out, 16, "%cpl", reg[1]);
        } else {
            fatal("unsupported register slot size %d", slot);
        }
    } else if (slot == 4) {
        snprintf(out, 16, "%sd", reg);
    } else if (slot == 2) {
        snprintf(out, 16, "%sw", reg);
    } else if (slot == 1) {
        snprintf(out, 16, "%sb", reg);
    } else {
        fatal("unsupported register slot size %d", slot);
    }
}

static const char *x64Ptr(int slot) {
    if (slot == 4)
        return "dword ptr";
    if (slot == 8)
        return "qword ptr";
    fatal("unsupported slot size %d", slot);
}

/* ---------- Thunk emission ---------- */

static void emitArgComments(Buf *o, const Sig *f, const Layout *src, const Layout *tgt) {
    for (int i = 0; i < f->nargs; i++) {
        char sd[64];
        char dd[64];
        describePlace(sd, sizeof sd, &src->places[i]);
        describePlace(dd, sizeof dd, &tgt->places[i]);
        bline(o, "\t# Arg %d (src=%s, dst=%s, class=int, sign_extended=%s)", i, sd, dd,
              signExtended(f->args[i]) ? "True" : "False");
    }
}

static long retSizeOrDie(const Sig *f) {
    if (f->ret->kind == TY_VOID)
        return 0;
    long size = typeSize(f->ret);
    if (size < 0)
        fatal("unsupported return type for %s", f->name);
    return size;
}

static void emitThunk32(Buf *o, const Sig *f, int h2g) {
    const char *callTarget = h2g ? "[eax+4]" : f->name;
    long align = h2g ? 0 : 16;

    if (f->variadic) {
        if (f->sourceCc != CC_C || f->targetCc != CC_C)
            fatal("variadic function %s must be cdecl", f->name);
        bline(o, "\tjmp %s", callTarget);
        return;
    }

    Layout src = computeLayout(f->args, f->nargs, f->sourceCc, ARCH_X86, 4, h2g ? 1 : 0);
    Layout tgt = computeLayout(f->args, f->nargs, f->targetCc, ARCH_X86, 0, 0);

    if (h2g)
        bline(o, "\tGET_TEB_HOST ecx");
    else
        bline(o, "\tmov ecx, fs:[TEB_SELF]");

    bline(o, "\tmov ax, fs");
    bline(o, "\tmov dx, word ptr [ecx+TEB_FS_SEL]");
    bline(o, "\tmov word ptr [ecx+TEB_FS_SEL], ax");
    bline(o, "\tmov fs, dx");
    bline(o, "\tmov ax, gs");
    bline(o, "\tmov dx, word ptr [ecx+TEB_GS_SEL]");
    bline(o, "\tmov word ptr [ecx+TEB_GS_SEL], ax");
    bline(o, "\tmov gs, dx");

    if (f->nargs > 0 || h2g)
        bline(o, "\tmov eax, esp");

    bline(o, "\tpush ebp");
    bline(o, "\tmov ebp, dword ptr [ecx+TEB_SP]");
    bline(o, "\tmov dword ptr [ecx+TEB_SP], esp");
    bline(o, "\tmov esp, ebp");

    if (tgt.stackSize > 0)
        bline(o, "\tsub esp, %ld", tgt.stackSize);
    if (align > 0)
        bline(o, "\tand esp, ~%ld", align - 1);

    /* Publish the guest's errno before the shim runs. eax holds the source argument pointer, so it is
     * saved around the hook. esp is 16-byte aligned here, so the pad keeps the call site aligned. */
    if (!h2g) {
        bline(o, "\tpush eax");
        bline(o, "\tsub esp, 12");
        bline(o, "\tcall wibo_errnoGuestToHost");
        bline(o, "\tadd esp, 12");
        bline(o, "\tpop eax");
    }

    for (int i = 0; i < f->nargs; i++) {
        const Place *t = &tgt.places[i];
        if (!t->onStack)
            continue;
        const Place *s = &src.places[i];
        if (!s->onStack)
            fatal("source calling convention %s requires register argument %d of %s", ccName(f->sourceCc), i,
                  f->name);
        if (s->slot != t->slot)
            fatal("argument %d of %s requires size conversion %d->%d", i, f->name, s->slot, t->slot);
        for (int off = 0; off < t->slot; off += 4) {
            bline(o, "\tmov ecx, [eax+%ld]", s->stackOff + off);
            bline(o, "\tmov [esp+%ld], ecx", t->stackOff + off);
        }
    }

    for (int i = 0; i < f->nargs; i++) {
        const Place *t = &tgt.places[i];
        if (t->onStack)
            continue;
        const Place *s = &src.places[i];
        if (!s->onStack)
            fatal("source calling convention %s requires register argument %d of %s", ccName(f->sourceCc), i,
                  f->name);
        bline(o, "\tmov %s, [eax+%ld]", t->reg, s->stackOff);
    }

    bline(o, "\tcall %s", callTarget);

    /* The hook may clobber eax, edx and ecx. ecx is reloaded from the TEB below, so only the return value
     * needs saving. */
    if (!h2g) {
        bline(o, "\tpush eax");
        bline(o, "\tpush edx");
        bline(o, "\tsub esp, 8");
        bline(o, "\tcall wibo_errnoHostToGuest");
        bline(o, "\tadd esp, 8");
        bline(o, "\tpop edx");
        bline(o, "\tpop eax");
    }

    long retSize = retSizeOrDie(f);
    if (retSize > 8)
        fatal("return size %ld not supported for %s", retSize, f->name);
    int saveEax = retSize > 0;
    int saveEdx = retSize > 4;

    if (saveEax)
        bline(o, "\tpush eax");
    if (saveEdx)
        bline(o, "\tpush edx");
    if (h2g)
        bline(o, "\tmov ecx, fs:[TEB_SELF]");
    else
        bline(o, "\tGET_TEB_HOST ecx");
    bline(o, "\tmov ax, fs");
    bline(o, "\tmov dx, word ptr [ecx+TEB_FS_SEL]");
    bline(o, "\tmov word ptr [ecx+TEB_FS_SEL], ax");
    bline(o, "\tmov fs, dx");
    bline(o, "\tmov ax, gs");
    bline(o, "\tmov dx, word ptr [ecx+TEB_GS_SEL]");
    bline(o, "\tmov word ptr [ecx+TEB_GS_SEL], ax");
    bline(o, "\tmov gs, dx");
    if (saveEdx)
        bline(o, "\tpop edx");
    if (saveEax)
        bline(o, "\tpop eax");

    bline(o, "\tmov esp, ebp");
    bline(o, "\tmov ebp, dword ptr [ecx+TEB_SP]");
    bline(o, "\tmov dword ptr [ecx+TEB_SP], esp");
    bline(o, "\tleave");

    if (f->sourceCc == CC_STDCALL && src.stackSize > 0)
        bline(o, "\tret %ld", src.stackSize);
    else
        bline(o, "\tret");
}

/* Variadic guests: slots past the declared parameters are forwarded as 64-bit integers, so the
 * host's va_arg sees the MS argument list. The host only reads the slots the format consumes. */
static const int kVarargSlots = 16;

static void emitThunk64(Buf *o, const Sig *f, int h2g) {
    const char *callTarget = h2g ? "edi" : f->name;
    long align = h2g ? 0 : 16;
    Ty *srcSlotArgs[kVarargSlots + 8];
    Ty *tgtSlotArgs[kVarargSlots + 8];
    Ty **srcArgs = f->args;
    Ty **tgtArgs = f->args;
    int nslots = f->nargs;

    if (f->variadic) {
        static Ty *sU32, *sU64;

        if (f->sourceCc != CC_C || f->targetCc != CC_C)
            fatal("variadic function %s must be cdecl", f->name);
        if (h2g) {
            bline(o, "\tjmp %s", callTarget);
            return;
        }
        if (f->nargs > 8)
            fatal("variadic function %s has too many fixed arguments", f->name);
        if (!sU32) {
            sU32 = newTy(TY_UINT);
            sU64 = newTy(TY_ULONG);
        }
        /* Guest variadic slots are 4-byte cdecl stack entries; the host sees them zero-extended to 8 bytes. */
        for (int i = 0; i < f->nargs; i++) {
            srcSlotArgs[i] = f->args[i];
            tgtSlotArgs[i] = f->args[i];
        }
        for (int i = f->nargs; i < f->nargs + kVarargSlots; i++) {
            srcSlotArgs[i] = sU32;
            tgtSlotArgs[i] = sU64;
        }
        srcArgs = srcSlotArgs;
        tgtArgs = tgtSlotArgs;
        nslots = f->nargs + kVarargSlots;
    }

    Layout src = computeLayout(srcArgs, nslots, f->sourceCc, h2g ? ARCH_X86_64 : ARCH_X86, h2g ? 24 : 20,
                               h2g ? 1 : 0);
    Layout tgt = computeLayout(tgtArgs, nslots, f->targetCc, h2g ? ARCH_X86 : ARCH_X86_64, 0, 0);
    char r[16];

    if (h2g) {
        bline(o, ".code64");
        bline(o, "\tpush rbp");
        bline(o, "\tpush rbx");
        bline(o, "\tmov r10, rsp");
        bline(o, "\tGET_TEB_HOST rbx");
        if (!g_darwin) {
            bline(o, "\tREAD_FSBASE r9, rbx");
            bline(o, "\tmov qword ptr [rbx+TEB_FSBASE], r9");
        }
        bline(o, "\tmov rbp, qword ptr [rbx+TEB_SP]");
        bline(o, "\tmov qword ptr [rbx+TEB_SP], rsp");
        bline(o, "\tmov rsp, rbp");
        if (tgt.stackSize > 0)
            bline(o, "\tsub rsp, %ld", tgt.stackSize);
        if (align > 0)
            bline(o, "\tand rsp, ~%ld", align - 1);

        for (int i = 0; i < f->nargs; i++) {
            const Place *t = &tgt.places[i];
            const Place *s = &src.places[i];
            if (!t->onStack)
                fatal("unexpected register argument %d of %s", i, f->name);
            if (s->onStack) {
                x64Reg(r, "rax", t->slot);
                bline(o, "\tmov %s, %s [r10+%ld]", r, x64Ptr(s->slot), s->stackOff);
            } else if (s->reg) {
                x64Reg(r, s->reg, t->slot);
            } else {
                fatal("argument %d of %s is not a register or stack slot", i, f->name);
            }
            bline(o, "\tmov %s [rsp+%ld], %s", x64Ptr(t->slot), t->stackOff, r);
        }

        bline(o, "\tLJMP32 rbx");
        if (!g_darwin) {
            bline(o, "\tmov ax, word ptr [ebx+TEB_FS_SEL]");
            bline(o, "\tmov fs, ax");
        }
        bline(o, "\tcall %s", callTarget);
        bline(o, "\tmov ebx, fs:[TEB_SELF]");
        bline(o, "\tLJMP64 ebx");
        if (signExtended(f->ret))
            bline(o, "\tcdqe");
        if (!g_darwin) {
            bline(o, "\tmov r9, qword ptr [rbx+TEB_FSBASE]");
            bline(o, "\tWRITE_FSBASE r9, rbx");
        }
        bline(o, "\tmov rsp, qword ptr [rbx+TEB_SP]");
        bline(o, "\tmov qword ptr [rbx+TEB_SP], rbp");
        bline(o, "\tpop rbx");
        bline(o, "\tpop rbp");
        bline(o, "\tret");
        return;
    }

    bline(o, ".code32");
    bline(o, "\tpush ebp");
    bline(o, "\tpush esi");
    bline(o, "\tpush edi");
    bline(o, "\tpush ebx");
    bline(o, "\tmov ebx, fs:[TEB_SELF]");
    if (!g_darwin) {
        bline(o, "\tmov di, fs");
        bline(o, "\tmov word ptr [ebx+TEB_FS_SEL], di");
    }
    bline(o, "\tLJMP64 ebx");
    if (!g_darwin) {
        bline(o, "\tmov r9, qword ptr [rbx+TEB_FSBASE]");
        bline(o, "\tWRITE_FSBASE r9, rbx");
    }
    bline(o, "\tmov r10, rsp");
    bline(o, "\tmov rbp, qword ptr [rbx+TEB_SP]");
    bline(o, "\tmov qword ptr [rbx+TEB_SP], rsp");
    bline(o, "\tmov rsp, rbp");
    if (tgt.stackSize > 0)
        bline(o, "\tsub rsp, %ld", tgt.stackSize);
    if (align > 0)
        bline(o, "\tand rsp, ~%ld", align - 1);
    /* Publish the guest's errno to the host before the shim runs. This precedes the argument loads, so
     * it may clobber caller-saved registers. r10 holds the guest stack pointer, so it is saved on the stack,
     * and rsp is restored from the saved copy after the call. */
    bline(o, "\tpush r10");
    bline(o, "\tmov r10, rsp");
    bline(o, "\tand rsp, ~15");
    bline(o, "\tpush r10");
    bline(o, "\tpush r10");
    bline(o, "\tcall wibo_errnoGuestToHost");
    bline(o, "\tmov rsp, qword ptr [rsp]");
    bline(o, "\tpop r10");

    for (int i = 0; i < nslots; i++) {
        const Place *t = &tgt.places[i];
        const Place *s = &src.places[i];
        const Ty *arg = srcArgs[i];
        if (t->onStack) {
            if (s->onStack) {
                x64Reg(r, "rax", s->slot);
                bline(o, "\tmov %s, %s [r10+%ld]", r, x64Ptr(s->slot), s->stackOff);
                x64Reg(r, "rax", t->slot);
            } else if (s->reg) {
                x64Reg(r, s->reg, t->slot);
            } else {
                fatal("argument %d of %s is not a register or stack slot", i, f->name);
            }
            bline(o, "\tmov %s [rsp+%ld], %s", x64Ptr(t->slot), t->stackOff, r);
        } else if (t->reg) {
            if (s->slot == 4 && t->slot == 8) {
                x64Reg(r, t->reg, s->slot);
                bline(o, "\tmov %s, %s [r10+%ld]", r, x64Ptr(s->slot), s->stackOff);
                if (signExtended(arg))
                    bline(o, "\tmovsxd %s, %s", t->reg, r);
            } else if (s->slot == 8 && t->slot == 8) {
                bline(o, "\tmov %s, %s [r10+%ld]", t->reg, x64Ptr(s->slot), s->stackOff);
            } else {
                fatal("unsupported conversion from %d to %d bytes for argument %d of %s", s->slot, t->slot, i,
                      f->name);
            }
        }
    }

    bline(o, "\tcall %s", callTarget);
    /* The hook may clobber caller-saved registers, so rax and rdx (the return value) are kept.
     * rsp is 16-byte aligned here, so the two pushes keep the call site aligned. */
    bline(o, "\tpush rax");
    bline(o, "\tpush rdx");
    bline(o, "\tcall wibo_errnoHostToGuest");
    bline(o, "\tpop rdx");
    bline(o, "\tpop rax");
    if (typeSize(f->ret) > 4)
        bline(o, "\tmov rdx, rax\n\tshr rdx, 32");
    bline(o, "\tmov rsp, qword ptr [rbx+TEB_SP]");
    bline(o, "\tmov qword ptr [rbx+TEB_SP], rbp");
    bline(o, "\tLJMP32 rbx");
    if (!g_darwin) {
        bline(o, "\tmov di, word ptr [ebx+TEB_FS_SEL]");
        bline(o, "\tmov fs, di");
    }
    bline(o, "\tpop ebx");
    bline(o, "\tpop edi");
    bline(o, "\tpop esi");
    bline(o, "\tpop ebp");
    if (f->sourceCc == CC_STDCALL && src.stackSize > 0)
        bline(o, "\tret %ld", src.stackSize);
    else
        bline(o, "\tret");
}

static void emitThunk(Buf *o, const Sig *f, int h2g) {
    if (g_arch == ARCH_X86_64)
        emitThunk64(o, f, h2g);
    else
        emitThunk32(o, f, h2g);
}

/*
 * Win64 guest -> SysV host. The guest's rcx/rdx/r8/r9 are first spilled to their home slots, which
 * are the caller's shadow space above the return address. That makes the register shuffle order-free.
 * rdi/rsi and xmm6-15 are callee-saved on Win64 but caller-saved on SysV, so they are preserved here.
 */
static int isFloatTy(const Ty *t) {
    return t->kind == TY_FLOAT || t->kind == TY_DOUBLE || t->kind == TY_LDOUBLE;
}

static void emitThunkWin64(Buf *o, const Sig *f) {
    static const char *const kWin64Regs[] = {"rcx", "rdx", "r8", "r9"};
    const long kHomeOff = 16;  /* [rbp+16] is the first shadow slot: rbp = entry rsp - 8 */
    const long kStackOff = 48; /* [rbp+48] is the 5th argument */
    const int kXmmSaved = 10;  /* xmm6..xmm15 */
    if (f->variadic && f->nargs > kVarargSlots)
        fatal("variadic function %s has too many fixed arguments", f->name);
    static Ty *sU64;
    Ty *slotArgs[kVarargSlots];
    int nslots = f->nargs;
    Ty **args = f->args;
    if (f->variadic) {
        if (!sU64)
            sU64 = newTy(TY_ULONG);
        nslots = kVarargSlots;
        for (int i = 0; i < kVarargSlots; i++)
            slotArgs[i] = i < f->nargs ? f->args[i] : sU64;
        args = slotArgs;
    }

    for (int i = 0; i < nslots; i++) {
        if (isFloatTy(args[i]))
            fatal("floating-point argument %d of %s is not supported by Win64 thunks", i, f->name);
        if (typeSize(args[i]) > 8)
            fatal("argument %d of %s is larger than 8 bytes", i, f->name);
    }
    if (f->ret->kind != TY_VOID && typeSize(f->ret) > 8)
        fatal("return size %ld not supported for %s", typeSize(f->ret), f->name);

    Layout tgt = computeLayout(args, nslots, CC_C, ARCH_X86_64, 0, 0);
    long stackArgs = (tgt.stackSize + 15) & ~15L;
    long frame = stackArgs + kXmmSaved * 16;

    bline(o, "\tpush rbp");
    bline(o, "\tmov rbp, rsp");
    bline(o, "\tpush rdi");
    bline(o, "\tpush rsi");
    bline(o, "\tsub rsp, %ld", frame);
    for (int k = 0; k < kXmmSaved; k++)
        bline(o, "\tmovups xmmword ptr [rsp+%ld], xmm%d", stackArgs + 16L * k, 6 + k);
    for (int i = 0; i < nslots && i < 4; i++)
        bline(o, "\tmov qword ptr [rbp+%ld], %s", kHomeOff + 8L * i, kWin64Regs[i]);

    for (int i = 0; i < nslots; i++) {
        long srcOff = i < 4 ? kHomeOff + 8L * i : kStackOff + 8L * (i - 4);
        const Place *t = &tgt.places[i];
        if (t->onStack) {
            bline(o, "\tmov rax, qword ptr [rbp+%ld]", srcOff);
            bline(o, "\tmov qword ptr [rsp+%ld], rax", t->stackOff);
        } else {
            bline(o, "\tmov %s, qword ptr [rbp+%ld]", t->reg, srcOff);
        }
    }

    /* SysV variadic callees read al as the vector-register count; no floats are passed, so it is 0. */
    bline(o, "\txor eax, eax");
    bline(o, "\tcall %s", f->name);
    if (f->ret->kind != TY_VOID && typeSize(f->ret) == 4 && signExtended(f->ret))
        bline(o, "\tmovsxd rax, eax");

    for (int k = 0; k < kXmmSaved; k++)
        bline(o, "\tmovups xmm%d, xmmword ptr [rsp+%ld]", 6 + k, stackArgs + 16L * k);
    bline(o, "\tadd rsp, %ld", frame);
    bline(o, "\tpop rsi");
    bline(o, "\tpop rdi");
    bline(o, "\tpop rbp");
    bline(o, "\tret");
}

/* ---------- Output ---------- */

static void emitAsm(Buf *o, const Sig *funcs, size_t nf, const Sig *tds, size_t nt) {
    bline(o, "# Auto-generated thunks; DO NOT EDIT.");
    bline(o, "#include \"macros.S\"");
    bline(o, ".text");

    if (g_win64) {
        for (size_t i = 0; i < nf; i++) {
            const Sig *f = &funcs[i];
            bline(o, "%s", "");
            bline(o, "# %s (win64 guest -> sysv host)", f->name);
            bline(o, "ASM_GLOBAL(thunk_%s_%s, @function)", g_dll, f->xname);
            emitThunkWin64(o, f);
            bline(o, "ASM_END(thunk_%s_%s)", g_dll, f->xname);
        }
        return;
    }

    for (size_t i = 0; i < nf; i++) {
        const Sig *f = &funcs[i];
        Layout src = computeLayout(f->args, f->nargs, f->sourceCc, ARCH_X86, 0, 0);
        Layout tgt = computeLayout(f->args, f->nargs, f->targetCc, g_arch, 0, 0);
        bline(o, "%s", "");
        bline(o, "# %s (source_cc=%s, target_cc=%s, variadic=%s)", f->name, ccName(f->sourceCc),
              ccName(f->targetCc), f->variadic ? "True" : "False");
        emitArgComments(o, f, &src, &tgt);
        bline(o, "ASM_GLOBAL(thunk_%s_%s, @function)", g_dll, f->xname);
        emitThunk(o, f, 0);
        bline(o, "ASM_END(thunk_%s_%s)", g_dll, f->xname);
    }

    for (size_t i = 0; i < nt; i++) {
        const Sig *td = &tds[i];
        Layout src = computeLayout(td->args, td->nargs, td->sourceCc, g_arch, 0, 1);
        Layout tgt = computeLayout(td->args, td->nargs, td->targetCc, ARCH_X86, 0, 0);
        bline(o, "%s", "");
        bline(o, "# %s (target_cc=%s, variadic=%s)", td->name, ccName(td->targetCc),
              td->variadic ? "True" : "False");
        emitArgComments(o, td, &src, &tgt);
        bline(o, "ASM_WEAK(call_%s, @function)", td->name);
        emitThunk(o, td, 1);
        bline(o, "ASM_END(call_%s)", td->name);
    }
}

static void emitHeaderWin64(Buf *o, const Sig *funcs, size_t nf, const Sig *tds, size_t nt, const Var *vars,
                            size_t nv) {
    char guard[128];
    size_t gi = 0;
    for (const char *p = g_dll; *p && gi + 1 < sizeof guard - 20; p++)
        guard[gi++] = (char)toupper((unsigned char)*p);
    guard[gi] = '\0';
    char fullGuard[160];
    snprintf(fullGuard, sizeof fullGuard, "WIBO_GEN_%s_WIN64_THUNKS_H", guard);

    bline(o, "/* Auto-generated; DO NOT EDIT. */");
    bline(o, "#ifndef %s", fullGuard);
    bline(o, "#define %s", fullGuard);
    bline(o, "#include <stddef.h>");
    bline(o, "#include <string.h>");
    bline(o, "#ifdef __cplusplus");
    bline(o, "extern \"C\" {");
    bline(o, "#endif");
    for (size_t i = 0; i < nf; i++)
        bline(o, "extern void thunk_%s_%s(void);", g_dll, funcs[i].xname);
    bline(o, "#ifdef __cplusplus");
    bline(o, "}");
    bline(o, "#endif");
    bline(o, "%s", "");
    // Host code calls guest function pointers directly; the ms_abi cast makes gcc emit the Win64 call sequence.
    for (size_t i = 0; i < nt; i++) {
        const Sig *td = &tds[i];
        if (td->variadic)
            continue;
        bprintf(o, "static inline ");
        writeTypeForHeader(o, td->ret, 0);
        bprintf(o, " call_%s(%s fn", td->name, td->name);
        for (int a = 0; a < td->nargs; a++) {
            bprintf(o, ", ");
            writeTypeForHeader(o, td->args[a], 0);
            bprintf(o, " arg%d", a);
        }
        bline(o, ") {");
        bprintf(o, "\t%s((", td->ret->kind == TY_VOID ? "" : "return ");
        writeTypeForHeader(o, td->ret, 0);
        bprintf(o, " (__attribute__((ms_abi)) *)(");
        if (td->nargs == 0)
            bprintf(o, "void");
        for (int a = 0; a < td->nargs; a++) {
            if (a)
                bprintf(o, ", ");
            writeTypeForHeader(o, td->args[a], 0);
        }
        bprintf(o, "))(void *)fn)(");
        for (int a = 0; a < td->nargs; a++) {
            if (a)
                bprintf(o, ", ");
            bprintf(o, "arg%d", a);
        }
        bline(o, ");");
        bline(o, "}");
    }
    bline(o, "%s", "");
    bline(o, "static inline void *%sThunkByName(const char *name) {", g_dll);
    for (size_t i = 0; i < nf; i++)
        bline(o, "\tif (strcmp(name, \"%s\") == 0) return (void*)&thunk_%s_%s;", funcs[i].xname, g_dll,
              funcs[i].xname);
    for (size_t i = 0; i < nv; i++)
        bline(o, "\tif (strcmp(name, \"%s\") == 0) return (void*)&%s;", vars[i].xname, vars[i].name);
    bline(o, "\treturn NULL;");
    bline(o, "}");
    bline(o, "#endif /* %s */", fullGuard);
}

static void emitHeader(Buf *o, const Sig *funcs, size_t nf, const Sig *tds, size_t nt, const Var *vars, size_t nv) {
    if (g_win64) {
        emitHeaderWin64(o, funcs, nf, tds, nt, vars, nv);
        return;
    }
    char guard[128];
    size_t gi = 0;
    for (const char *p = g_dll; *p && gi + 1 < sizeof guard - 20; p++)
        guard[gi++] = (char)toupper((unsigned char)*p);
    guard[gi] = '\0';
    char fullGuard[160];
    snprintf(fullGuard, sizeof fullGuard, "WIBO_GEN_%s_THUNKS_H", guard);

    bline(o, "/* Auto-generated; DO NOT EDIT. */");
    bline(o, "#ifndef %s", fullGuard);
    bline(o, "#define %s", fullGuard);
    bline(o, "#include <stddef.h>");
    bline(o, "#include <string.h>");
    bline(o, "#ifdef __cplusplus");
    bline(o, "extern \"C\" {");
    bline(o, "#endif");

    for (size_t i = 0; i < nf; i++) {
        const Sig *f = &funcs[i];
        const char *ccAttr;
        if (g_arch == ARCH_X86_64)
            ccAttr = "";
        else if (f->sourceCc == CC_STDCALL)
            ccAttr = "__attribute__((stdcall)) ";
        else if (f->sourceCc == CC_C)
            ccAttr = "__attribute__((cdecl)) ";
        else
            fatal("unsupported calling convention %s for function %s", ccName(f->sourceCc), f->name);
        bprintf(o, "%s", ccAttr);
        writeTypeForHeader(o, f->ret, 1);
        bprintf(o, " thunk_%s_%s(", g_dll, f->xname);
        for (int a = 0; a < f->nargs; a++) {
            if (a)
                bprintf(o, ", ");
            writeTypeForHeader(o, f->args[a], 1);
            bprintf(o, " arg%d", a);
        }
        bline(o, ");");
    }

    for (size_t i = 0; i < nt; i++) {
        const Sig *td = &tds[i];
        if (td->variadic)
            continue;
        writeTypeForHeader(o, td->ret, 0);
        bprintf(o, " call_%s(%s fn", td->name, td->name);
        for (int a = 0; a < td->nargs; a++) {
            bprintf(o, ", ");
            writeTypeForHeader(o, td->args[a], 0);
            bprintf(o, " arg%d", a);
        }
        bline(o, ");");
    }

    bline(o, "#ifdef __cplusplus");
    bline(o, "}");
    bline(o, "#endif");
    bline(o, "%s", "");
    bline(o, "static inline void *%sThunkByName(const char *name) {", g_dll);
    for (size_t i = 0; i < nf; i++)
        bline(o, "\tif (strcmp(name, \"%s\") == 0) return (void*)&thunk_%s_%s;", funcs[i].xname, g_dll,
              funcs[i].xname);
    for (size_t i = 0; i < nv; i++)
        bline(o, "\tif (strcmp(name, \"%s\") == 0) return (void*)&%s;", vars[i].xname, vars[i].name);
    bline(o, "\treturn NULL;");
    bline(o, "}");
    bline(o, "#endif /* %s */", fullGuard);
}

static void writeFile(const char *path, const Buf *b) {
    FILE *f = fopen(path, "w");
    if (!f)
        fatal("cannot open %s for writing: %s", path, strerror(errno));
    if (fwrite(b->data ? b->data : "", 1, b->len, f) != b->len) {
        fclose(f);
        fatal("failed writing %s", path);
    }
    if (fclose(f) != 0)
        fatal("failed writing %s", path);
}

/* ---------- Preprocessing ---------- */

typedef struct {
    char **items;
    size_t n;
    size_t cap;
} StrList;

static void slPush(StrList *l, char *s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = realloc(l->items, l->cap * sizeof *l->items);
        if (!l->items)
            fatal("out of memory");
    }
    l->items[l->n++] = s;
}

static char *joinOpt(const char *opt, const char *val) {
    size_t a = strlen(opt);
    size_t b = strlen(val);
    char *s = malloc(a + b + 1);
    if (!s)
        fatal("out of memory");
    memcpy(s, opt, a);
    memcpy(s + a, val, b + 1);
    return s;
}

/* Feeds `in` to the compiler on stdin and captures stdout in `out`. */
static void runCompiler(const char *ccCmd, const char *const *modeArgs, size_t nModeArgs, const StrList *incs,
                        const StrList *defs, const Buf *in, Buf *out) {
    StrList argv = {0, 0, 0};
    char *cmd = strdup(ccCmd);
    if (!cmd)
        fatal("out of memory");
    for (char *tok = strtok(cmd, " \t"); tok; tok = strtok(NULL, " \t"))
        slPush(&argv, tok);
    if (argv.n == 0)
        fatal("empty --cc command");
    for (size_t i = 0; i < nModeArgs; i++)
        slPush(&argv, (char *)modeArgs[i]);
    for (size_t i = 0; i < incs->n; i++)
        slPush(&argv, incs->items[i]);
    for (size_t i = 0; i < defs->n; i++)
        slPush(&argv, defs->items[i]);
    slPush(&argv, "-");
    slPush(&argv, NULL);

    int inPipe[2];
    int outPipe[2];
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0)
        fatal("pipe failed: %s", strerror(errno));
    pid_t pid = fork();
    if (pid < 0)
        fatal("fork failed: %s", strerror(errno));
    if (pid == 0) {
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        close(inPipe[0]);
        close(inPipe[1]);
        close(outPipe[0]);
        close(outPipe[1]);
        execvp(argv.items[0], argv.items);
        fprintf(stderr, "gentramp: cannot execute %s: %s\n", argv.items[0], strerror(errno));
        _exit(127);
    }
    close(inPipe[0]);
    close(outPipe[1]);

    size_t off = 0;
    while (off < in->len) {
        ssize_t w = write(inPipe[1], in->data + off, in->len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            fatal("writing to preprocessor failed: %s", strerror(errno));
        }
        off += (size_t)w;
    }
    close(inPipe[1]);

    char chunk[65536];
    for (;;) {
        ssize_t r = read(outPipe[0], chunk, sizeof chunk);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            fatal("reading preprocessor output failed: %s", strerror(errno));
        }
        if (r == 0)
            break;
        bprintf(out, "%.*s", (int)r, chunk);
    }
    close(outPipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            fatal("waitpid failed: %s", strerror(errno));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fatal("'%s' failed on generated input", ccCmd);

    free(cmd);
    free(argv.items);
}

static void preprocess(const char *ccCmd, const StrList *incs, const StrList *defs, const Buf *tu, Buf *out) {
    static const char *const kPreprocessArgs[] = {"-E", "-P", "-x", "c", "-DWIBO_CODEGEN=1"};
    runCompiler(ccCmd, kPreprocessArgs, sizeof kPreprocessArgs / sizeof kPreprocessArgs[0], incs, defs, tu, out);
}

/* ---------- Record size probing ---------- */

static const char *g_ccCmd;
static const StrList *g_incs;
static const StrList *g_defs;
static Buf g_pp;

typedef struct SizeCacheEntry {
    char *spelling;
    long size;
    struct SizeCacheEntry *next;
} SizeCacheEntry;

static SizeCacheEntry *g_sizeCache;

/* Asks the compiler for sizeof(spelling) by emitting it as a data symbol in assembly. */
static long probeRecordSize(const char *spelling) {
    for (SizeCacheEntry *e = g_sizeCache; e; e = e->next) {
        if (strcmp(e->spelling, spelling) == 0)
            return e->size;
    }

    Buf src = {0, 0, 0};
    bprintf(&src, "%.*s", (int)g_pp.len, g_pp.data ? g_pp.data : "");
    bprintf(&src, "\nconst unsigned long gt_probe_size = sizeof(%s);\n", spelling);

    static const char *const kAsmArgs[] = {"-S", "-w", "-x", "c", "-DWIBO_CODEGEN=1", "-o", "-"};
    Buf asmOut = {0, 0, 0};
    runCompiler(g_ccCmd, kAsmArgs, sizeof kAsmArgs / sizeof kAsmArgs[0], g_incs, g_defs, &src, &asmOut);

    const char *text = asmOut.data ? asmOut.data : "";
    const char *label = strstr(text, "gt_probe_size:");
    if (!label)
        fatal("cannot determine size of '%s'", spelling);
    const char *dir = NULL;
    static const char *const kDirectives[] = {".long", ".quad"};
    for (size_t i = 0; i < sizeof kDirectives / sizeof kDirectives[0]; i++) {
        const char *q = strstr(label, kDirectives[i]);
        if (q && (!dir || q < dir))
            dir = q;
    }
    if (!dir)
        fatal("cannot determine size of '%s'", spelling);
    long size = strtol(dir + strlen(".long"), NULL, 10);
    if (size <= 0)
        fatal("invalid size for '%s'", spelling);

    SizeCacheEntry *e = malloc(sizeof *e);
    if (!e)
        fatal("out of memory");
    e->spelling = strdup(spelling);
    if (!e->spelling)
        fatal("out of memory");
    e->size = size;
    e->next = g_sizeCache;
    g_sizeCache = e;

    free(src.data);
    free(asmOut.data);
    return size;
}

/* ---------- Driver ---------- */

static void usage(FILE *f) {
    fprintf(f,
            "usage: gentramp --dll NAME --headers H... --arch x86|x86_64 --out-asm FILE --out-hdr FILE\n"
            "                [--guest-abi default|win64] [-I DIR]... [-D MACRO]... [--cc COMPILER]\n");
}

static void sortSigs(Sig *arr, size_t n) {
    for (size_t i = 1; i < n; i++) {
        Sig key = arr[i];
        size_t j = i;
        while (j > 0 && strcmp(arr[j - 1].name, key.name) > 0) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = key;
    }
}

static void sortVars(Var *arr, size_t n) {
    for (size_t i = 1; i < n; i++) {
        Var key = arr[i];
        size_t j = i;
        while (j > 0 && strcmp(arr[j - 1].name, key.name) > 0) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = key;
    }
}

int main(int argc, char **argv) {
    const char *dll = NULL;
    const char *archName = NULL;
    const char *outAsm = NULL;
    const char *outHdr = NULL;
    const char *cc = "cc";
    StrList headers = {0, 0, 0};
    StrList incs = {0, 0, 0};
    StrList defs = {0, 0, 0};

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--dll") && i + 1 < argc) {
            dll = argv[++i];
        } else if (!strcmp(a, "--headers")) {
            while (i + 1 < argc && argv[i + 1][0] != '-')
                slPush(&headers, argv[++i]);
        } else if (!strcmp(a, "--arch") && i + 1 < argc) {
            archName = argv[++i];
        } else if (!strcmp(a, "--guest-abi") && i + 1 < argc) {
            const char *abi = argv[++i];
            if (!strcmp(abi, "win64"))
                g_win64 = 1;
            else if (strcmp(abi, "default"))
                fatal("unsupported guest ABI: %s", abi);
        } else if (!strcmp(a, "--out-asm") && i + 1 < argc) {
            outAsm = argv[++i];
        } else if (!strcmp(a, "--out-hdr") && i + 1 < argc) {
            outHdr = argv[++i];
        } else if (!strcmp(a, "--cc") && i + 1 < argc) {
            cc = argv[++i];
        } else if (!strcmp(a, "-I") && i + 1 < argc) {
            slPush(&incs, joinOpt("-I", argv[++i]));
        } else if (!strncmp(a, "-I", 2) && a[2]) {
            slPush(&incs, joinOpt("-I", a + 2));
        } else if (!strcmp(a, "-D") && i + 1 < argc) {
            slPush(&defs, joinOpt("-D", argv[++i]));
        } else if (!strncmp(a, "-D", 2) && a[2]) {
            slPush(&defs, joinOpt("-D", a + 2));
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "gentramp: unknown or incomplete argument: %s\n", a);
            usage(stderr);
            return 2;
        }
    }

    if (!dll || headers.n == 0 || !archName || !outAsm || !outHdr) {
        usage(stderr);
        return 2;
    }
    if (!strcmp(archName, "x86")) {
        g_arch = ARCH_X86;
        g_ptrSize = 4;
        g_longSize = 4;
    } else if (!strcmp(archName, "x86_64")) {
        g_arch = ARCH_X86_64;
        g_ptrSize = 8;
        g_longSize = 8;
    } else {
        fatal("unsupported architecture: %s", archName);
    }
    if (g_win64 && g_arch != ARCH_X86_64)
        fatal("--guest-abi win64 requires --arch x86_64");
#ifdef __APPLE__
    g_darwin = 1;
#endif
    g_dll = dll;

    Buf tu = {0, 0, 0};
    for (size_t i = 0; i < headers.n; i++)
        bprintf(&tu, "#include \"%s\"\n", headers.items[i]);
    Buf pp = {0, 0, 0};
    g_ccCmd = cc;
    g_incs = &incs;
    g_defs = &defs;
    preprocess(cc, &incs, &defs, &tu, &pp);
    g_pp = pp;
    lexSource(pp.data ? pp.data : "", pp.len);
    parseTranslationUnit();

    if (g_nfuncs == 0 && g_ntds == 0 && g_nvars == 0) {
        fprintf(stderr, "No functions, typedefs, or variables found for generation.\n");
        return 1;
    }
    sortSigs(g_funcs, g_nfuncs);
    sortSigs(g_tds, g_ntds);
    sortVars(g_vars, g_nvars);

    Buf asmOut = {0, 0, 0};
    Buf hdrOut = {0, 0, 0};
    emitAsm(&asmOut, g_funcs, g_nfuncs, g_tds, g_ntds);
    emitHeader(&hdrOut, g_funcs, g_nfuncs, g_tds, g_ntds, g_vars, g_nvars);
    writeFile(outAsm, &asmOut);
    writeFile(outHdr, &hdrOut);
    return 0;
}
