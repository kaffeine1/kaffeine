# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
#
# Explicit getline from EMU68/test.countries and EMU68/test.data, into a
# variable and into $0, followed by close() and a second pass.

BEGIN {
    FS = "\t"
    cfile = "EMU68/test.countries"
    dfile = "EMU68/test.data"

    count = 0
    total_area = 0
    total_pop = 0
    while ((rc = (getline line < cfile)) > 0) {
        count++
        n = split(line, f, "\t")
        total_area += f[2]
        total_pop += f[3]
        if (count <= 3 || count == 10)
            printf "pass1 %d (%d): %s/%s\n", count, n, f[1], f[4]
    }
    printf "pass1 summary: rc=%d count=%d area=%d pop=%d\n",
        rc, count, total_area, total_pop
    close(cfile)

    for (i = 1; i <= 3; i++) {
        rc = (getline < cfile)
        printf "pass2 %d: rc=%d NF=%d country=%s area=%s\n",
            i, rc, NF, $1, $2
    }
    close(cfile)

    dcount = 0
    while ((getline dline < dfile) > 0 && dcount < 5) {
        if (dline != "") {
            dcount++
            printf "data %d: %s\n", dcount, dline
        }
    }
    close(dfile)
}
