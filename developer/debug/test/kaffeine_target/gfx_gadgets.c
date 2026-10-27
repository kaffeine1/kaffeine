/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    GadTools gadgets, the way C programs create them, with no window and no
    input needed. A VisualInfo of the default public screen; a context and
    a gadget of every kind but LISTVIEW_KIND, with labels, the program's own
    TextAttr and UserData: a disabled button with a key letter, a checked
    checkbox, a cycle and an mx with labels, a palette with a colour table,
    a text, a number with a format, a slider with a level display, a
    scroller with arrows, a string longer than its MaxChars, an integer and
    a generic gadget. The program reads the list back through the 68k
    structures: the links, the gadgets that GadTools puts before a slider
    and a scroller, the IDs, UserData and flags, the labels and the
    StringInfo and PropInfo of the gadgets that have them. FreeGadgets()
    gives the guest memory back.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/text.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/gadtools.h>
#include <proto/intuition.h>
#include <string.h>

#include "m68ktest.h"

struct IntuitionBase *IntuitionBase;
struct Library *GadToolsBase;

/* The 68k layouts that programs read */
typedef char check_gadget[sizeof(struct Gadget) == 44 ? 1 : -1];
typedef char check_extgadget[sizeof(struct ExtGadget) == 56 ? 1 : -1];
typedef char check_stringinfo[sizeof(struct StringInfo) == 36 ? 1 : -1];
typedef char check_newgadget[sizeof(struct NewGadget) == 30 ? 1 : -1];

/* AROS marks every gadget of GadTools with this bit of GadgetType. */
#define GADTOOLS_TYPE 0x0100

enum
{
    ID_BUTTON = 1, ID_CHECKBOX, ID_CYCLE, ID_MX, ID_PALETTE, ID_TEXT, ID_NUMBER,
    ID_SLIDER, ID_SCROLLER, ID_STRING, ID_INTEGER, ID_GENERIC
};

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, 0 };
static STRPTR cycle_labels[] = { (STRPTR)"Low", (STRPTR)"Middle", (STRPTR)"High", NULL };
static STRPTR mx_labels[] = { (STRPTR)"One", (STRPTR)"Two", NULL };
static UBYTE colors[4] = { 3, 2, 1, 0 };

static struct NewGadget ng;

static void place(WORD top, const char *label, UWORD id, ULONG userdata)
{
    ng.ng_TopEdge = top;
    ng.ng_GadgetText = (CONST_STRPTR)label;
    ng.ng_GadgetID = id;
    ng.ng_UserData = (APTR)userdata;
}

static BOOL is_gadtools(struct Gadget *g)
{
    return (g->GadgetType & GADTOOLS_TYPE) != 0;
}

static void labels(struct Gadget *button, struct Gadget *cycle)
{
    struct IntuiText *it = button->GadgetText, *key = it ? it->NextText : NULL;

    if (!check(it && it->IText && key && key->IText, "the label of the button and its key letter"))
        return;
    check_str("the label without the underscore", (const char *)it->IText, "Ok");
    check_str("the key letter, again", (const char *)key->IText, "O");
    check(it->ITextFont && !strcmp((const char *)it->ITextFont->ta_Name, "topaz.font") &&
          it->ITextFont->ta_YSize == 8, "the label has the program's font");
    check(key->ITextFont && (key->ITextFont->ta_Style & FSF_UNDERLINED),
          "the key letter is underlined");
    it = cycle->GadgetText;
    check(it && it->IText && !strcmp((const char *)it->IText, "Level") && !it->NextText,
          "the label of the cycle");
}

int main(void)
{
    struct Gadget *glist = NULL, *context, *button, *checkbox, *cycle, *mx, *palette, *text;
    struct Gadget *number, *slider, *scroller, *string, *integer, *generic, *g;
    struct Screen *scr;
    APTR vi;
    ULONG avail, count;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 37);
    if (!check(IntuitionBase && GadToolsBase, "intuition.library and gadtools.library 37"))
        return test_done("gfx_gadgets");

    scr = LockPubScreen(NULL);
    vi = scr ? GetVisualInfoA(scr, NULL) : NULL;
    if (!check(scr && vi, "a VisualInfo of the public screen"))
        return test_done("gfx_gadgets");
    avail = AvailMem(MEMF_ANY);

    context = CreateContext(&glist);
    if (!check(context && glist == context && !context->NextGadget && is_gadtools(context),
               "CreateContext() gives the context, also in the list pointer"))
        return test_done("gfx_gadgets");

    ng.ng_LeftEdge = 100;
    ng.ng_Width = 120;
    ng.ng_Height = 14;
    ng.ng_TextAttr = &topaz8;
    ng.ng_VisualInfo = vi;

    place(10, "_Ok", ID_BUTTON, 0x1001);
    ng.ng_Flags = PLACETEXT_IN;
    button = CreateGadget(BUTTON_KIND, context, &ng, GT_Underscore, '_', GA_Disabled, TRUE, TAG_DONE);
    ng.ng_Flags = PLACETEXT_LEFT;
    place(30, NULL, ID_CHECKBOX, 0x1002);
    checkbox = CreateGadget(CHECKBOX_KIND, button, &ng, GTCB_Checked, TRUE, TAG_DONE);
    place(50, "Level", ID_CYCLE, 0x1003);
    cycle = CreateGadget(CYCLE_KIND, checkbox, &ng, GTCY_Labels, (ULONG)cycle_labels,
                         GTCY_Active, 1, TAG_DONE);
    place(70, NULL, ID_MX, 0x1004);
    mx = CreateGadget(MX_KIND, cycle, &ng, GTMX_Labels, (ULONG)mx_labels, GTMX_Spacing, 2, TAG_DONE);
    place(100, NULL, ID_PALETTE, 0x1005);
    palette = CreateGadget(PALETTE_KIND, mx, &ng, GTPA_NumColors, 4, GTPA_ColorTable, (ULONG)colors,
                           GTPA_Color, 2, TAG_DONE);
    place(120, NULL, ID_TEXT, 0x1006);
    text = CreateGadget(TEXT_KIND, palette, &ng, GTTX_Text, (ULONG)"Hello", GTTX_Border, TRUE, TAG_DONE);
    place(140, NULL, ID_NUMBER, 0x1007);
    number = CreateGadget(NUMBER_KIND, text, &ng, GTNM_Number, 42, GTNM_Format, (ULONG)"%ld%%",
                          TAG_DONE);
    place(160, "Volume", ID_SLIDER, 0x1008);
    slider = CreateGadget(SLIDER_KIND, number, &ng, GTSL_Min, 0, GTSL_Max, 100, GTSL_Level, 25,
                          GTSL_LevelFormat, (ULONG)"%3ld", GTSL_MaxLevelLen, 3, TAG_DONE);
    place(180, NULL, ID_SCROLLER, 0x1009);
    ng.ng_Width = 16;
    ng.ng_Height = 80;
    scroller = CreateGadget(SCROLLER_KIND, slider, &ng, GTSC_Arrows, 12, PGA_Freedom, LORIENT_VERT,
                            GTSC_Total, 100, GTSC_Visible, 10, TAG_DONE);
    ng.ng_Width = 120;
    ng.ng_Height = 14;
    place(270, "Name", ID_STRING, 0x100A);
    string = CreateGadget(STRING_KIND, scroller, &ng, GTST_String, (ULONG)"Hello, world",
                          GTST_MaxChars, 5, TAG_DONE);
    place(290, NULL, ID_INTEGER, 0x100B);
    integer = CreateGadget(INTEGER_KIND, string, &ng, GTIN_Number, -42, TAG_DONE);
    place(310, NULL, ID_GENERIC, 0x100C);
    generic = CreateGadget(GENERIC_KIND, integer, &ng, TAG_DONE);
    if (!check(button && checkbox && cycle && mx && palette && text && number && slider &&
               scroller && string && integer && generic, "a gadget of each kind"))
    {
        FreeGadgets(glist);
        return test_done("gfx_gadgets");
    }

    /* the list as GadTools links it, with the gadgets that it puts before
       a slider and a scroller */
    for (count = 0, g = glist; g && count < 100; g = g->NextGadget)
        count++;
    check_long("the gadgets of the list, the context and the added ones too", count, 16);
    check(context->NextGadget == button && button->NextGadget == checkbox &&
          checkbox->NextGadget == cycle && cycle->NextGadget == mx && mx->NextGadget == palette &&
          palette->NextGadget == text && text->NextGadget == number, "the simple kinds in their order");
    g = number->NextGadget;
    check(g && g != slider && g->NextGadget == slider && is_gadtools(g) && g->GadgetID == 0 &&
          !g->UserData, "the level display before the slider");
    g = slider->NextGadget;
    check(g && g->NextGadget && g->NextGadget->NextGadget == scroller &&
          g->GadgetID == ID_SCROLLER && g->NextGadget->GadgetID == ID_SCROLLER && !g->UserData,
          "two arrows with the ID of the scroller before it");
    check(g && (g->Flags & GFLG_LABELMASK) == GFLG_LABELIMAGE && g->GadgetText &&
          ((struct Image *)g->GadgetText)->Width > 0, "an arrow has an image as its label");
    check(scroller->NextGadget == string && string->NextGadget == integer &&
          integer->NextGadget == generic && !generic->NextGadget, "the end of the list");

    /* what the program reads of its gadgets */
    check(button->GadgetID == ID_BUTTON && button->UserData == (APTR)0x1001 &&
          generic->GadgetID == ID_GENERIC && generic->UserData == (APTR)0x100C &&
          slider->UserData == (APTR)0x1008 && scroller->UserData == (APTR)0x1009,
          "the IDs and UserData of the program");
    check(is_gadtools(button) && is_gadtools(slider) && is_gadtools(string) && is_gadtools(generic),
          "every gadget is one of GadTools");
    check(button->LeftEdge == 100 && button->TopEdge == 10 && button->Width == 120 &&
          button->Height == 14, "the place of the button");
    check(button->Flags & GFLG_DISABLED, "GA_Disabled");
    check(!button->GadgetRender && !button->SpecialInfo, "no native pointers in the button");
    check((checkbox->Flags & GFLG_SELECTED) && checkbox->Width == 26 && checkbox->Height == 11,
          "a checked checkbox of its own size");
    labels(button, cycle);
    check(slider->SpecialInfo &&
          (((struct PropInfo *)slider->SpecialInfo)->Flags & FREEHORIZ),
          "the slider has a horizontal PropInfo");
    check(scroller->SpecialInfo &&
          (((struct PropInfo *)scroller->SpecialInfo)->Flags & FREEVERT),
          "the scroller has a vertical PropInfo");
    if (check(string->SpecialInfo != NULL, "the string has a StringInfo"))
    {
        struct StringInfo *si = (struct StringInfo *)string->SpecialInfo;

        check_str("GTST_String cut to MaxChars", (const char *)si->Buffer, "Hello");
        check_long("MaxChars with its NUL", si->MaxChars, 6);
        check(!si->Extension && !(string->Flags & GFLG_STRINGEXTEND),
              "no native extension of the StringInfo");
    }
    if (check(integer->SpecialInfo != NULL, "the integer has a StringInfo"))
    {
        struct StringInfo *si = (struct StringInfo *)integer->SpecialInfo;

        check_long("LongInt", si->LongInt, -42);
        check_str("the number in the buffer", (const char *)si->Buffer, "-42");
        check(integer->Activation & GACT_LONGINT, "GACT_LONGINT");
    }
    check(generic->GadgetType == GADTOOLS_TYPE && (generic->Flags & GFLG_EXTENDED) &&
          (((struct ExtGadget *)generic)->MoreFlags & GMORE_BOUNDS) &&
          ((struct ExtGadget *)generic)->BoundsWidth == 120,
          "a generic gadget is an ExtGadget with its bounds");

    /* GadTools returns NULL without a previous gadget or a VisualInfo */
    check(!CreateGadget(BUTTON_KIND, NULL, &ng, TAG_DONE), "no gadget without a previous one");
    ng.ng_VisualInfo = NULL;
    check(!CreateGadget(BUTTON_KIND, generic, &ng, TAG_DONE) && !generic->NextGadget,
          "no gadget without a VisualInfo");

    FreeGadgets(glist);
    FreeGadgets(NULL);
    check_long("FreeGadgets() gives the guest memory back", AvailMem(MEMF_ANY), avail);
    FreeVisualInfo(vi);
    UnlockPubScreen(NULL, scr);

    CloseLibrary(GadToolsBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return test_done("gfx_gadgets");
}
