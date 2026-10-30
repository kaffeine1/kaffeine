/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef KAFFEINE_VIDEOCONTROL_H
#define KAFFEINE_VIDEOCONTROL_H
#include "kaffeine_thunks.h"
struct Screen;
#define KAFFEINE_VPE_SIZE 66
#define KAFFEINE_VPE_LIMIT 4096
#define KAFFEINE_VIDEO_TAG_STEPS 256
#ifdef __cplusplus
extern "C" {
#endif
IPTR Kaffeine_VideoControl(struct KaffeineContext *, void *);
IPTR Kaffeine_VideoControlUnsupported(struct KaffeineContext *, void *);
/* Borrowed for the live Screen's existing lifetime. No extra visitor lease.
 * Retire never dereferences Screen/ColorMap/VPE; native close may have run. */
void Kaffeine_VideoControlRetireScreen(struct KaffeineContext *, struct Screen *);
/* Quiescent arena destruction only; no guest or native object dereference. */
void Kaffeine_VideoControlCleanup(struct KaffeineContext *);
#ifdef __cplusplus
}
#endif
#define KAFFEINE_GRAPHICS_VIDEO_NODE_THUNKS \
    { 660, Kaffeine_VideoControlUnsupported }, \
    { 666, Kaffeine_VideoControlUnsupported }, \
    { 672, Kaffeine_VideoControlUnsupported }, \
    { 702, Kaffeine_VideoControlUnsupported }
#endif
