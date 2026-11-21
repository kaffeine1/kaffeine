#!/bin/sh
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
#
# Builds kaffeine-jit68.library, and the probe kj68probe, outside an AROS
# tree, with the tools and the SDK of an AROS build for the target. This is
# how the library and the probe of the native run described in the README
# were built. The branch proposed to AROS builds them with the rules of
# the AROS tree instead.
#
# usage: build-outside-tree.sh <cc> <sdk> <genmodule> <strip> <libentry.o> <out>
#   cc          the AROS cross compiler of the target
#   sdk         the Developer directory of the AROS build (its --sysroot)
#   genmodule   the genmodule of that build
#   strip       the strip of the cross toolchain
#   libentry.o  compiler/libinit/libentry.o of the build's generated tree
#   out         a new directory for everything this script writes
set -e
CC="$1"; SDK="$2"; GM="$3"; STRIP="$4"; LIBENTRY="$5"; OUT="$6"
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(dirname "$HERE")
N=kaffeine-jit68
mkdir -p "$OUT/gen/include/proto" "$OUT/gen/include/clib" "$OUT/gen/include/defines" "$OUT/gen/include/inline" "$OUT/obj"
"$GM" -c "$HERE/$N.conf" -d "$OUT/gen" writefiles $N library
"$GM" -c "$HERE/$N.conf" -d "$OUT/gen/include" writeincludes $N library
"$GM" -c "$HERE/$N.conf" -d "$OUT/gen/include" writelibdefs $N library
printf '#define LC_LIBDEFS_FILE "%s"\n' "$OUT/gen/include/${N}_libdefs.h" > "$OUT/gen/include/${N}_deflibdefs.h"
# The flags of a plain C disk library of the AROS build, with the warnings on.
CFLAGS="-mcmodel=large -O2 -Wall -Wextra -Wno-unused-parameter -Wno-pointer-sign -Wno-parentheses -DAROS_BUILD_TYPE=AROS_BUILD_TYPE_PERSONAL -I$OUT/gen/include -I$SRC/include -I$SRC/engine -include $OUT/gen/include/${N}_deflibdefs.h"
for f in "$OUT/gen/${N}_start.c" "$OUT/gen/${N}_end.c" "$SRC/engine/kj68_engine.c" "$SRC/engine/kj68_translate.c" "$HERE/kj68_library.c" "$HERE/kj68_platform.c"; do
  o="$OUT/obj/$(basename "$f" .c).o"
  "$CC" --sysroot "$SDK" -iquote "$OUT/gen/" $CFLAGS -D__SRCFILENAME__="\"$(basename "$f")\"" -c "$f" -o "$o"
done
printf '%s %s %s %s %s %s\n' "$LIBENTRY" "$OUT/obj/${N}_start.o" "$OUT/obj/kj68_library.o" "$OUT/obj/kj68_engine.o" "$OUT/obj/kj68_translate.o" "$OUT/obj/kj68_platform.o" > "$OUT/link.rsp"
"$CC" --sysroot "$SDK" -mcmodel=large -nostartfiles -Wl,-Map -Xlinker "$OUT/$N.map" -Wl,@"$OUT/link.rsp" "$OUT/obj/${N}_end.o" -o "$OUT/$N.library" -static -Wl,--allow-multiple-definition -Wl,--start-group -Wl,--end-group
"$STRIP" --strip-unneeded -R.comment "$OUT/$N.library"
"$CC" --sysroot "$SDK" -mcmodel=large -O2 -Wall -Wextra -Wno-pointer-sign -Wno-parentheses -I"$SRC/include" "$HERE/kj68probe.c" -o "$OUT/kj68probe"
