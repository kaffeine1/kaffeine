#pragma once
#include <exec/types.h>
#include <exec/ports.h>
#include <graphics/gfx.h>

struct TextAttr
{
    STRPTR ta_Name;
    UWORD ta_YSize;
    UBYTE ta_Style;
    UBYTE ta_Flags;
};

struct TagItem;

struct TTextAttr
{
    STRPTR tta_Name;
    UWORD tta_YSize;
    UBYTE tta_Style;
    UBYTE tta_Flags;
    struct TagItem *tta_Tags;
};

struct TextFont
{
    struct Message tf_Message;
    UWORD tf_YSize;
    UBYTE tf_Style;
    UBYTE tf_Flags;
    UWORD tf_XSize;
    UWORD tf_Baseline;
    UWORD tf_BoldSmear;
    UWORD tf_Accessors;
    UBYTE tf_LoChar;
    UBYTE tf_HiChar;
    APTR tf_CharData;
    UWORD tf_Modulo;
    APTR tf_CharLoc;
    APTR tf_CharSpace;
    APTR tf_CharKern;
};

struct TextExtent
{
    UWORD te_Width;
    UWORD te_Height;
    struct Rectangle te_Extent;
};

#define FS_NORMAL 0
#define FSF_UNDERLINED 0x01
#define FSF_COLORFONT 0x40
#define FSF_TAGGED 0x80
#define FPF_ROMFONT 0x01
#define FPF_DISKFONT 0x02
#define FPF_PROPORTIONAL 0x20
#define FPF_DESIGNED 0x40
#define TA_DeviceDPI 0x80000001UL
