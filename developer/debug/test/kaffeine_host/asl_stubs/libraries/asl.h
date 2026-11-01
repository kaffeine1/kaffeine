/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
/* Names, numbers and the FileRequester fields of
   compiler/include/libraries/asl.h; host pointers have host width. */
#pragma once
#include <exec/types.h>
#include <utility/tagitem.h>

struct WBArg;

struct FileRequester
{
    UBYTE  fr_Reserved0[4];
    STRPTR fr_File;
    STRPTR fr_Drawer;
    UBYTE  fr_Reserved1[10];
    WORD   fr_LeftEdge;
    WORD   fr_TopEdge;
    WORD   fr_Width;
    WORD   fr_Height;
    UBYTE  fr_Reserved2[2];
    LONG   fr_NumArgs;
    struct WBArg *fr_ArgList;
    APTR   fr_UserData;
    UBYTE  fr_Reserved3[8];
    STRPTR fr_Pattern;
};

#define ASL_FileRequest       0
#define ASL_FontRequest       1
#define ASL_ScreenModeRequest 2

#define ASL_TB (TAG_USER + 0x80000)

#define ASLFR_Window             (ASL_TB + 2)
#define ASLFR_Screen             (ASL_TB + 40)
#define ASLFR_PubScreenName      (ASL_TB + 41)
#define ASLFR_PrivateIDCMP       (ASL_TB + 42)
#define ASLFR_IntuiMsgFunc       (ASL_TB + 70)
#define ASLFR_SleepWindow        (ASL_TB + 43)
#define ASLFR_UserData           (ASL_TB + 52)
#define ASLFR_PopToFront         (ASL_TB + 131)
#define ASLFR_Activate           (ASL_TB + 132)
#define ASLFR_TextAttr           (ASL_TB + 51)
#define ASLFR_Locale             (ASL_TB + 50)
#define ASLFR_TitleText          (ASL_TB + 1)
#define ASLFR_PositiveText       (ASL_TB + 18)
#define ASLFR_NegativeText       (ASL_TB + 19)
#define ASLFR_InitialLeftEdge    (ASL_TB + 3)
#define ASLFR_InitialTopEdge     (ASL_TB + 4)
#define ASLFR_InitialWidth       (ASL_TB + 5)
#define ASLFR_InitialHeight      (ASL_TB + 6)
#define ASLFR_InitialFile        (ASL_TB + 8)
#define ASLFR_InitialDrawer      (ASL_TB + 9)
#define ASLFR_InitialPattern     (ASL_TB + 10)
#define ASLFR_InitialShowVolumes (ASL_TB + 130)
#define ASLFR_Flags1             (ASL_TB + 20)
#define ASLFR_Flags2             (ASL_TB + 22)
#define ASLFR_DoSaveMode         (ASL_TB + 44)
#define ASLFR_DoMultiSelect      (ASL_TB + 45)
#define ASLFR_DoPatterns         (ASL_TB + 46)
#define ASLFR_DrawersOnly        (ASL_TB + 47)
#define ASLFR_FilterFunc         (ASL_TB + 49)
#define ASLFR_RejectIcons        (ASL_TB + 60)
#define ASLFR_RejectPattern      (ASL_TB + 61)
#define ASLFR_AcceptPattern      (ASL_TB + 62)
#define ASLFR_FilterDrawers      (ASL_TB + 63)
#define ASLFR_HookFunc           (ASL_TB + 7)
#define ASLFR_SetSortBy          (ASL_TB + 124)
#define ASLFR_GetSortBy          (ASL_TB + 125)
#define ASLFR_SetSortDrawers     (ASL_TB + 126)
#define ASLFR_GetSortDrawers     (ASL_TB + 127)
#define ASLFR_SetSortOrder       (ASL_TB + 128)
#define ASLFR_GetSortOrder       (ASL_TB + 129)
