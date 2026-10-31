# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
#
# Deliberate fatal error step for the one true awk: writes a line to standard
# output on the first record and then calls printf with a missing argument so
# that awk flushes stdout, writes its diagnostic to stderr and exits with 2.

NR == 1 {
    print "testing fatal error on", $1
    printf("%d %d\n", $2)
}
