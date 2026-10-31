# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
#
# String builtins: split(), substr(), index(), length(), sub(), gsub(),
# tolower(), toupper() and sprintf().

BEGIN {
    s = "alpha,beta,,gamma:delta;epsilon"
    n = split(s, parts, /[,:;]+/)
    printf "split-re (%d):", n
    for (i = 1; i <= n; i++)
        printf " [%d]=%s", i, parts[i]
    printf "\n"

    nc = split("m68k", chars, "")
    printf "split-chars (%d):", nc
    for (i = 1; i <= nc; i++)
        printf " %s", chars[i]
    printf "\n"

    base = "The One True AWK 20230911"
    printf "len=%d idx-True=%d idx-missing=%d\n",
        length(base), index(base, "True"), index(base, "Lua")
    printf "sub1=[%s] sub2=[%s] sub3=[%s]\n",
        substr(base, 5, 8), substr(base, 18), substr(base, 1, 3)
    printf "case: lower=[%s] upper=[%s]\n",
        tolower(base), toupper(base)

    t = "foo=10 bar=20 baz=30"
    c1 = sub(/[0-9]+/, "<&>", t)
    printf "sub (%d): %s\n", c1, t
    c2 = gsub(/[a-z]+=[0-9<>]+/, "[&]", t)
    printf "gsub (%d): %s\n", c2, t

    u = "path/to/file.awk"
    c3 = gsub(/\//, "\\&", u)
    printf "gsub-esc (%d): %s\n", c3, u

    msg = sprintf("%s:%04d:%-6s:%.3f", "step", 7, "ok", 12.375)
    printf "sprintf: %s (len=%d)\n", msg, length(msg)
}
