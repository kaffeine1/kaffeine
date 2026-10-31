/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    mathieeedoubbas.library, mathieeedoubtrans.library and
    mathieeesingbas.library thunks for 68k callers. On the m68k, an IEEE 754
    double travels in two data registers: the high 32 bits in D0 (or D2) and
    the low 32 bits in D1 (or D3). A single travels in one, D0 (or D1).
*/
#ifndef KAFFEINE_THUNKS_MATHIEEE_H
#define KAFFEINE_THUNKS_MATHIEEE_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

/* mathieeedoubbas.library */
IPTR kaffeine_mathieeedoubbas_IEEEDPFix(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubbas_IEEEDPFlt(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubbas_IEEEDPCmp(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubbas_IEEEDPTst(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubbas_IEEEDPAbs(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubbas_IEEEDPNeg(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubbas_IEEEDPAdd(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubbas_IEEEDPSub(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubbas_IEEEDPMul(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubbas_IEEEDPDiv(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubbas_IEEEDPFloor(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubbas_IEEEDPCeil(struct KaffeineContext *ctx, void *cpu);

/* mathieeesingbas.library */
IPTR kaffeine_mathieeesingbas_IEEESPFix(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeesingbas_IEEESPFlt(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeesingbas_IEEESPCmp(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeesingbas_IEEESPTst(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeesingbas_IEEESPAbs(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeesingbas_IEEESPNeg(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeesingbas_IEEESPAdd(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeesingbas_IEEESPSub(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeesingbas_IEEESPMul(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeesingbas_IEEESPDiv(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeesingbas_IEEESPFloor(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeesingbas_IEEESPCeil(struct KaffeineContext *ctx, void *cpu);

/* mathieeedoubtrans.library */
IPTR kaffeine_mathieeedoubtrans_IEEEDPAtan(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubtrans_IEEEDPSin(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubtrans_IEEEDPCos(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubtrans_IEEEDPTan(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubtrans_IEEEDPSincos(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubtrans_IEEEDPSinh(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubtrans_IEEEDPCosh(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubtrans_IEEEDPTanh(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubtrans_IEEEDPExp(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubtrans_IEEEDPLog(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubtrans_IEEEDPPow(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubtrans_IEEEDPSqrt(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubtrans_IEEEDPTieee(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubtrans_IEEEDPFieee(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubtrans_IEEEDPAsin(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubtrans_IEEEDPAcos(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathieeedoubtrans_IEEEDPLog10(struct KaffeineContext *ctx, void *cpu);

extern const struct M68KThunkEntry kaffeine_thunks_mathieeedoubbas[];
extern const ULONG kaffeine_thunks_mathieeedoubbas_count;

extern const struct M68KThunkEntry kaffeine_thunks_mathieeedoubtrans[];
extern const ULONG kaffeine_thunks_mathieeedoubtrans_count;

extern const struct M68KThunkEntry kaffeine_thunks_mathieeesingbas[];
extern const ULONG kaffeine_thunks_mathieeesingbas_count;

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_THUNKS_MATHIEEE_H */
