/*
 * simt_isa.h - instruction set definition shared by the C assembler and the
 *              C++ simulator.
 *
 * The ISA is a small, SASS-flavoured, 32-bit, warp-oriented instruction set:
 * 64 architectural registers per thread, integer + IEEE-754 single precision
 * arithmetic, global/shared memory ops, atomics, barriers and branches whose
 * reconvergence points are computed by the assembler (immediate
 * post-dominator analysis).
 *
 * This header is valid C11 and C++17 (extern "C" guarded).
 */
#ifndef SIMT_ISA_H
#define SIMT_ISA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIMT_WARP_SIZE  32
#define SIMT_MAX_REGS   64
#define SIMT_NAME_MAX   64
#define SIMT_MAX_PARAMS 16

typedef enum simt_opcode {
    OP_NOP = 0,
    OP_EXIT,
    OP_BAR,
    /* data movement */
    OP_MOVI,  /* rd = imm                    */
    OP_FMOVI, /* rd = float immediate        */
    OP_MOV,   /* rd = ra                     */
    OP_S2R,   /* rd = special register       */
    OP_LDP,   /* rd = kernel parameter[imm]  */
    /* integer ALU */
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_UDIV,
    OP_UREM,
    OP_AND,
    OP_OR,
    OP_XOR,
    OP_SHL,
    OP_SHR,
    OP_ADDI,
    OP_MULI,
    OP_SHLI,
    OP_SHRI,
    OP_ANDI,
    /* comparisons produce 0/1, SEL is a predicated move */
    OP_SLT,
    OP_SLE,
    OP_SEQ,
    OP_SNE,
    OP_SEL, /* rd = ra ? rb : rc */
    /* single precision */
    OP_FADD,
    OP_FSUB,
    OP_FMUL,
    OP_FFMA, /* rd = ra * rb + rc (fused) */
    OP_FMAX,
    OP_FMIN,
    OP_FSLT,
    OP_FSLE,
    OP_I2F,
    OP_F2I,
    OP_FEXP, /* special function unit */
    OP_FRCP, /* special function unit */
    /* memory */
    OP_LDG,
    OP_STG,
    OP_LDS,
    OP_STS,
    OP_ATOMADD, /* rd = old value, global[addr] += rb */
    /* control flow */
    OP_BRA,
    OP_BNZ,
    OP_BZ,
    OP_COUNT
} simt_opcode;

typedef enum simt_sreg {
    SR_TID = 0, /* thread index within block        */
    SR_BID,     /* block index within grid          */
    SR_BDIM,    /* threads per block                */
    SR_GDIM,    /* blocks per grid                  */
    SR_LANE,    /* lane within warp                 */
    SR_WARP,    /* warp index within block          */
    SR_GTID,    /* bid * bdim + tid                 */
    SR_COUNT
} simt_sreg;

typedef enum simt_unit {
    UNIT_ALU = 0,
    UNIT_FMA,
    UNIT_SFU,
    UNIT_LSU,
    UNIT_CTRL,
    UNIT_COUNT
} simt_unit;

/* Operand pattern of an instruction; drives parsing, disassembly and the
 * scoreboard's notion of which registers are read / written. */
typedef enum simt_fmt {
    FMT_NONE = 0,
    FMT_RD_IMM,
    FMT_RD_FIMM,
    FMT_RD_RA,
    FMT_RD_SR,
    FMT_RD_RA_RB,
    FMT_RD_RA_IMM,
    FMT_RD_RA_RB_RC,
    FMT_RD_MEM,    /* rd, [ra + imm]            */
    FMT_MEM_RS,    /* [ra + imm], rb            */
    FMT_RD_MEM_RS, /* rd, [ra + imm], rb        */
    FMT_LABEL,     /* label                     */
    FMT_RA_LABEL   /* ra, label                 */
} simt_fmt;

typedef struct simt_opinfo {
    const char *name;
    simt_fmt fmt;
    simt_unit unit;
    uint8_t latency;  /* result latency in cycles (0: decided by memory system) */
    uint8_t interval; /* cycles the issuing unit stays busy                    */
} simt_opinfo;

/* One decoded instruction. */
typedef struct simt_inst {
    uint8_t op;
    uint8_t rd, ra, rb, rc;
    uint8_t sr;
    uint8_t pad0, pad1;
    int32_t imm;
    int32_t target; /* resolved branch target (pc) or -1                      */
    int32_t reconv; /* conditional branches: immediate post-dominator pc      */
    int32_t line;   /* 1-based source line for diagnostics                    */
} simt_inst;

const simt_opinfo *simt_opinfo_get(simt_opcode op);
int simt_opcode_from_name(const char *name);
const char *simt_sreg_name(simt_sreg sr);
int simt_sreg_from_name(const char *name);

/* Destination register of an instruction or -1. */
int simt_inst_dst(const simt_inst *in);
/* Fills out[] with source registers, returns how many (0..3). */
int simt_inst_srcs(const simt_inst *in, uint8_t out[3]);
int simt_inst_is_cond_branch(const simt_inst *in);
int simt_inst_is_mem(const simt_inst *in);

/* Human readable form, e.g. "ldg r4, [r2 + 16]". */
void simt_disasm(const simt_inst *in, char *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* SIMT_ISA_H */
