/* simt_isa.c - opcode tables, operand queries and the disassembler. */
#include "simt_isa.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* name, operand format, functional unit, latency, issue interval */
static const simt_opinfo k_opinfo[OP_COUNT] = {
    [OP_NOP] = {"nop", FMT_NONE, UNIT_CTRL, 1, 1},
    [OP_EXIT] = {"exit", FMT_NONE, UNIT_CTRL, 1, 1},
    [OP_BAR] = {"bar", FMT_NONE, UNIT_CTRL, 1, 1},

    [OP_MOVI] = {"movi", FMT_RD_IMM, UNIT_ALU, 4, 1},
    [OP_FMOVI] = {"fmovi", FMT_RD_FIMM, UNIT_ALU, 4, 1},
    [OP_MOV] = {"mov", FMT_RD_RA, UNIT_ALU, 4, 1},
    [OP_S2R] = {"s2r", FMT_RD_SR, UNIT_ALU, 4, 1},
    [OP_LDP] = {"ldp", FMT_RD_IMM, UNIT_ALU, 4, 1},

    [OP_ADD] = {"add", FMT_RD_RA_RB, UNIT_ALU, 4, 1},
    [OP_SUB] = {"sub", FMT_RD_RA_RB, UNIT_ALU, 4, 1},
    [OP_MUL] = {"mul", FMT_RD_RA_RB, UNIT_FMA, 5, 1},
    [OP_UDIV] = {"udiv", FMT_RD_RA_RB, UNIT_SFU, 36, 8},
    [OP_UREM] = {"urem", FMT_RD_RA_RB, UNIT_SFU, 36, 8},
    [OP_AND] = {"and", FMT_RD_RA_RB, UNIT_ALU, 4, 1},
    [OP_OR] = {"or", FMT_RD_RA_RB, UNIT_ALU, 4, 1},
    [OP_XOR] = {"xor", FMT_RD_RA_RB, UNIT_ALU, 4, 1},
    [OP_SHL] = {"shl", FMT_RD_RA_RB, UNIT_ALU, 4, 1},
    [OP_SHR] = {"shr", FMT_RD_RA_RB, UNIT_ALU, 4, 1},
    [OP_ADDI] = {"addi", FMT_RD_RA_IMM, UNIT_ALU, 4, 1},
    [OP_MULI] = {"muli", FMT_RD_RA_IMM, UNIT_FMA, 5, 1},
    [OP_SHLI] = {"shli", FMT_RD_RA_IMM, UNIT_ALU, 4, 1},
    [OP_SHRI] = {"shri", FMT_RD_RA_IMM, UNIT_ALU, 4, 1},
    [OP_ANDI] = {"andi", FMT_RD_RA_IMM, UNIT_ALU, 4, 1},

    [OP_SLT] = {"slt", FMT_RD_RA_RB, UNIT_ALU, 4, 1},
    [OP_SLE] = {"sle", FMT_RD_RA_RB, UNIT_ALU, 4, 1},
    [OP_SEQ] = {"seq", FMT_RD_RA_RB, UNIT_ALU, 4, 1},
    [OP_SNE] = {"sne", FMT_RD_RA_RB, UNIT_ALU, 4, 1},
    [OP_SEL] = {"sel", FMT_RD_RA_RB_RC, UNIT_ALU, 4, 1},

    [OP_FADD] = {"fadd", FMT_RD_RA_RB, UNIT_FMA, 4, 1},
    [OP_FSUB] = {"fsub", FMT_RD_RA_RB, UNIT_FMA, 4, 1},
    [OP_FMUL] = {"fmul", FMT_RD_RA_RB, UNIT_FMA, 4, 1},
    [OP_FFMA] = {"ffma", FMT_RD_RA_RB_RC, UNIT_FMA, 4, 1},
    [OP_FMAX] = {"fmax", FMT_RD_RA_RB, UNIT_FMA, 4, 1},
    [OP_FMIN] = {"fmin", FMT_RD_RA_RB, UNIT_FMA, 4, 1},
    [OP_FSLT] = {"fslt", FMT_RD_RA_RB, UNIT_FMA, 4, 1},
    [OP_FSLE] = {"fsle", FMT_RD_RA_RB, UNIT_FMA, 4, 1},
    [OP_I2F] = {"i2f", FMT_RD_RA, UNIT_ALU, 6, 1},
    [OP_F2I] = {"f2i", FMT_RD_RA, UNIT_ALU, 6, 1},
    [OP_FEXP] = {"fexp", FMT_RD_RA, UNIT_SFU, 20, 8},
    [OP_FRCP] = {"frcp", FMT_RD_RA, UNIT_SFU, 20, 8},

    [OP_LDG] = {"ldg", FMT_RD_MEM, UNIT_LSU, 0, 1},
    [OP_STG] = {"stg", FMT_MEM_RS, UNIT_LSU, 0, 1},
    [OP_LDS] = {"lds", FMT_RD_MEM, UNIT_LSU, 0, 1},
    [OP_STS] = {"sts", FMT_MEM_RS, UNIT_LSU, 0, 1},
    [OP_ATOMADD] = {"atomadd", FMT_RD_MEM_RS, UNIT_LSU, 0, 1},

    [OP_BRA] = {"bra", FMT_LABEL, UNIT_CTRL, 1, 1},
    [OP_BNZ] = {"bnz", FMT_RA_LABEL, UNIT_CTRL, 1, 1},
    [OP_BZ] = {"bz", FMT_RA_LABEL, UNIT_CTRL, 1, 1},
};

static const char *const k_sreg_names[SR_COUNT] = {
    [SR_TID] = "tid", [SR_BID] = "bid", [SR_BDIM] = "bdim", [SR_GDIM] = "gdim",
    [SR_LANE] = "lane", [SR_WARP] = "warp", [SR_GTID] = "gtid",
};

const simt_opinfo *simt_opinfo_get(simt_opcode op) {
    if ((unsigned)op >= (unsigned)OP_COUNT) return NULL;
    return &k_opinfo[op];
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

int simt_opcode_from_name(const char *name) {
    int i;
    for (i = 0; i < OP_COUNT; ++i)
        if (ieq(k_opinfo[i].name, name)) return i;
    return -1;
}

const char *simt_sreg_name(simt_sreg sr) {
    if ((unsigned)sr >= (unsigned)SR_COUNT) return "?";
    return k_sreg_names[sr];
}

int simt_sreg_from_name(const char *name) {
    int i;
    for (i = 0; i < SR_COUNT; ++i)
        if (ieq(k_sreg_names[i], name)) return i;
    return -1;
}

int simt_inst_dst(const simt_inst *in) {
    switch (k_opinfo[in->op].fmt) {
    case FMT_RD_IMM:
    case FMT_RD_FIMM:
    case FMT_RD_RA:
    case FMT_RD_SR:
    case FMT_RD_RA_RB:
    case FMT_RD_RA_IMM:
    case FMT_RD_RA_RB_RC:
    case FMT_RD_MEM:
    case FMT_RD_MEM_RS:
        return in->rd;
    default:
        return -1;
    }
}

int simt_inst_srcs(const simt_inst *in, uint8_t out[3]) {
    switch (k_opinfo[in->op].fmt) {
    case FMT_RD_RA:
    case FMT_RD_RA_IMM:
    case FMT_RD_MEM:
    case FMT_RA_LABEL:
        out[0] = in->ra;
        return 1;
    case FMT_RD_RA_RB:
    case FMT_MEM_RS:
    case FMT_RD_MEM_RS:
        out[0] = in->ra;
        out[1] = in->rb;
        return 2;
    case FMT_RD_RA_RB_RC:
        out[0] = in->ra;
        out[1] = in->rb;
        out[2] = in->rc;
        return 3;
    default:
        return 0;
    }
}

int simt_inst_is_cond_branch(const simt_inst *in) {
    return in->op == OP_BNZ || in->op == OP_BZ;
}

int simt_inst_is_mem(const simt_inst *in) {
    return k_opinfo[in->op].unit == UNIT_LSU;
}

void simt_disasm(const simt_inst *in, char *buf, size_t n) {
    const simt_opinfo *oi = &k_opinfo[in->op];
    switch (oi->fmt) {
    case FMT_NONE:
        snprintf(buf, n, "%s", oi->name);
        break;
    case FMT_RD_IMM:
        snprintf(buf, n, "%s r%u, %d", oi->name, in->rd, in->imm);
        break;
    case FMT_RD_FIMM: {
        union {
            uint32_t u;
            float f;
        } cv;
        cv.u = (uint32_t)in->imm;
        snprintf(buf, n, "%s r%u, %g", oi->name, in->rd, (double)cv.f);
        break;
    }
    case FMT_RD_RA:
        snprintf(buf, n, "%s r%u, r%u", oi->name, in->rd, in->ra);
        break;
    case FMT_RD_SR:
        snprintf(buf, n, "%s r%u, %s", oi->name, in->rd, simt_sreg_name((simt_sreg)in->sr));
        break;
    case FMT_RD_RA_RB:
        snprintf(buf, n, "%s r%u, r%u, r%u", oi->name, in->rd, in->ra, in->rb);
        break;
    case FMT_RD_RA_IMM:
        snprintf(buf, n, "%s r%u, r%u, %d", oi->name, in->rd, in->ra, in->imm);
        break;
    case FMT_RD_RA_RB_RC:
        snprintf(buf, n, "%s r%u, r%u, r%u, r%u", oi->name, in->rd, in->ra, in->rb, in->rc);
        break;
    case FMT_RD_MEM:
        snprintf(buf, n, "%s r%u, [r%u %c %d]", oi->name, in->rd, in->ra,
                 in->imm < 0 ? '-' : '+', in->imm < 0 ? -in->imm : in->imm);
        break;
    case FMT_MEM_RS:
        snprintf(buf, n, "%s [r%u %c %d], r%u", oi->name, in->ra,
                 in->imm < 0 ? '-' : '+', in->imm < 0 ? -in->imm : in->imm, in->rb);
        break;
    case FMT_RD_MEM_RS:
        snprintf(buf, n, "%s r%u, [r%u %c %d], r%u", oi->name, in->rd, in->ra,
                 in->imm < 0 ? '-' : '+', in->imm < 0 ? -in->imm : in->imm, in->rb);
        break;
    case FMT_LABEL:
        snprintf(buf, n, "%s @%d", oi->name, in->target);
        break;
    case FMT_RA_LABEL:
        snprintf(buf, n, "%s r%u, @%d", oi->name, in->ra, in->target);
        break;
    }
}
