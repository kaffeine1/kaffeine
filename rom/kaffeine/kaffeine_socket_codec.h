/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#ifndef KAFFEINE_SOCKET_CODEC_H
#define KAFFEINE_SOCKET_CODEC_H
#include <stdint.h>
#include <stddef.h>

/* Classic AmiTCP scalars are big endian, including fd_mask (32 bits).
 * Network-order address/port octets are copied, never byte-swapped twice. */
static inline uint32_t KSocketRead32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}
static inline void KSocketWrite32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline size_t KSocketSetBytes(unsigned nfds)
{
    return ((size_t)nfds + 31) / 32 * 4;
}
static inline int KSocketIsSet(const uint8_t *p, unsigned fd)
{
    return (KSocketRead32(p + (fd / 32) * 4) >> (fd % 32)) & 1;
}
static inline void KSocketSet(uint8_t *p, unsigned fd)
{
    p += fd / 32 * 4;
    KSocketWrite32(p, KSocketRead32(p) | (UINT32_C(1) << (fd % 32)));
}
static inline int KSocketRange(uint32_t total, uint32_t address, uint32_t bytes)
{
    return address && address < total && bytes <= total - address;
}
static inline int KSocketNativeSet(uint32_t *words, size_t count, unsigned fd)
{
    if (fd / 32 >= count) return 0;
    words[fd / 32] |= UINT32_C(1) << (fd % 32);
    return 1;
}
static inline int KSocketNativeIsSet(const uint32_t *words, size_t count, unsigned fd)
{
    return fd / 32 < count && ((words[fd / 32] >> (fd % 32)) & 1);
}
#endif
