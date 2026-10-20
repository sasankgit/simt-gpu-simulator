/*
 * simt_asm.h - two-pass assembler for the SIMT ISA (pure C11).
 *
 * Source syntax (one kernel per translation unit):
 *
 *     .kernel name          ; kernel name (default "kernel")
 *     .shared 4096          ; static shared memory in bytes (constant expr)
 *     .equ    TILE, 16      ; named constant (expressions: + - * << >> ( ))
 *     .reg    acc, r7       ; register alias
 *   loop:                   ; label
 *     ffma acc, a, b, acc   ; instructions, operands separated by commas
 *     ldg  x, [ptr + 16]    ; memory operand: [reg], [reg + expr], [reg - expr]
 *     bnz  p, loop
 *     exit
 *
 * Besides encoding, the assembler builds the control-flow graph of the
 * kernel and computes, for every conditional branch, its immediate
 * post-dominator.  The simulator uses that PC as the reconvergence point of
 * the SIMT stack (the same technique GPGPU-Sim uses).
 */
#ifndef SIMT_ASM_H
#define SIMT_ASM_H

#include "simt_isa.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct simt_program {
    char name[SIMT_NAME_MAX];
    simt_inst *code;
    uint32_t ncode;
    uint32_t nregs;        /* highest register used + 1               */
    uint32_t shared_bytes; /* static shared memory per block          */
    uint32_t nbranches;    /* conditional branches                    */
    uint32_t unreachable;  /* instructions not reachable from entry   */
    char **text;           /* normalised source text per instruction  */
    char **label;          /* first label attached to a pc, or NULL   */
} simt_program;

typedef struct simt_asm_error {
    int line; /* 1-based, 0 if not tied to a line */
    char message[256];
} simt_asm_error;

/* Assemble a NUL-terminated source string. Returns NULL on failure and
 * fills *err.  The result must be released with simt_program_free(). */
simt_program *simt_assemble(const char *source, simt_asm_error *err);
void simt_program_free(simt_program *p);

/* Number of set bits; exposed because the unit tests exercise it. */
unsigned simt_popcount64(uint64_t x);

#ifdef __cplusplus
}
#endif

#endif /* SIMT_ASM_H */
