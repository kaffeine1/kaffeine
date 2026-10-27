#pragma once
#include <exec/ports.h>
#include <intuition/intuition.h>
#include <workbench/startup.h>

/* Native layouts. Guest NDK offsets are written separately in the test. */
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
struct AppMessage
{
    struct Message am_Message;
    UWORD am_Type;
    IPTR am_UserData, am_ID;
    LONG am_NumArgs;
    struct WBArg *am_ArgList;
    UWORD am_Version, am_Class;
    WORD am_MouseX, am_MouseY;
    ULONG am_Seconds, am_Micros, am_Reserved[8];
};
struct AppWindow;
struct AppIcon;
struct AppMenuItem;
#define WB_DISKMAGIC 0xE310
#define WB_DISKVERSION 1
#define WBPROJECT 4
#define AM_VERSION 1
#define AMTYPE_APPWINDOW 7
#define AMTYPE_APPICON 8
#define AMTYPE_APPMENUITEM 9
