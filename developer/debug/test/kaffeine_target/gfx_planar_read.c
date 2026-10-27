/*
 * Copyright (C) 2026, The AROS Development Team.
 *
 * Original ReadPixel regression with manually initialized public structures.
 * No InitRastPort/InitBitMap adapter or display is needed. Only pixels inside
 * real bitplanes are read.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <string.h>
#include "m68ktest.h"

enum { ROW_BYTES = 4, ROWS = 3, WIDTH = 32, PLANE_BYTES = 12, BLOCK_BYTES = 28 };

static const UBYTE colors[WIDTH] = {
    0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,
    0xFF,0x00,0x80,0x01,0xAA,0x55,0xF0,0x0F,
    0x11,0x22,0x33,0x44,0x5A,0x66,0x77,0x88,
    0x99,0xA5,0xBB,0xCC,0xDD,0xEE,0xFE,0x7F
};

static UBYTE color_at(LONG x, LONG y)
{
    if (y == 0) return colors[x];
    if (y == 1) return 255 - colors[x];
    return (UBYTE)(x * 37 + 11);
}

static void read_depth(UBYTE depth)
{
    struct BitMap bm, saved_bm;
    struct RastPort rp, saved_rp;
    UBYTE *blocks[8] = { NULL };
    UBYTE saved[8][BLOCK_BYTES];
    LONG p, x, y, mismatches = 0;
    BOOL intact = TRUE;

    memset(&bm, 0, sizeof(bm));
    memset(&rp, 0, sizeof(rp));
    bm.BytesPerRow = ROW_BYTES;
    bm.Rows = ROWS;
    bm.Depth = depth;
    bm.Flags = depth == 2 ? BMF_STANDARD : 0;
    rp.BitMap = &bm;
    /* ReadPixel does not use the RastPort write mask. */
    rp.Mask = 0;
    for (p = 0; p < depth; ++p)
    {
        blocks[p] = AllocMem(BLOCK_BYTES, MEMF_CHIP | MEMF_CLEAR);
        if (!blocks[p]) break;
        memset(blocks[p], 0xC3, 8);
        memset(blocks[p] + 20, 0x3C, 8);
        bm.Planes[p] = blocks[p] + 8;
        for (y = 0; y < ROWS; ++y)
            for (x = 0; x < WIDTH; ++x)
                if (color_at(x, y) & (1UL << p))
                    bm.Planes[p][y * ROW_BYTES + (x >> 3)] |= 0x80 >> (x & 7);
        memcpy(saved[p], blocks[p], BLOCK_BYTES);
    }
    if (!check(p == depth, "allocate guarded original bitplanes")) goto cleanup;
    memcpy(&saved_bm, &bm, sizeof(bm));
    memcpy(&saved_rp, &rp, sizeof(rp));
    for (y = 0; y < ROWS; ++y)
        for (x = 0; x < WIDTH; ++x)
        {
            LONG expected = color_at(x, y) & ((1UL << depth) - 1);
            LONG got;
            got = ReadPixel(&rp, x, y);
            if (got != expected)
            {
                if (mismatches < 3)
                    Printf("depth=%ld x=%ld y=%ld got=%ld expected=%ld\n",
                           (LONG)depth, x, y, got, expected);
                ++mismatches;
            }
        }
    check_long("all 96 pixels match original colors across three rows", mismatches, 0);
    check(!memcmp(&bm, &saved_bm, sizeof(bm)), "ReadPixel preserves every BitMap byte");
    check(!memcmp(&rp, &saved_rp, sizeof(rp)), "ReadPixel preserves every RastPort byte");
    for (p = 0; p < depth; ++p)
        if (memcmp(blocks[p], saved[p], BLOCK_BYTES)) intact = FALSE;
    check(intact, "ReadPixel preserves all plane bytes and both guards");
cleanup:
    for (p = 0; p < 8; ++p)
        if (blocks[p]) FreeMem(blocks[p], BLOCK_BYTES);
}

int main(void)
{
    read_depth(1);
    read_depth(2);
    read_depth(8);
    return test_done("gfx_planar_read");
}
