# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
#
# Regular expressions with POSIX character classes, bracket ranges, negated
# classes, alternation, quantifiers and match() with RSTART/RLENGTH on
# EMU68/test.countries.

BEGIN {
    FS = "\t"
    samples[1] = "item_42 = 0x1a2f"
    samples[2] = "AROS_m68k_2026"
    samples[3] = "   \t  spaced-out"
    for (i = 1; i <= 3; i++) {
        s = samples[i]
        has_hex = (s ~ /0x[[:xdigit:]]+/)
        has_up  = (s ~ /^[[:upper:]]+[[:punct:]][[:lower:]][[:alnum:]_]+$/)
        has_sp  = (s ~ /^[[:space:]]+[[:alpha:]]+-[[:alpha:]]+$/)
        if (match(s, /[[:alpha:]_]+[[:digit:]]*/))
            tok = substr(s, RSTART, RLENGTH) "@" RSTART ":" RLENGTH
        else
            tok = "none"
        printf "sample %d: hex=%d up=%d sp=%d tok=%s\n",
            i, has_hex, has_up, has_sp, tok
    }
}

{
    if ($4 ~ /^(North|South) America$/)
        alt = "americas"
    else if ($4 ~ /^(Asia|Africa)$/)
        alt = "afro-eurasia"
    else
        alt = "oceania"
    cls = ($1 ~ /^[A-CS-Z][[:lower:]]+$/) ? "grp1" : "grp2"
    no_vow_end = ($1 ~ /[^aeiou]$/) ? "cons" : "vowel"
    if (match($0, /[[:digit:]]+\t[[:digit:]]+/))
        nums = substr($0, RSTART, RLENGTH)
    else
        nums = ""
    gsub(/\t/, ":", nums)
    printf "%s\t%s\t%s\t%s\t%s\n", $1, alt, cls, no_vow_end, nums
}
