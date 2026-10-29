/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef KAFFEINE_THUNKS_LOCKS_H
#define KAFFEINE_THUNKS_LOCKS_H
#include "kaffeine_thunks.h"
#ifdef __cplusplus
extern "C" {
#endif
IPTR kaffeine_dos_DupLock(struct KaffeineContext *, void *);
IPTR kaffeine_dos_ParentDir(struct KaffeineContext *, void *);
IPTR kaffeine_dos_SameLock(struct KaffeineContext *, void *);
IPTR kaffeine_dos_SameDevice(struct KaffeineContext *, void *);
IPTR kaffeine_dos_SetProgramDir(struct KaffeineContext *, void *);
IPTR kaffeine_dos_GetProgramDir(struct KaffeineContext *, void *);
IPTR kaffeine_dos_MakeLink(struct KaffeineContext *, void *);
IPTR kaffeine_dos_AssignAddToList(struct KaffeineContext *, void *);
IPTR kaffeine_dos_ReadLinkUnsupported(struct KaffeineContext *, void *);
IPTR kaffeine_dos_UnsupportedLockObject(struct KaffeineContext *, void *);
#define KAFFEINE_DOS_LOCK_THUNKS \
    { 96, kaffeine_dos_DupLock }, \
    { 210, kaffeine_dos_ParentDir }, \
    { 420, kaffeine_dos_SameLock }, \
    { 438, kaffeine_dos_ReadLinkUnsupported }, \
    { 444, kaffeine_dos_MakeLink }, \
    { 480, kaffeine_dos_UnsupportedLockObject }, \
    { 594, kaffeine_dos_SetProgramDir }, \
    { 600, kaffeine_dos_GetProgramDir }, \
    { 654, kaffeine_dos_UnsupportedLockObject }, \
    { 660, kaffeine_dos_UnsupportedLockObject }, \
    { 666, kaffeine_dos_UnsupportedLockObject }, \
    { 672, kaffeine_dos_UnsupportedLockObject }, \
    { 678, kaffeine_dos_UnsupportedLockObject }, \
    { 684, kaffeine_dos_UnsupportedLockObject }, \
    { 690, kaffeine_dos_UnsupportedLockObject }, \
    { 696, kaffeine_dos_UnsupportedLockObject }, \
    { 702, kaffeine_dos_UnsupportedLockObject }, \
    { 984, kaffeine_dos_SameDevice }, \
    { 1356, kaffeine_dos_AssignAddToList }
#ifdef __cplusplus
}
#endif
#endif
