#!/usr/bin/env python3
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
"""Compile the production Init/Drop and private stream helpers verbatim.

Domain loading and scheduler transport retain their standalone suites. This
unit links the stream bridge to real FileRecords, Close, heap and dispatcher.
"""
from pathlib import Path
import sys

core, output = map(Path, sys.argv[1:])
source = (core / "kaffeine_domain_process.c").read_text()
start = source.index("/* DosEntry saves these identities")
end = source.index("/* Allocated before either a copied module", start)
includes = '''/* Generated verbatim from the production stream bridge. */
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include "kaffeine_intern.h"
#include "kaffeine_filehandle.h"
#include "kaffeine_locks.h"
#include "kaffeine_offsets.h"
'''
output.write_text(includes + source[start:end])
