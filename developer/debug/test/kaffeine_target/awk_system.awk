# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
#
# Builtin system() executing an AmigaDOS command through libnix system()
# (SystemTagList on AmigaOS 2.0+ and AROS) and capturing its return value.

BEGIN {
    print "before system"
    rc = system("echo kaffeine")
    print "rc=" rc
}
