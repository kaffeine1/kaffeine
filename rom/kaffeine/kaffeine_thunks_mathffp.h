/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    mathffp.library's Motorola FFP register interface, not IEEE single.
*/
#ifndef KAFFEINE_THUNKS_MATHFFP_H
#define KAFFEINE_THUNKS_MATHFFP_H

#include "kaffeine_thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Each result is returned in D0 by the dispatcher. These thunks preserve
   D1-D7 and A0-A7; the dispatcher itself pops the guest return address.
   Unary arguments use D0 except SPTst (D1). Binary arguments use D1/D0:
   SPCmp compares D1 with D0, SPSub computes D0-D1, SPDiv computes D0/D1.
   Only guest CCR N/Z/V are changed; the other SR bits are preserved. */
IPTR kaffeine_mathffp_SPFix(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathffp_SPFlt(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathffp_SPCmp(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathffp_SPTst(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathffp_SPAbs(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathffp_SPNeg(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathffp_SPAdd(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathffp_SPSub(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathffp_SPMul(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathffp_SPDiv(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathffp_SPFloor(struct KaffeineContext *ctx, void *cpu);
IPTR kaffeine_mathffp_SPCeil(struct KaffeineContext *ctx, void *cpu);

extern const struct M68KThunkEntry kaffeine_thunks_mathffp[];
extern const ULONG kaffeine_thunks_mathffp_count;

#ifdef __cplusplus
}
#endif

/* Literal offsets checked against mathffp_lib.fd and mathffp.conf.
   The registration owner supplies the table and its sizeof-derived count;
   host fixtures instantiate this same initializer without OpenLibrary. */
#define KAFFEINE_MATHFFP_THUNKS \
    { 30, kaffeine_mathffp_SPFix }, \
    { 36, kaffeine_mathffp_SPFlt }, \
    { 42, kaffeine_mathffp_SPCmp }, \
    { 48, kaffeine_mathffp_SPTst }, \
    { 54, kaffeine_mathffp_SPAbs }, \
    { 60, kaffeine_mathffp_SPNeg }, \
    { 66, kaffeine_mathffp_SPAdd }, \
    { 72, kaffeine_mathffp_SPSub }, \
    { 78, kaffeine_mathffp_SPMul }, \
    { 84, kaffeine_mathffp_SPDiv }, \
    { 90, kaffeine_mathffp_SPFloor }, \
    { 96, kaffeine_mathffp_SPCeil }

#endif /* KAFFEINE_THUNKS_MATHFFP_H */
