#pragma once
#include <intuition/intuition.h>
struct DrawerData;
struct DiskObject
{
    UWORD do_Magic, do_Version;
    struct Gadget do_Gadget;
    UBYTE do_Type;
    STRPTR do_DefaultTool;
    STRPTR *do_ToolTypes;
    LONG do_CurrentX, do_CurrentY;
    struct DrawerData *do_DrawerData;
    STRPTR do_ToolWindow;
    LONG do_StackSize;
};
