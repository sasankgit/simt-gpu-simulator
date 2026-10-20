/* simt_asm.c - assembler, expression evaluator and CFG / post-dominator
 *              analysis for the SIMT ISA.  Pure C11, no external deps.      */
#include "simt_asm.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/* Tiny growable-array helper (type-generic via macros)                      */
/* ------------------------------------------------------------------------ */
#define VEC_T(T) \
    struct {     \
        T *data; \
        size_t len, cap; \
    }

#define VEC_PUSH(v, item)                                                         \
    do {                                                                          \
        if ((v).len == (v).cap) {                                                 \
            size_t ncap_ = (v).cap ? (v).cap * 2 : 16;                            \
            void *np_ = realloc((v).data, ncap_ * sizeof(*(v).data));             \
            if (!np_) {                                                           \
                fputs("simt_asm: out of memory\n", stderr);                       \
                abort();                                                          \
            }                                                                     \
            (v).data = np_;                                                       \
            (v).cap = ncap_;                                                      \
        }                                                                         \
        (v).data[(v).len++] = (item);                                             \
    } while (0)

#define VEC_FREE(v)      \
    do {                 \
        free((v).data);  \
        (v).data = NULL; \
        (v).len = (v).cap = 0; \
    } while (0)

/* ------------------------------------------------------------------------ */
/* Assembler context                                                         */
/* ------------------------------------------------------------------------ */
typedef struct {
    char name[SIMT_NAME_MAX];
    int64_t value;
} equ_t;
typedef struct {
    char name[SIMT_NAME_MAX];
    int reg;
} alias_t;
typedef struct {
    char name[SIMT_NAME_MAX];
    int32_t pc;
    int line;
} label_t;
typedef struct {
    int line;
    char *text;
} rawinst_t;

typedef struct {
    const char *src;
    simt_asm_error *err;
    int failed;
    char kernel_name[SIMT_NAME_MAX];
    uint32_t shared_bytes;
    VEC_T(equ_t) equs;
    VEC_T(alias_t) aliases;
    VEC_T(label_t) labels;
    VEC_T(rawinst_t) raw;
} asm_ctx;

static void set_error(asm_ctx *c, int line, const char *fmt, ...) {
    va_list ap;
    if (c->failed) return; /* keep the first diagnostic */
    c->failed = 1;
    c->err->line = line;
    va_start(ap, fmt);
    vsnprintf(c->err->message, sizeof c->err->message, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------------ */
/* String helpers                                                            */
/* ------------------------------------------------------------------------ */
static char *trim(char *s) {
    char *end;
    while (isspace((unsigned char)*s)) ++s;
    if (!*s) return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

static void strip_comment(char *s) {
    char *p;
    for (p = s; *p; ++p) {
        if (*p == ';' || *p == '#' || (p[0] == '/' && p[1] == '/')) {
            *p = '\0';
            return;
        }
    }
}

static int is_ident_start(int ch) { return isalpha(ch) || ch == '_'; }
static int is_ident_char(int ch) { return isalnum(ch) || ch == '_'; }

static int is_ident(const char *s) {
    if (!is_ident_start((unsigned char)*s)) return 0;
    for (++s; *s; ++s)
        if (!is_ident_char((unsigned char)*s)) return 0;
    return 1;
}

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (!d) {
        fputs("simt_asm: out of memory\n", stderr);
        abort();
    }
    memcpy(d, s, n);
    return d;
}

/* ------------------------------------------------------------------------ */
/* Symbol tables                                                             */
/* ------------------------------------------------------------------------ */
static const equ_t *find_equ(const asm_ctx *c, const char *name) {
    size_t i;
    for (i = 0; i < c->equs.len; ++i)
        if (strcmp(c->equs.data[i].name, name) == 0) return &c->equs.data[i];
    return NULL;
}

static const alias_t *find_alias(const asm_ctx *c, const char *name) {
    size_t i;
    for (i = 0; i < c->aliases.len; ++i)
        if (strcmp(c->aliases.data[i].name, name) == 0) return &c->aliases.data[i];
    return NULL;
}

static const label_t *find_label(const asm_ctx *c, const char *name) {
    size_t i;
    for (i = 0; i < c->labels.len; ++i)
        if (strcmp(c->labels.data[i].name, name) == 0) return &c->labels.data[i];
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Constant expression evaluator: recursive descent with C-like precedence   */
/*   expr   := term (('+'|'-') term)*                                        */
/*   term   := factor (('*'|'<<'|'>>') factor)*                              */
/*   factor := '-' factor | '+' factor | '(' expr ')' | number | symbol      */
/* ------------------------------------------------------------------------ */
typedef struct {
    asm_ctx *c;
    int line;
    const char *p;
    int ok;
} expr_t;

static int64_t parse_expr(expr_t *e);

static void expr_skip_ws(expr_t *e) {
    while (isspace((unsigned char)*e->p)) e->p++;
}

static int64_t parse_factor(expr_t *e) {
    expr_skip_ws(e);
    if (*e->p == '-') {
        e->p++;
        return -parse_factor(e);
    }
    if (*e->p == '+') {
        e->p++;
        return parse_factor(e);
    }
    if (*e->p == '(') {
        int64_t v;
        e->p++;
        v = parse_expr(e);
        expr_skip_ws(e);
        if (*e->p != ')') {
            set_error(e->c, e->line, "expected ')' in expression");
            e->ok = 0;
            return 0;
        }
        e->p++;
        return v;
    }
    if (isdigit((unsigned char)*e->p)) {
        char *end;
        int base = (e->p[0] == '0' && (e->p[1] == 'x' || e->p[1] == 'X')) ? 16 : 10;
        unsigned long long v = strtoull(e->p, &end, base);
        if (end == e->p) {
            set_error(e->c, e->line, "malformed number");
            e->ok = 0;
            return 0;
        }
        e->p = end;
        return (int64_t)v;
    }
    if (is_ident_start((unsigned char)*e->p)) {
        char name[SIMT_NAME_MAX];
        size_t n = 0;
        const equ_t *eq;
        while (is_ident_char((unsigned char)*e->p) && n + 1 < sizeof name) name[n++] = *e->p++;
        name[n] = '\0';
        eq = find_equ(e->c, name);
        if (!eq) {
            set_error(e->c, e->line, "undefined symbol '%s'", name);
            e->ok = 0;
            return 0;
        }
        return eq->value;
    }
    set_error(e->c, e->line, "unexpected '%c' in expression", *e->p ? *e->p : '?');
    e->ok = 0;
    return 0;
}

static int64_t parse_term(expr_t *e) {
    int64_t v = parse_factor(e);
    for (;;) {
        expr_skip_ws(e);
        if (*e->p == '*') {
            e->p++;
            v *= parse_factor(e);
        } else if (e->p[0] == '<' && e->p[1] == '<') {
            e->p += 2;
            v = (int64_t)((uint64_t)v << (parse_factor(e) & 63));
        } else if (e->p[0] == '>' && e->p[1] == '>') {
            e->p += 2;
            v >>= (parse_factor(e) & 63);
        } else {
            break;
        }
        if (!e->ok) break;
    }
    return v;
}

static int64_t parse_expr(expr_t *e) {
    int64_t v = parse_term(e);
    for (;;) {
        expr_skip_ws(e);
        if (*e->p == '+') {
            e->p++;
            v += parse_term(e);
        } else if (*e->p == '-') {
            e->p++;
            v -= parse_term(e);
        } else {
            break;
        }
        if (!e->ok) break;
    }
    return v;
}

static int eval_expr(asm_ctx *c, int line, const char *text, int64_t *out) {
    expr_t e;
    e.c = c;
    e.line = line;
    e.p = text;
    e.ok = 1;
    *out = parse_expr(&e);
    if (!e.ok) return 0;
    expr_skip_ws(&e);
    if (*e.p != '\0') {
        set_error(c, line, "trailing characters in expression '%s'", text);
        return 0;
    }
    return 1;
}

static int eval_imm32(asm_ctx *c, int line, const char *text, int32_t *out) {
    int64_t v;
    if (!eval_expr(c, line, text, &v)) return 0;
    if (v < INT32_MIN || v > (int64_t)UINT32_MAX) {
        set_error(c, line, "immediate '%s' does not fit in 32 bits", text);
        return 0;
    }
    *out = (int32_t)(uint32_t)v;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Operand parsing                                                           */
/* ------------------------------------------------------------------------ */
static int parse_raw_reg(const char *tok, int *out) {
    char *end;
    long v;
    if ((tok[0] != 'r' && tok[0] != 'R') || !isdigit((unsigned char)tok[1])) return 0;
    v = strtol(tok + 1, &end, 10);
    if (*end != '\0' || v < 0 || v >= SIMT_MAX_REGS) return 0;
    *out = (int)v;
    return 1;
}

static int parse_reg(asm_ctx *c, int line, const char *tok, uint8_t *out) {
    const alias_t *al = find_alias(c, tok);
    int r;
    if (al) {
        *out = (uint8_t)al->reg;
        return 1;
    }
    if (parse_raw_reg(tok, &r)) {
        *out = (uint8_t)r;
        return 1;
    }
    set_error(c, line, "expected a register (r0..r%d or alias), got '%s'", SIMT_MAX_REGS - 1, tok);
    return 0;
}

/* [reg], [reg + expr], [reg - expr] */
static int parse_mem(asm_ctx *c, int line, const char *tok, uint8_t *reg, int32_t *imm) {
    char buf[128];
    size_t n = strlen(tok);
    char *inner, *split;
    if (n < 3 || tok[0] != '[' || tok[n - 1] != ']' || n >= sizeof buf) {
        set_error(c, line, "malformed memory operand '%s' (expected [reg + imm])", tok);
        return 0;
    }
    memcpy(buf, tok + 1, n - 2);
    buf[n - 2] = '\0';
    inner = trim(buf);
    split = inner;
    while (*split && *split != '+' && *split != '-') ++split;
    if (*split) {
        char sign = *split;
        char exprbuf[128];
        *split = '\0';
        snprintf(exprbuf, sizeof exprbuf, "%c%s", sign, split + 1);
        if (!parse_reg(c, line, trim(inner), reg)) return 0;
        return eval_imm32(c, line, exprbuf, imm);
    }
    *imm = 0;
    return parse_reg(c, line, trim(inner), reg);
}

static int parse_float_bits(asm_ctx *c, int line, const char *tok, int32_t *out) {
    char *end;
    float f;
    double d = strtod(tok, &end);
    if (end == tok || *end != '\0') {
        set_error(c, line, "expected a floating point literal, got '%s'", tok);
        return 0;
    }
    f = (float)d;
    memcpy(out, &f, sizeof f);
    return 1;
}

/* Split "a, b, [c + d]" on commas that are not inside brackets. */
static int split_operands(char *text, char *ops[], int max_ops) {
    int n = 0, depth = 0;
    char *start = text, *p;
    if (!*trim(text)) return 0;
    for (p = text;; ++p) {
        if (*p == '[') ++depth;
        if (*p == ']') --depth;
        if ((*p == ',' && depth == 0) || *p == '\0') {
            int last = (*p == '\0');
            *p = '\0';
            if (n < max_ops) ops[n] = trim(start);
            ++n;
            start = p + 1;
            if (last) break;
        }
    }
    return n;
}

/* ------------------------------------------------------------------------ */
/* Pass 1: labels, directives, collect raw instruction text                  */
/* ------------------------------------------------------------------------ */
static void handle_directive(asm_ctx *c, int line, char *text) {
    char *name = text, *args;
    char *ops[4];
    int nops;
    while (*text && !isspace((unsigned char)*text)) ++text;
    if (*text) *text++ = '\0';
    args = trim(text);

    if (strcmp(name, ".kernel") == 0) {
        if (!is_ident(args)) {
            set_error(c, line, ".kernel expects an identifier");
            return;
        }
        snprintf(c->kernel_name, sizeof c->kernel_name, "%s", args);
    } else if (strcmp(name, ".shared") == 0) {
        int64_t v;
        if (!eval_expr(c, line, args, &v)) return;
        if (v < 0 || v > (1 << 20)) {
            set_error(c, line, ".shared size out of range");
            return;
        }
        c->shared_bytes = (uint32_t)v;
    } else if (strcmp(name, ".equ") == 0) {
        equ_t eq;
        int64_t v;
        nops = split_operands(args, ops, 4);
        if (nops != 2 || !is_ident(ops[0])) {
            set_error(c, line, ".equ expects: .equ NAME, expression");
            return;
        }
        if (!eval_expr(c, line, ops[1], &v)) return;
        snprintf(eq.name, sizeof eq.name, "%s", ops[0]);
        eq.value = v;
        VEC_PUSH(c->equs, eq);
    } else if (strcmp(name, ".reg") == 0) {
        alias_t al;
        int r;
        nops = split_operands(args, ops, 4);
        if (nops != 2 || !is_ident(ops[0]) || !parse_raw_reg(ops[1], &r)) {
            set_error(c, line, ".reg expects: .reg NAME, rN");
            return;
        }
        if (parse_raw_reg(ops[0], &r)) {
            set_error(c, line, "alias '%s' looks like a register name", ops[0]);
            return;
        }
        parse_raw_reg(ops[1], &r);
        snprintf(al.name, sizeof al.name, "%s", ops[0]);
        al.reg = r;
        VEC_PUSH(c->aliases, al);
    } else {
        set_error(c, line, "unknown directive '%s'", name);
    }
}

static void process_line(asm_ctx *c, int line, char *buf) {
    char *p;
    strip_comment(buf);
    p = trim(buf);
    /* any number of "label:" prefixes */
    for (;;) {
        char *colon = strchr(p, ':');
        char name[SIMT_NAME_MAX];
        size_t n;
        label_t lb;
        if (!colon) break;
        n = (size_t)(colon - p);
        if (n == 0 || n >= sizeof name) break;
        memcpy(name, p, n);
        name[n] = '\0';
        if (!is_ident(trim(name))) break;
        if (find_label(c, trim(name))) {
            set_error(c, line, "label '%s' defined twice", trim(name));
            return;
        }
        snprintf(lb.name, sizeof lb.name, "%s", trim(name));
        lb.pc = (int32_t)c->raw.len;
        lb.line = line;
        VEC_PUSH(c->labels, lb);
        p = trim(colon + 1);
    }
    if (!*p) return;
    if (*p == '.') {
        handle_directive(c, line, p);
        return;
    }
    {
        rawinst_t ri;
        ri.line = line;
        ri.text = dup_str(p);
        VEC_PUSH(c->raw, ri);
    }
}

static void pass1(asm_ctx *c) {
    const char *s = c->src;
    int line = 0;
    while (*s && !c->failed) {
        const char *eol = strchr(s, '\n');
        size_t len = eol ? (size_t)(eol - s) : strlen(s);
        char *buf = (char *)malloc(len + 1);
        if (!buf) abort();
        memcpy(buf, s, len);
        buf[len] = '\0';
        ++line;
        process_line(c, line, buf);
        free(buf);
        s += len;
        if (*s == '\n') ++s;
    }
}

/* ------------------------------------------------------------------------ */
/* Pass 2: encode instructions                                               */
/* ------------------------------------------------------------------------ */
static int resolve_label(asm_ctx *c, int line, const char *name, int32_t *pc) {
    const label_t *lb = find_label(c, name);
    if (!lb) {
        set_error(c, line, "undefined label '%s'", name);
        return 0;
    }
    if ((size_t)lb->pc >= c->raw.len) {
        set_error(c, line, "label '%s' points past the last instruction", name);
        return 0;
    }
    *pc = lb->pc;
    return 1;
}

static int need_ops(asm_ctx *c, int line, const char *mn, int got, int want) {
    if (got != want) {
        set_error(c, line, "'%s' expects %d operand(s), got %d", mn, want, got);
        return 0;
    }
    return 1;
}

static int encode_one(asm_ctx *c, const rawinst_t *ri, simt_inst *out) {
    char text[256];
    char *mn, *rest, *ops[6];
    int nops, opc;
    const simt_opinfo *oi;
    int line = ri->line;

    if (strlen(ri->text) >= sizeof text) {
        set_error(c, line, "instruction too long");
        return 0;
    }
    strcpy(text, ri->text);
    mn = text;
    rest = text;
    while (*rest && !isspace((unsigned char)*rest)) ++rest;
    if (*rest) *rest++ = '\0';
    nops = split_operands(rest, ops, 6);
    if (nops > 6) {
        set_error(c, line, "too many operands");
        return 0;
    }

    opc = simt_opcode_from_name(mn);
    if (opc < 0) {
        set_error(c, line, "unknown mnemonic '%s'", mn);
        return 0;
    }
    oi = simt_opinfo_get((simt_opcode)opc);
    memset(out, 0, sizeof *out);
    out->op = (uint8_t)opc;
    out->target = -1;
    out->reconv = -1;
    out->line = line;

    switch (oi->fmt) {
    case FMT_NONE:
        return need_ops(c, line, mn, nops, 0);
    case FMT_RD_IMM:
        if (!need_ops(c, line, mn, nops, 2)) return 0;
        if (!parse_reg(c, line, ops[0], &out->rd)) return 0;
        if (!eval_imm32(c, line, ops[1], &out->imm)) return 0;
        if (opc == OP_LDP && (out->imm < 0 || out->imm >= SIMT_MAX_PARAMS)) {
            set_error(c, line, "parameter index out of range (0..%d)", SIMT_MAX_PARAMS - 1);
            return 0;
        }
        return 1;
    case FMT_RD_FIMM:
        if (!need_ops(c, line, mn, nops, 2)) return 0;
        return parse_reg(c, line, ops[0], &out->rd) && parse_float_bits(c, line, ops[1], &out->imm);
    case FMT_RD_RA:
        if (!need_ops(c, line, mn, nops, 2)) return 0;
        return parse_reg(c, line, ops[0], &out->rd) && parse_reg(c, line, ops[1], &out->ra);
    case FMT_RD_SR: {
        int sr;
        if (!need_ops(c, line, mn, nops, 2)) return 0;
        if (!parse_reg(c, line, ops[0], &out->rd)) return 0;
        sr = simt_sreg_from_name(ops[1]);
        if (sr < 0) {
            set_error(c, line, "unknown special register '%s'", ops[1]);
            return 0;
        }
        out->sr = (uint8_t)sr;
        return 1;
    }
    case FMT_RD_RA_RB:
        if (!need_ops(c, line, mn, nops, 3)) return 0;
        return parse_reg(c, line, ops[0], &out->rd) && parse_reg(c, line, ops[1], &out->ra) &&
               parse_reg(c, line, ops[2], &out->rb);
    case FMT_RD_RA_IMM:
        if (!need_ops(c, line, mn, nops, 3)) return 0;
        return parse_reg(c, line, ops[0], &out->rd) && parse_reg(c, line, ops[1], &out->ra) &&
               eval_imm32(c, line, ops[2], &out->imm);
    case FMT_RD_RA_RB_RC:
        if (!need_ops(c, line, mn, nops, 4)) return 0;
        return parse_reg(c, line, ops[0], &out->rd) && parse_reg(c, line, ops[1], &out->ra) &&
               parse_reg(c, line, ops[2], &out->rb) && parse_reg(c, line, ops[3], &out->rc);
    case FMT_RD_MEM:
        if (!need_ops(c, line, mn, nops, 2)) return 0;
        return parse_reg(c, line, ops[0], &out->rd) && parse_mem(c, line, ops[1], &out->ra, &out->imm);
    case FMT_MEM_RS:
        if (!need_ops(c, line, mn, nops, 2)) return 0;
        return parse_mem(c, line, ops[0], &out->ra, &out->imm) && parse_reg(c, line, ops[1], &out->rb);
    case FMT_RD_MEM_RS:
        if (!need_ops(c, line, mn, nops, 3)) return 0;
        return parse_reg(c, line, ops[0], &out->rd) && parse_mem(c, line, ops[1], &out->ra, &out->imm) &&
               parse_reg(c, line, ops[2], &out->rb);
    case FMT_LABEL:
        if (!need_ops(c, line, mn, nops, 1)) return 0;
        return resolve_label(c, line, ops[0], &out->target);
    case FMT_RA_LABEL:
        if (!need_ops(c, line, mn, nops, 2)) return 0;
        return parse_reg(c, line, ops[0], &out->ra) && resolve_label(c, line, ops[1], &out->target);
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* CFG analysis                                                              */
/* ------------------------------------------------------------------------ */
unsigned simt_popcount64(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (unsigned)((x * 0x0101010101010101ULL) >> 56);
}

/* Successors of instruction i; node n is the virtual EXIT node. */
static int successors(const simt_inst *code, uint32_t n, uint32_t i, uint32_t out[2]) {
    switch (code[i].op) {
    case OP_EXIT:
        out[0] = n;
        return 1;
    case OP_BRA:
        out[0] = (uint32_t)code[i].target;
        return 1;
    case OP_BNZ:
    case OP_BZ:
        out[0] = (uint32_t)code[i].target;
        out[1] = i + 1;
        return 2;
    default:
        out[0] = i + 1;
        return 1;
    }
}

static int analyze_cfg(asm_ctx *c, simt_program *prog) {
    const uint32_t n = prog->ncode;
    const simt_inst *code = prog->code;
    const size_t words = ((size_t)n + 1 + 63) / 64;
    uint8_t *reach = (uint8_t *)calloc(n + 1, 1);
    uint8_t *to_exit = (uint8_t *)calloc(n + 1, 1);
    uint32_t *stack = (uint32_t *)malloc((n + 1) * sizeof(uint32_t));
    uint64_t *pd = (uint64_t *)calloc((size_t)(n + 1) * words, sizeof(uint64_t));
    uint32_t i, sp = 0;
    int changed, ok = 1;

    if (!reach || !to_exit || !stack || !pd) abort();

    /* the last instruction must not fall off the end of the program */
    if (code[n - 1].op != OP_EXIT && code[n - 1].op != OP_BRA) {
        set_error(c, code[n - 1].line, "control may fall off the end of the kernel (end with 'exit')");
        ok = 0;
        goto done;
    }

    /* reachability from the entry point (iterative DFS) */
    stack[sp++] = 0;
    reach[0] = 1;
    while (sp) {
        uint32_t cur = stack[--sp], s[2];
        int k, ns;
        if (cur == n) continue;
        ns = successors(code, n, cur, s);
        for (k = 0; k < ns; ++k)
            if (!reach[s[k]]) {
                reach[s[k]] = 1;
                stack[sp++] = s[k];
            }
    }
    for (i = 0; i < n; ++i)
        if (!reach[i]) prog->unreachable++;

    /* which nodes can reach EXIT (backward fixed point) */
    to_exit[n] = 1;
    do {
        changed = 0;
        for (i = 0; i < n; ++i) {
            uint32_t s[2];
            int k, ns = successors(code, n, i, s);
            if (to_exit[i]) continue;
            for (k = 0; k < ns; ++k)
                if (to_exit[s[k]]) {
                    to_exit[i] = 1;
                    changed = 1;
                    break;
                }
        }
    } while (changed);
    for (i = 0; i < n; ++i)
        if (reach[i] && !to_exit[i]) {
            set_error(c, code[i].line, "execution can never reach 'exit' from here (infinite loop?)");
            ok = 0;
            goto done;
        }

    /* post-dominator sets:  pdom(n)={n};  pdom(i)={i} U  (intersection pdom(succ)) */
    {
        uint32_t b;
        size_t w;
        for (i = 0; i < n; ++i)
            for (b = 0; b <= n; ++b) pd[(size_t)i * words + b / 64] |= 1ULL << (b % 64);
        pd[(size_t)n * words + n / 64] |= 1ULL << (n % 64);

        do {
            changed = 0;
            for (i = n; i-- > 0;) {
                uint32_t s[2];
                int k, ns = successors(code, n, i, s);
                uint64_t *row = &pd[(size_t)i * words];
                for (w = 0; w < words; ++w) {
                    uint64_t acc = ~0ULL;
                    for (k = 0; k < ns; ++k) acc &= pd[(size_t)s[k] * words + w];
                    if (w == i / 64) acc |= 1ULL << (i % 64);
                    if (acc != row[w]) {
                        row[w] = acc;
                        changed = 1;
                    }
                }
            }
        } while (changed);

        /* immediate post-dominator = strict post-dominator with the largest
         * post-dominator set (they form a chain towards EXIT).               */
        for (i = 0; i < n; ++i) {
            uint32_t best = n, d;
            unsigned best_pop = 0;
            if (!simt_inst_is_cond_branch(&code[i])) continue;
            prog->nbranches++;
            if (!reach[i]) {
                prog->code[i].reconv = (int32_t)n;
                continue;
            }
            for (d = 0; d <= n; ++d) {
                unsigned pop = 0;
                size_t ww;
                if (d == i || !((pd[(size_t)i * words + d / 64] >> (d % 64)) & 1ULL)) continue;
                for (ww = 0; ww < words; ++ww) pop += simt_popcount64(pd[(size_t)d * words + ww]);
                if (pop >= best_pop) {
                    best_pop = pop;
                    best = d;
                }
            }
            prog->code[i].reconv = (int32_t)best;
        }
    }

done:
    free(reach);
    free(to_exit);
    free(stack);
    free(pd);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* Public API                                                                */
/* ------------------------------------------------------------------------ */
void simt_program_free(simt_program *p) {
    uint32_t i;
    if (!p) return;
    if (p->text)
        for (i = 0; i < p->ncode; ++i) free(p->text[i]);
    if (p->label)
        for (i = 0; i < p->ncode; ++i) free(p->label[i]);
    free(p->text);
    free(p->label);
    free(p->code);
    free(p);
}

static void ctx_free(asm_ctx *c) {
    size_t i;
    for (i = 0; i < c->raw.len; ++i) free(c->raw.data[i].text);
    VEC_FREE(c->raw);
    VEC_FREE(c->equs);
    VEC_FREE(c->aliases);
    VEC_FREE(c->labels);
}

simt_program *simt_assemble(const char *source, simt_asm_error *err) {
    asm_ctx c;
    simt_program *prog = NULL;
    size_t i;
    uint32_t maxreg = 0;

    memset(&c, 0, sizeof c);
    c.src = source;
    c.err = err;
    err->line = 0;
    err->message[0] = '\0';
    snprintf(c.kernel_name, sizeof c.kernel_name, "kernel");

    pass1(&c);
    if (c.failed) goto fail;
    if (c.raw.len == 0) {
        set_error(&c, 0, "kernel contains no instructions");
        goto fail;
    }

    prog = (simt_program *)calloc(1, sizeof *prog);
    if (!prog) abort();
    prog->ncode = (uint32_t)c.raw.len;
    prog->code = (simt_inst *)calloc(c.raw.len, sizeof(simt_inst));
    prog->text = (char **)calloc(c.raw.len, sizeof(char *));
    prog->label = (char **)calloc(c.raw.len, sizeof(char *));
    if (!prog->code || !prog->text || !prog->label) abort();
    snprintf(prog->name, sizeof prog->name, "%s", c.kernel_name);
    prog->shared_bytes = c.shared_bytes;

    for (i = 0; i < c.raw.len; ++i) {
        simt_inst *in = &prog->code[i];
        uint8_t srcs[3];
        int ns, k, dst;
        if (!encode_one(&c, &c.raw.data[i], in)) goto fail;
        prog->text[i] = dup_str(c.raw.data[i].text);
        dst = simt_inst_dst(in);
        if (dst >= 0 && (uint32_t)dst > maxreg) maxreg = (uint32_t)dst;
        ns = simt_inst_srcs(in, srcs);
        for (k = 0; k < ns; ++k)
            if (srcs[k] > maxreg) maxreg = srcs[k];
    }
    prog->nregs = maxreg + 1;

    for (i = 0; i < c.labels.len; ++i) {
        uint32_t pc = (uint32_t)c.labels.data[i].pc;
        if (pc < prog->ncode && !prog->label[pc]) prog->label[pc] = dup_str(c.labels.data[i].name);
    }

    if (!analyze_cfg(&c, prog)) goto fail;

    ctx_free(&c);
    return prog;

fail:
    simt_program_free(prog);
    ctx_free(&c);
    return NULL;
}
