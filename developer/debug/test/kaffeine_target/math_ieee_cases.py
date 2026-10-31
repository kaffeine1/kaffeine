#!/usr/bin/env python3
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
#
# Print the case table of math_ieee.c: the arguments and the expected results
# of every IEEE double vector and of the single precision basic vectors, as
# big-endian bit patterns. A case with a
# tolerance of 0 has an exact IEEE 754 result: arithmetic, conversions, sqrt
# and values such as atan(0) leave no choice of rounding. A case with a
# tolerance compares with the host libm within that many units in the last
# place. The expected flags follow the "Flags:" sections of the AROS sources
# in workbench/libs/mathieeedoubbas, mathieeedoubtrans and mathieeesingbas,
# and the V that
# ieeedpsin.c and ieeedpcos.c set for an infinite argument. Comparisons follow
# the RESULT sections: -0.0 and +0.0 are equal. NaNs order by sign and
# magnitude, above +Inf or below -Inf, as "math_ieee probe" measures on
# AmigaOS 3.2.3.
#
#   python3 math_ieee_cases.py          the case table
#   python3 math_ieee_cases.py sincos   the IEEEDPSincos(0.5) results
import math, struct, sys

def bits(x):
    return struct.unpack(">Q", struct.pack(">d", x))[0]
def sbits(x):
    return struct.unpack(">I", struct.pack(">f", x))[0]
INF, NAN = float("inf"), float("nan")
DMAX = 1.7976931348623157e308
N, Z, V = 8, 4, 2

def d(x):   # a double as hi, lo
    b = bits(x); return (b >> 32, b & 0xFFFFFFFF)

cases = []
def add(lib, lvo, name, desc, args, kind, result, flags, tol=0, a0=None):
    cases.append((lib, lvo, name, desc, args, kind, result, flags, tol))

B, T, S = 0, 1, 2
FMAX = 3.4028234663852886e38
# kind: D = double in D0/D1, L = LONG in D0, S = single in D0, NAN = a NaN double
# mathieeedoubbas
add(B, 30, "IEEEDPFix", "2.9 truncates to 2", [d(2.9)], "L", 2, 0)
add(B, 30, "IEEEDPFix", "-2.9 truncates to -2", [d(-2.9)], "L", -2 & 0xFFFFFFFF, N)
add(B, 30, "IEEEDPFix", "0.5 gives 0", [d(0.5)], "L", 0, Z)
add(B, 30, "IEEEDPFix", "-0.0 gives 0", [d(-0.0)], "L", 0, Z)
add(B, 30, "IEEEDPFix", "2^31 saturates", [d(2147483648.0)], "L", 0x7FFFFFFF, V)
add(B, 30, "IEEEDPFix", "-2^31-1 saturates", [d(-2147483649.0)], "L", 0x80000000, N | V)
add(B, 30, "IEEEDPFix", "+Inf saturates", [d(INF)], "L", 0x7FFFFFFF, V)
add(B, 36, "IEEEDPFlt", "7", [(7, 0)], "D", d(7.0), 0)
add(B, 36, "IEEEDPFlt", "-7", [(-7 & 0xFFFFFFFF, 0)], "D", d(-7.0), N)
add(B, 36, "IEEEDPFlt", "0", [(0, 0)], "D", d(0.0), Z)
add(B, 36, "IEEEDPFlt", "2^31-1", [(0x7FFFFFFF, 0)], "D", d(2147483647.0), 0)
add(B, 36, "IEEEDPFlt", "-2^31", [(0x80000000, 0)], "D", d(-2147483648.0), N)
add(B, 42, "IEEEDPCmp", "1 < 2", [d(1.0), d(2.0)], "L", 0xFFFFFFFF, N)
add(B, 42, "IEEEDPCmp", "2 > 1", [d(2.0), d(1.0)], "L", 1, 0)
add(B, 42, "IEEEDPCmp", "2 = 2", [d(2.0), d(2.0)], "L", 0, Z)
add(B, 42, "IEEEDPCmp", "-0.0 = +0.0", [d(-0.0), d(0.0)], "L", 0, Z)
add(B, 42, "IEEEDPCmp", "-Inf < +Inf", [d(-INF), d(INF)], "L", 0xFFFFFFFF, N)
add(B, 42, "IEEEDPCmp", "-2 < -0.5", [d(-2.0), d(-0.5)], "L", 0xFFFFFFFF, N)
add(B, 42, "IEEEDPCmp", "1 < +NaN, above +Inf", [d(1.0), d(NAN)], "L", 0xFFFFFFFF, N)
add(B, 42, "IEEEDPCmp", "1 > -NaN, below -Inf", [d(1.0), d(-NAN)], "L", 1, 0)
add(B, 42, "IEEEDPCmp", "+Inf < +NaN", [d(INF), d(NAN)], "L", 0xFFFFFFFF, N)
add(B, 42, "IEEEDPCmp", "+NaN = +NaN", [d(NAN), d(NAN)], "L", 0, Z)
add(B, 48, "IEEEDPTst", "-3 is negative", [d(-3.0)], "L", 0xFFFFFFFF, N)
add(B, 48, "IEEEDPTst", "0 is zero", [d(0.0)], "L", 0, Z)
add(B, 48, "IEEEDPTst", "5 is positive", [d(5.0)], "L", 1, 0)
add(B, 48, "IEEEDPTst", "+NaN is positive", [d(NAN)], "L", 1, 0)
add(B, 48, "IEEEDPTst", "-NaN is negative", [d(-NAN)], "L", 0xFFFFFFFF, N)
add(B, 54, "IEEEDPAbs", "-4", [d(-4.0)], "D", d(4.0), 0)
add(B, 54, "IEEEDPAbs", "-0.0 gives +0.0", [d(-0.0)], "D", d(0.0), Z)
add(B, 60, "IEEEDPNeg", "4", [d(4.0)], "D", d(-4.0), N)
add(B, 60, "IEEEDPNeg", "+0.0 gives -0.0", [d(0.0)], "D", d(-0.0), Z)
add(B, 66, "IEEEDPAdd", "1.5 + 2.25", [d(1.5), d(2.25)], "D", d(3.75), 0)
add(B, 66, "IEEEDPAdd", "1 + -3", [d(1.0), d(-3.0)], "D", d(-2.0), N)
add(B, 66, "IEEEDPAdd", "2 + -2 is +0.0", [d(2.0), d(-2.0)], "D", d(0.0), Z)
add(B, 66, "IEEEDPAdd", "-0.0 + -0.0 is -0.0", [d(-0.0), d(-0.0)], "D", d(-0.0), Z)
add(B, 66, "IEEEDPAdd", "MAX + MAX overflows", [d(DMAX), d(DMAX)], "D", d(INF), V)
add(B, 72, "IEEEDPSub", "1 - 3", [d(1.0), d(3.0)], "D", d(-2.0), N)
add(B, 72, "IEEEDPSub", "0.5 - 0.5", [d(0.5), d(0.5)], "D", d(0.0), Z)
add(B, 72, "IEEEDPSub", "-MAX - MAX overflows", [d(-DMAX), d(DMAX)], "D", d(-INF), N | V)
add(B, 78, "IEEEDPMul", "1.5 * -4", [d(1.5), d(-4.0)], "D", d(-6.0), N)
add(B, 78, "IEEEDPMul", "0 * -1 is -0.0", [d(0.0), d(-1.0)], "D", d(-0.0), Z)
add(B, 78, "IEEEDPMul", "1e300 * 1e300 overflows", [d(1e300), d(1e300)], "D", d(INF), V)
add(B, 84, "IEEEDPDiv", "1 / 4", [d(1.0), d(4.0)], "D", d(0.25), 0)
add(B, 84, "IEEEDPDiv", "1 / 3, correctly rounded", [d(1.0), d(3.0)], "D", d(1.0 / 3.0), 0)
add(B, 84, "IEEEDPDiv", "1 / 0 is +Inf", [d(1.0), d(0.0)], "D", d(INF), V)
add(B, 84, "IEEEDPDiv", "-1 / 0 is -Inf", [d(-1.0), d(0.0)], "D", d(-INF), N | V)
add(B, 84, "IEEEDPDiv", "0 / 0 is NaN", [d(0.0), d(0.0)], "NAN", None, V)
add(B, 90, "IEEEDPFloor", "-1.5", [d(-1.5)], "D", d(-2.0), N)
add(B, 90, "IEEEDPFloor", "2.7", [d(2.7)], "D", d(2.0), 0)
add(B, 90, "IEEEDPFloor", "-0.0", [d(-0.0)], "D", d(-0.0), Z)
add(B, 96, "IEEEDPCeil", "-1.5", [d(-1.5)], "D", d(-1.0), N)
add(B, 96, "IEEEDPCeil", "0.2", [d(0.2)], "D", d(1.0), 0)
add(B, 96, "IEEEDPCeil", "-0.5 gives -0.0", [d(-0.5)], "D", d(-0.0), Z)
# mathieeedoubtrans
add(T, 30, "IEEEDPAtan", "1 is pi/4", [d(1.0)], "D", d(math.atan(1.0)), 0, 2)
add(T, 30, "IEEEDPAtan", "-1 is -pi/4", [d(-1.0)], "D", d(math.atan(-1.0)), N, 2)
add(T, 30, "IEEEDPAtan", "0", [d(0.0)], "D", d(0.0), Z)
add(T, 36, "IEEEDPSin", "0", [d(0.0)], "D", d(0.0), Z)
add(T, 36, "IEEEDPSin", "0.5", [d(0.5)], "D", d(math.sin(0.5)), 0, 2)
add(T, 36, "IEEEDPSin", "+Inf is NaN with V", [d(INF)], "NAN", None, V)
add(T, 42, "IEEEDPCos", "0", [d(0.0)], "D", d(1.0), 0)
add(T, 42, "IEEEDPCos", "3 is negative", [d(3.0)], "D", d(math.cos(3.0)), N, 2)
add(T, 42, "IEEEDPCos", "-Inf is NaN with V", [d(-INF)], "NAN", None, V)
add(T, 48, "IEEEDPTan", "0", [d(0.0)], "D", d(0.0), Z)
add(T, 48, "IEEEDPTan", "0.5", [d(0.5)], "D", d(math.tan(0.5)), 0, 2)
add(T, 48, "IEEEDPTan", "+Inf is NaN with V", [d(INF)], "NAN", None, V)
add(T, 60, "IEEEDPSinh", "0", [d(0.0)], "D", d(0.0), Z)
add(T, 60, "IEEEDPSinh", "1000 overflows", [d(1000.0)], "D", d(INF), V)
add(T, 60, "IEEEDPSinh", "-1000 overflows", [d(-1000.0)], "D", d(-INF), N | V)
add(T, 66, "IEEEDPCosh", "0", [d(0.0)], "D", d(1.0), 0)
add(T, 66, "IEEEDPCosh", "-1000 overflows, not negative", [d(-1000.0)], "D", d(INF), V)
add(T, 72, "IEEEDPTanh", "0", [d(0.0)], "D", d(0.0), Z)
add(T, 72, "IEEEDPTanh", "100", [d(100.0)], "D", d(1.0), 0)
add(T, 72, "IEEEDPTanh", "-100", [d(-100.0)], "D", d(-1.0), N)
add(T, 78, "IEEEDPExp", "0", [d(0.0)], "D", d(1.0), 0)
add(T, 78, "IEEEDPExp", "1 is e", [d(1.0)], "D", d(math.e), 0, 2)
add(T, 78, "IEEEDPExp", "-1000 underflows to 0", [d(-1000.0)], "D", d(0.0), Z)
add(T, 78, "IEEEDPExp", "1000 overflows", [d(1000.0)], "D", d(INF), V)
add(T, 84, "IEEEDPLog", "1", [d(1.0)], "D", d(0.0), Z)
add(T, 84, "IEEEDPLog", "0.5 is -ln 2", [d(0.5)], "D", d(math.log(0.5)), N, 2)
add(T, 84, "IEEEDPLog", "0 is -Inf", [d(0.0)], "D", d(-INF), N)
add(T, 84, "IEEEDPLog", "-1 is NaN with V", [d(-1.0)], "NAN", None, V)
add(T, 90, "IEEEDPPow", "2 in D0/D1 to the power 3 in D2/D3 is 8", [d(2.0), d(3.0)], "D", d(8.0), 0, 1)
add(T, 90, "IEEEDPPow", "3 in D0/D1 to the power 2 in D2/D3 is 9", [d(3.0), d(2.0)], "D", d(9.0), 0, 1)
add(T, 90, "IEEEDPPow", "-2 to the power 3 is -8", [d(-2.0), d(3.0)], "D", d(-8.0), N, 1)
add(T, 90, "IEEEDPPow", "0 to the power 2 is 0", [d(0.0), d(2.0)], "D", d(0.0), Z)
add(T, 90, "IEEEDPPow", "10 to the power 1000 overflows", [d(10.0), d(1000.0)], "D", d(float("inf")), V)
add(T, 96, "IEEEDPSqrt", "4", [d(4.0)], "D", d(2.0), 0)
add(T, 96, "IEEEDPSqrt", "2, correctly rounded", [d(2.0)], "D", d(math.sqrt(2.0)), 0)
add(T, 96, "IEEEDPSqrt", "-0.0, zero but not negative", [d(-0.0)], "D", d(-0.0), Z)
add(T, 96, "IEEEDPSqrt", "-1 is NaN with V", [d(-1.0)], "NAN", None, V)
add(T, 102, "IEEEDPTieee", "1.5", [d(1.5)], "S", sbits(1.5), 0)
add(T, 102, "IEEEDPTieee", "0.1 rounds to nearest", [d(0.1)], "S", sbits(0.1), 0)
add(T, 102, "IEEEDPTieee", "-2", [d(-2.0)], "S", sbits(-2.0), N)
add(T, 102, "IEEEDPTieee", "0", [d(0.0)], "S", 0, Z)
add(T, 102, "IEEEDPTieee", "1e300 overflows", [d(1e300)], "S", sbits(INF), V)
add(T, 108, "IEEEDPFieee", "1.5", [(sbits(1.5), 0)], "D", d(1.5), 0)
add(T, 108, "IEEEDPFieee", "-2", [(sbits(-2.0), 0)], "D", d(-2.0), N)
add(T, 108, "IEEEDPFieee", "-0.0", [(0x80000000, 0)], "D", d(-0.0), Z)
add(T, 108, "IEEEDPFieee", "+Inf", [(sbits(INF), 0)], "D", d(INF), 0)
add(T, 114, "IEEEDPAsin", "1 is pi/2", [d(1.0)], "D", d(math.asin(1.0)), 0, 2)
add(T, 114, "IEEEDPAsin", "0", [d(0.0)], "D", d(0.0), Z)
add(T, 114, "IEEEDPAsin", "2 is NaN with V", [d(2.0)], "NAN", None, V)
add(T, 120, "IEEEDPAcos", "1", [d(1.0)], "D", d(0.0), Z)
add(T, 120, "IEEEDPAcos", "-1 is pi", [d(-1.0)], "D", d(math.pi), 0, 2)
add(T, 120, "IEEEDPAcos", "2 is NaN with V", [d(2.0)], "NAN", None, V)
add(T, 126, "IEEEDPLog10", "100", [d(100.0)], "D", d(2.0), 0, 2)
add(T, 126, "IEEEDPLog10", "1", [d(1.0)], "D", d(0.0), Z)
add(T, 126, "IEEEDPLog10", "0.001", [d(0.001)], "D", d(math.log10(0.001)), N, 2)
add(T, 126, "IEEEDPLog10", "-1 is NaN with V", [d(-1.0)], "NAN", None, V)
# mathieeesingbas: one single in D0, a second one in D1, the result in D0.
# The struct round trip gives the correctly rounded single of each result.
def s(x, y=0.0):
    return [(sbits(x), sbits(y))]
add(S, 30, "IEEESPFix", "2.9 truncates to 2", s(2.9), "L", 2, 0)
add(S, 30, "IEEESPFix", "-2.9 truncates to -2", s(-2.9), "L", -2 & 0xFFFFFFFF, N)
add(S, 30, "IEEESPFix", "2^31 saturates", s(2147483648.0), "L", 0x7FFFFFFF, V)
add(S, 30, "IEEESPFix", "-Inf saturates", s(-INF), "L", 0x80000000, N | V)
add(S, 36, "IEEESPFlt", "7", [(7, 0)], "S", sbits(7.0), 0)
add(S, 36, "IEEESPFlt", "0", [(0, 0)], "S", 0, Z)
add(S, 36, "IEEESPFlt", "2^24+1 is inexact", [(16777217, 0)], "S", sbits(16777216.0), V)
add(S, 36, "IEEESPFlt", "-2^31", [(0x80000000, 0)], "S", sbits(-2147483648.0), N)
add(S, 42, "IEEESPCmp", "1 < 2", s(1.0, 2.0), "L", 0xFFFFFFFF, N)
add(S, 42, "IEEESPCmp", "2 > 1", s(2.0, 1.0), "L", 1, 0)
add(S, 42, "IEEESPCmp", "-0.0 = +0.0", s(-0.0, 0.0), "L", 0, Z)
add(S, 42, "IEEESPCmp", "1 < +NaN", s(1.0, NAN), "L", 0xFFFFFFFF, N)
add(S, 42, "IEEESPCmp", "1 > -NaN", s(1.0, -NAN), "L", 1, 0)
add(S, 48, "IEEESPTst", "-3 is negative", s(-3.0), "L", 0xFFFFFFFF, N)
add(S, 48, "IEEESPTst", "+NaN is positive", s(NAN), "L", 1, 0)
add(S, 54, "IEEESPAbs", "-4", s(-4.0), "S", sbits(4.0), 0)
add(S, 60, "IEEESPNeg", "4", s(4.0), "S", sbits(-4.0), N)
add(S, 66, "IEEESPAdd", "1.5 + 2.25", s(1.5, 2.25), "S", sbits(3.75), 0)
add(S, 66, "IEEESPAdd", "MAX + MAX overflows", s(FMAX, FMAX), "S", sbits(INF), V)
add(S, 72, "IEEESPSub", "1 - 3", s(1.0, 3.0), "S", sbits(-2.0), N)
add(S, 78, "IEEESPMul", "1.5 * -4", s(1.5, -4.0), "S", sbits(-6.0), N)
add(S, 78, "IEEESPMul", "3e38 * 2 overflows", s(3e38, 2.0), "S", sbits(INF), V)
add(S, 78, "IEEESPMul", "1e-30 * 1e-30 underflows to 0", s(1e-30, 1e-30), "S", 0, Z | V)
add(S, 78, "IEEESPMul", "2^-126 * 0.5 is an exact denormal", s(2.0 ** -126, 0.5), "S", 0x00400000, 0)
add(S, 84, "IEEESPDiv", "1 / 3, correctly rounded", s(1.0, 3.0), "S", sbits(1.0 / 3.0), 0)
add(S, 84, "IEEESPDiv", "1 / 0 is +Inf", s(1.0, 0.0), "S", sbits(INF), V)
add(S, 84, "IEEESPDiv", "2^-126 / 3 rounds to a denormal", s(2.0 ** -126, 3.0), "S", 0x002AAAAB, V)
add(S, 84, "IEEESPDiv", "1e-30 / 1e30 underflows to 0", s(1e-30, 1e30), "S", 0, Z | V)
add(S, 90, "IEEESPFloor", "-1.5", s(-1.5), "S", sbits(-2.0), N)
add(S, 96, "IEEESPCeil", "0.2", s(0.2), "S", sbits(1.0), 0)

def fmt_flags(f):
    s = [n for b, n in ((N, "CC_N"), (Z, "CC_Z"), (V, "CC_V")) if f & b]
    return " | ".join(s) if s else "0"

if len(sys.argv) > 1 and sys.argv[1] == "sincos":
    for name, value in (("SIN_HALF", math.sin(0.5)), ("COS_HALF", math.cos(0.5))):
        hi, lo = d(value)
        print("#define %s_HI 0x%08XUL\n#define %s_LO 0x%08XUL" % (name, hi, name, lo))
    sys.exit(0)

out = []
for lib, lvo, name, desc, args, kind, result, flags, tol in cases:
    a = args + [(0, 0)] * (2 - len(args))
    if kind == "D":
        r = "0x%08XUL, 0x%08XUL" % result
    elif kind in ("L", "S"):
        r = "0x%08XUL, 0" % result
    else:
        r = "0, 0"
    out.append('    { %s, %3d, "%s(%s)", 0x%08XUL, 0x%08XUL, 0x%08XUL, 0x%08XUL, R_%s, %s, %s, %d },' % (
        ("BAS", "TRANS", "SING")[lib], lvo, name, desc, a[0][0], a[0][1], a[1][0], a[1][1], kind, r, fmt_flags(flags), tol))
print("\n".join(out))
print(len(cases), "cases", file=sys.stderr)
