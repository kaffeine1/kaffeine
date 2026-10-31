/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The configuration header of Lua 5.4.7, included in front of each file for
    both the m68k and the host build. It sets LUA_PATH_DEFAULT so that
    require finds modules in EMU68/ or the current directory without probing
    Unix paths, fixes the decimal point to '.' and restricts setlocale() to
    the C locale so that literals.lua produces the same output on every host
    and on AmigaOS, and leaves lua_strx2number and lua_number2strx undefined
    so that both builds use Lua's own hexadecimal float parser and formatter.
    On m68k it routes l_sprintf and lua_str2number to lua_libnix.c.
*/

#ifndef LUA_CONFIG_H
#define LUA_CONFIG_H

#include <locale.h>
#include <stddef.h>
#include <string.h>

#define LUA_PATH_DEFAULT "EMU68/?.lua;?.lua"
#define LUA_CPATH_DEFAULT ""
#define lua_getlocaledecpoint() '.'

#define LUA_CORE
#define LUA_LIB
#include "luaconf.h"

#undef lua_strx2number
#undef lua_number2strx

static inline char *lua_c_setlocale(int cat, const char *locale)
{
    (void)cat;
    if (locale == NULL || strcmp(locale, "C") == 0 || strcmp(locale, "") == 0)
        return (char *)"C";
    return NULL;
}

#define setlocale lua_c_setlocale

#ifdef __mc68000__
extern int lua_libnix_snprintf(char *buf, size_t sz, const char *fmt, ...);
extern double lua_libnix_strtod(const char *nptr, char **endptr);
#undef l_sprintf
#define l_sprintf(s, sz, f, i) lua_libnix_snprintf(s, sz, f, i)
#undef lua_str2number
#define lua_str2number(s, p) lua_libnix_strtod(s, p)
#endif

#endif
