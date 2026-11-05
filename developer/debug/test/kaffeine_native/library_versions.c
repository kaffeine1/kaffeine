/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The versions of the native libraries for which Kaffeine gives 68k
    programs native-backed bases, printed as exec_library_metadata
    (kaffeine_target) prints those of the bases: "info <name>
    <version>.<revision>", or "info <name> absent". On one system the two
    programs print the same line for every library present in both.
*/
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

static const char *const names[] =
{
    "intuition.library", "graphics.library", "utility.library", "gadtools.library",
    "layers.library", "icon.library", "diskfont.library", "locale.library",
    "keymap.library", "asl.library", "workbench.library", "iffparse.library",
    "mathffp.library", "mathieeedoubbas.library", "mathieeedoubtrans.library",
    "mathieeesingbas.library", "datatypes.library", "expansion.library",
    "cybergraphics.library"
};

static void print_version(const char *name, const struct Library *lib)
{
    Printf((CONST_STRPTR)"info %s %ld.%ld\n", name, (LONG)lib->lib_Version,
           (LONG)lib->lib_Revision);
}

int main(void)
{
    ULONG i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        struct Library *lib = OpenLibrary((CONST_STRPTR)names[i], 0);

        if (!lib)
        {
            Printf((CONST_STRPTR)"info %s absent\n", names[i]);
            continue;
        }
        print_version(names[i], lib);
        CloseLibrary(lib);
    }
    print_version("exec.library", &SysBase->LibNode);
    print_version("dos.library", &DOSBase->dl_lib);
    return RETURN_OK;
}
