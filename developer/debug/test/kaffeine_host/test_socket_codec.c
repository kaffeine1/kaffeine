/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#include "kaffeine_socket_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    unsigned fd, i, checks = 0;
    uint8_t data[40], expected[40];
    const uint8_t scalar[] = {0x89, 0xab, 0xcd, 0xef};
    assert(KSocketRead32(scalar) == UINT32_C(0x89abcdef)); ++checks;
    memset(data, 0x5a, sizeof(data));
    KSocketWrite32(data + 1, UINT32_C(0x8004667e));
    assert(data[0] == 0x5a && data[1] == 0x80 && data[2] == 4 &&
           data[3] == 0x66 && data[4] == 0x7e && data[5] == 0x5a); ++checks;
    for (fd = 0; fd < 256; ++fd)
    {
        memset(data, 0, sizeof(data)); memset(expected, 0, sizeof(expected));
        /* Independent byte-level oracle: little bit numbering within each
           big-endian four-byte word. Includes the signed bit 31 and word 1. */
        expected[1 + fd / 32 * 4 + 3 - (fd % 32) / 8] = (uint8_t)(1u << (fd % 8));
        KSocketSet(data + 1, fd);
        assert(!memcmp(data, expected, sizeof(data))); ++checks;
        for (i = 0; i < 256; ++i)
        { assert(KSocketIsSet(data + 1, i) == (fd == i)); ++checks; }
    }
    assert(KSocketSetBytes(0) == 0 && KSocketSetBytes(1) == 4 &&
           KSocketSetBytes(32) == 4 && KSocketSetBytes(33) == 8 &&
           KSocketSetBytes(64) == 8 && KSocketSetBytes(256) == 32); ++checks;
    assert(!KSocketRange(100, 0, 4) && !KSocketRange(100, 100, 0) &&
           KSocketRange(100, 99, 1) && !KSocketRange(100, 99, 2) &&
           !KSocketRange(100, UINT32_MAX - 1, 4) &&
           !KSocketRange(100, 1, UINT32_MAX) && !KSocketRange(0, 1, 1)); ++checks;
    {
        uint32_t words[2050];
        memset(words, 0, sizeof(words)); words[0] = words[2049] = UINT32_C(0xdeadbeef);
        assert(KSocketNativeSet(words + 1, 2048, 64)); ++checks;
        assert(words[3] == 1 && words[2] == 0 && KSocketNativeIsSet(words + 1, 2048, 64)); ++checks;
        assert(KSocketNativeSet(words + 1, 2048, 65535) && words[2048] == UINT32_C(0x80000000)); ++checks;
        assert(!KSocketNativeSet(words + 1, 2048, 65536) && !KSocketNativeIsSet(words + 1, 2048, 65536)); ++checks;
        assert(!KSocketNativeSet(words + 1, 0, 0) && !KSocketNativeIsSet(words + 1, 0, 0)); ++checks;
        assert(words[0] == UINT32_C(0xdeadbeef) && words[2049] == UINT32_C(0xdeadbeef)); ++checks;
    }
    printf("socket codec: %u checks, 0 failures\n", checks);
    return 0;
}
