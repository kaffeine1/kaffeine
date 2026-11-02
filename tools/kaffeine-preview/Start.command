#!/bin/sh
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
kit_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
python3 "$kit_dir/preview.py" start
preview_exit=$?
printf '\nPress Enter to close this launcher.\n'
read -r preview_reply
exit "$preview_exit"
