# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
#
# printf and sprintf conversions (%d, %i, %o, %x, %X, %u, %c, %s, %e, %E, %f,
# %g, %G and %%) with positive and negative field widths, precisions, zero
# padding and dynamic width/precision arguments, and the exact digits and
# full length of large and long floating conversions.

BEGIN {
    printf "int: [%d] [%i] [%6d] [%-6d] [%06d] [%+d] [% d]\n",
        42, -42, 42, 42, 42, 42, 42
    printf "neg: [%6d] [%-6d] [%06d] [%8i] [%-8i]\n",
        -42, -42, -42, -12345, -12345
    printf "base: [%o] [%#o] [%8o] [%x] [%X] [%#x] [%#X] [%08x] [%u]\n",
        255, 255, 255, 48879, 48879, 48879, 48879, 48879, 3000000000
    printf "wide: [%d] [%x] [%o]\n",
        1099511627776, 1099511627776, 1099511627776
    printf "char: [%c] [%4c] [%-4c] [%c] [%4c] [%-4c]\n",
        65, 66, 67, "Zebra", "Alpha", "Omega"
    printf "str: [%s] [%10s] [%-10s] [%.4s] [%8.4s] [%-8.4s]\n",
        "kaffeine", "kaffeine", "kaffeine", "kaffeine", "kaffeine", "kaffeine"
    printf "flt-f: [%.2f] [%10.3f] [%-10.3f] [%010.3f] [%010.3f] [%+8.2f]\n",
        3.25, 64.125, 64.125, 64.125, -3.25, 3.25
    printf "flt-e: [%.4e] [%.4E] [%12.3e] [%-12.3e]\n",
        64.125, 64.125, -0.003125, -0.003125
    printf "flt-g: [%g] [%.6g] [%.4g] [%.4G] [%10.5g] [%-10.5g]\n",
        64.125, 1234.5625, 0.0000625, 0.0000625, -64.125, -64.125
    printf "dyn: [%*d] [%*s] [%*.*f] [%%]\n",
        6, 77, -8, "emu68", 9, 3, 15.625
    printf "exact: [%.0f] [%.0f] [%.2f] [%.17g] [%.30g]\n",
        1234567890123456, 9007199254740992, 1e15 + 0.25, 0.1, 0.1
    printf "sign: [%020.2f] [%-+12.3e] [% 010.1f]\n", -1.25, 12345.678, 2.5
    printf "big: [%.20e] [%.0f]\n", 1e308, 2^70
    printf "long: [%d] [%s]\n", length(sprintf("%.1000f", 1.25)),
        substr(sprintf("%.300f", 1e308), 300, 20)
}
