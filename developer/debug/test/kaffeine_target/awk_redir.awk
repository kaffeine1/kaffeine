# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
#
# Output redirection (> and >>) to T:awk_redir.txt, closing the stream and
# reading the written records back with getline.

BEGIN {
    OFS = ":"
    out = "T:awk_redir.txt"

    print "header", "country", "area", "pop" > out
    printf "row:Russia:%d:%d\n", 8650, 262 > out
    printf "row:Canada:%d:%d\n", 3852, 24 > out
    close(out)

    print "row", "China", 3692, 866 >> out
    printf "footer:count=%d\n", 3 >> out
    close(out)

    n = 0
    while ((rc = (getline line < out)) > 0) {
        n++
        printf "%d: %s\n", n, line
    }
    printf "eof: rc=%d lines=%d\n", rc, n
    close(out)
}
