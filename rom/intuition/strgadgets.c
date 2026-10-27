/*
    Copyright (C) 1995-2019, The AROS Development Team. All rights reserved.
    Copyright (C) 2001-2003, The MorphOS Development Team. All Rights Reserved.
*/

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/utility.h>
#include <proto/keymap.h>
#include <exec/memory.h>
#include <intuition/screens.h>
#include <intuition/cghooks.h>
#include <graphics/gfxmacros.h>
#include <devices/inputevent.h>
#include <aros/asmcall.h>
#include <stdlib.h> /* atol() */
#include <stdio.h>  /* snprintf() */
#include <string.h>

#include "intuition_intern.h"
#include "strgadgets.h"

#ifdef __MORPHOS__
size_t  stccpy(char *s1, const char *s2, size_t n);
#endif

#undef DEBUG
#define SDEBUG 0
#define DEBUG 0
#include <aros/debug.h>

/*****************************************************************************************/

#define CURSORPEN   0
#define STRBACKPEN  1
#define STRTEXTPEN  2

#define NUMPENS 3
#define STRALIGNMASK    (GACT_STRINGLEFT|GACT_STRINGCENTER|GACT_STRINGRIGHT)
/* A gadget with a StringExtend: GACT_STRINGEXTEND means the same as
   GFLG_STRINGEXTEND since V37, as on AmigaOS 3.2.3; either without an
   Extension leaves the gadget plain */
#define STRINGEXTENDED(gad) ((((gad)->Flags & GFLG_STRINGEXTEND) || \
                              ((gad)->Activation & GACT_STRINGEXTEND)) && \
                             ((struct StringInfo *)(gad)->SpecialInfo)->Extension)

#define KEYBUFSIZE  10
#define SHIFT       (IEQUALIFIER_LSHIFT|IEQUALIFIER_RSHIFT)

#define HELPRAW     95  /* Raw     */
#define CTRL_A      1
#define CTRL_B      2
#define CTRL_C      3
#define CTRL_D      4
#define CTRL_E      5
#define CTRL_F      6
#define CTRL_G      7
#define CTRL_H      8
#define CTRL_I      9
#define CTRL_J      10
#define CTRL_K      11
#define CTRL_L      12
#define CTRL_M      13
#define CTRL_N      14
#define CTRL_O      15
#define CTRL_P      16
#define CTRL_Q      17
#define CTRL_R      18
#define CTRL_S      19
#define CTRL_T      20
#define CTRL_U      21
#define CTRL_V      22
#define CTRL_W      23
#define CTRL_X      24
#define CTRL_Y      25
#define CTRL_Z      26

#define BACKSPACE   8   /* Vanilla */
#define TAB     9   /* Vanilla */
#define TABRAW      66  /* Raw     */
#define RETURN      13  /* Vanilla */
#define DELETE      127 /* Vanilla */
#define HOMERAW     112 /* Raw     */
#define ENDRAW      113 /* Raw     */

/*****************************************************************************************/

VOID UpdateStringInfo(struct Gadget *);

/*****************************************************************************************/

#define CharXSize(char, rp) ((rp->Font->tf_Flags & FPF_PROPORTIONAL) ?    \
                            rp->Font->tf_XSize : TextLength(rp, &(char), 1))


#undef MIN
#define MIN(a, b) ((a < b) ? a : b)

/*****************************************************************************************/

CONST char cursorkey[] = "_";
#define CURSORKEYLEN    1

/*****************************************************************************************/

STATIC WORD MaxDispPos(struct StringInfo *strinfo, struct BBox *bbox,
                       struct RastPort *rp, struct IntuitionBase *IntuitionBase)
{

    struct GfxBase *GfxBase = GetPrivIBase(IntuitionBase)->GfxBase;
    WORD                numfit, max_disppos, numchars;
    struct TextExtent   te;
    BOOL                cursor_at_end;

    cursor_at_end = (strinfo->BufferPos == strinfo->NumChars);

    EnterFunc(bug("MaxDispPos(current length: %d, bufferpos=%d)\n", strinfo->NumChars, strinfo->BufferPos));

    D(bug("cursor_at_end: %d\n", cursor_at_end));

    if (cursor_at_end) /* Cursor at end of string ? */
    {
        D(bug("Making cursor last char\n"));
        numchars = strinfo->NumChars + 1; /* Take cursor into account */

        /*  This has allready been done by UpdateDisp() which called us
            strinfo->Buffer[strinfo->NumChars] = 0x20;
         
        */
    }
    else
    {

        numchars = strinfo->NumChars;
    }


    /* Find the amount of characters that fit into the bbox, counting
    ** from the last character in the buffer and forward,
    */
    numfit = TextFit(rp,
                     &(strinfo->Buffer[numchars - 1]),
                     numchars, &te, NULL,
                     -1, bbox->Width, rp->Font->tf_YSize);


    max_disppos = numchars - numfit;

    /*    if ((max_disppos > 0) && (!cursor_at_end))
        max_disppos --;
      */

    D(bug("Numchars w/cursor: %d, Numfit: %d, maxdisppos=%d  bbox->Width = %d  te->te_Width = %d\n",
          numchars, numfit, max_disppos, bbox->Width, te.te_Width));

    ReturnInt("MaxDispPos", WORD, max_disppos);
}

/*****************************************************************************************/

void UpdateDisp(struct Gadget       *gad,
                struct BBox     *bbox,
                struct RastPort     *rp,
                struct IntuitionBase    *IntuitionBase)
{


    struct GfxBase *GfxBase = GetPrivIBase(IntuitionBase)->GfxBase;
    struct TextExtent    te;
    struct StringInfo   *strinfo = (struct StringInfo *)gad->SpecialInfo;
    STRPTR               dispstr;

    EnterFunc(bug("UpdateDisp(gad=%p, bbox=%p, rp=%p)\n",
                  gad, bbox, rp));

    /* If the cursor is at the trailing \0, insert a SPACE instead */
    if (strinfo->BufferPos == strinfo->NumChars)
        strinfo->Buffer[strinfo->NumChars] = 0x20;

    /* In this function we check if the cursor has gone outside
    ** of the visible area (because of application setting
    ** strinfo->BufferPos or strinfo->DispPos to a different value, or
    ** because of user input).
    ** This is made a bit difficult by the rule (R), that there
    ** should NOT be available space on the right, and characters
    ** scrolled out at the left, at the same time.
    ** We have 3 possible scenarios:
    ** 1) Cursor to the left of DispPos:
    **    Set DispPos to the lowest of BufferPos and the
    **    maximum allowed disppos (according to (R) ).
    ** 2) Cursor to the right of visible area:
    **    Set dispose sou that the cursor is the last visible character.
    **    This afheres to (R).
    ** 3) Cursor inside visible area. Do a check on rule (R),
    **    and if DispPos > max allowed, then adjust it down,
    **    so that the last character in the buffer becomes last character
    **    displayed. (The cursor will still be visible after adjustion)
    */


    /* 1) Cursor to the left of visible area */
    if (strinfo->BufferPos < strinfo->DispPos)
    {
        WORD max_disppos;

        max_disppos = MaxDispPos(strinfo, bbox, rp, IntuitionBase);
        strinfo->DispPos = MIN(strinfo->BufferPos, max_disppos);
    }
    else /* Cursor equal to the right of disppos [ 2) or 3) ] */
    {
        UWORD strsize;

        /* How many pixels are there from current 1st displayed to the cursor ? */
        strsize = TextLength(rp,
                             strinfo->Buffer + strinfo->DispPos,
                             strinfo->BufferPos - strinfo->DispPos + 1);

        /* 2) More than fits into the gadget ? */
        if (strsize > bbox->Width)
        {
            WORD numfit;

            /* Compute new DispPos such that the cursor is at the right */
            numfit = TextFit(rp,
                             &(strinfo->Buffer[strinfo->BufferPos]),
                             strinfo->NumChars, &te, NULL, -1,
                             bbox->Width, rp->Font->tf_YSize);

            /* In a box narrower than the character at the cursor nothing
               fits, and the text then starts at the cursor, not past it */
            strinfo->DispPos = strinfo->BufferPos - ((numfit > 0) ? numfit - 1 : 0);

            D(bug("cursor right of visible area, new disppos: %d\n", strinfo->DispPos));
        }
        else /* 3). Cursor inside gadget */
        {
            WORD max_disppos;

            max_disppos = MaxDispPos(strinfo, bbox, rp, IntuitionBase);
            if (strinfo->DispPos > max_disppos)
                strinfo->DispPos = max_disppos;

        } /* if (cursor inside or to the right of visible area )*/

    }

    /* Update the DispCount */
    /* It might be necessary with special handling for centre aligned gads */
    dispstr = &(strinfo->Buffer[strinfo->DispPos]);
    strinfo->DispCount = TextFit(rp, dispstr,
                                 strinfo->NumChars - strinfo->DispPos,
                                 &te, NULL, 1,
                                 bbox->Width,
                                 rp->Font->tf_YSize);

    /* 0-terminate string */
    strinfo->Buffer[strinfo->NumChars] = 0x00;
    ReturnVoid("UpdateDisp");
}

/*****************************************************************************************/

/* Where the text of a centred or right aligned string gadget starts in its
   box, as on AmigaOS 3.1 and 3.2.3. Such a gadget aligns what it draws:
   the characters from DispPos that fit into the box, which UpdateDisp()
   counts in DispCount, and the cursor after them when it is at the end of
   the text. Not the rest of a text that does not fit, which would start
   left of the box. The room it aligns them in is the box; for a plain
   gadget, one without a StringExtend, in a font of fixed width, the whole
   characters that fit into the box, which a text that does not fit fills,
   so that it shows such a text from the left of its box. Right aligned
   text ends at the right edge of the room. Returns the offset of the text
   into the box, never below 0, and in textwidth the width aligned. */
STATIC WORD AlignedText(struct Gadget    *gad,
                        struct BBox      *bbox,
                        struct RastPort  *rp,
                        WORD             *textwidth,
                        struct IntuitionBase *IntuitionBase)
{
    struct GfxBase      *GfxBase = GetPrivIBase(IntuitionBase)->GfxBase;
    struct StringInfo   *strinfo = (struct StringInfo *)gad->SpecialInfo;
    STRPTR               dispstr = &(strinfo->Buffer[strinfo->DispPos]);
    WORD                 dispstrlen = strinfo->NumChars - strinfo->DispPos;
    WORD                 room = bbox->Width;
    WORD                 drawn, offset;
    struct TextExtent    te;

    if (dispstrlen < 0)
        dispstrlen = 0;
    drawn = TextFit(rp, dispstr, dispstrlen, &te, NULL, 1, bbox->Width, rp->Font->tf_YSize);
    *textwidth = TextLength(rp, dispstr, drawn);
    if (drawn == dispstrlen && strinfo->BufferPos == strinfo->NumChars)
        *textwidth += TextLength(rp, cursorkey, CURSORKEYLEN);

    if (!STRINGEXTENDED(gad) && !(rp->Font->tf_Flags & FPF_PROPORTIONAL) &&
        rp->Font->tf_XSize > 0)
        room -= room % rp->Font->tf_XSize;

    offset = room - *textwidth;
    if ((gad->Activation & STRALIGNMASK) == GACT_STRINGCENTER)
        offset /= 2;

    return (offset > 0) ? offset : 0;
}

/*****************************************************************************************/

STATIC WORD GetTextLeft(struct Gadget      *gad,
                        struct BBox        *bbox,
                        struct RastPort    *rp,
                        struct IntuitionBase   *IntuitionBase)
{
    /* Gets left position of text in the string gadget */

    WORD                 text_left = 0;
    WORD                 textwidth;

    /* Calcluate start offset of gadget text */
    switch (gad->Activation & STRALIGNMASK)
    {
        case GACT_STRINGLEFT:
        /* FIXME: is this default: correct? */
        default:
            text_left = bbox->Left;
            break;

        case GACT_STRINGCENTER:
        case GACT_STRINGRIGHT:
            text_left = bbox->Left + AlignedText(gad, bbox, rp, &textwidth, IntuitionBase);
            break;
    }
    return (text_left);
}

/*****************************************************************************************/

STATIC WORD GetTextRight(struct Gadget     *gad,
                         struct BBox       *bbox,
                         struct RastPort   *rp,
                         struct IntuitionBase  *IntuitionBase)
{
    /* Gets right offset of text in the string gadget */

    struct GfxBase      *GfxBase = GetPrivIBase(IntuitionBase)->GfxBase;
    struct StringInfo   *strinfo = (struct StringInfo *)gad->SpecialInfo;
    WORD                 text_right = 0;
    STRPTR               dispstr = &(strinfo->Buffer[strinfo->DispPos]);
    UWORD                dispstrlen;
    WORD                 textwidth;

    dispstrlen = strinfo->NumChars - strinfo->DispPos;

    /* Calcluate start offset of gadget text */
    switch (gad->Activation & STRALIGNMASK)
    {
    case GACT_STRINGLEFT:
        /* FIXME: is this default: correct? */
        default:
            text_right =  bbox->Left + TextLength(rp, dispstr, dispstrlen);
            break;

        case GACT_STRINGCENTER:
        case GACT_STRINGRIGHT:
            text_right = bbox->Left + AlignedText(gad, bbox, rp, &textwidth, IntuitionBase);
            text_right += textwidth - 1;
            break;
    }
    return (text_right);
}

/*****************************************************************************************/

/* The font of a string gadget, which the render draws its text in and a click
   measures it with: that of its StringExtend, else the screen's, also for a
   gadget without a StringExtend, as on AmigaOS. Neither GfxBase->DefaultFont
   nor the font of the window's RastPort plays a part there. */
STATIC struct TextFont *StrGadgetFont(struct Gadget *gad, struct DrawInfo *dri)
{
    if (STRINGEXTENDED(gad))
    {
        struct StringExtend *strext = ((struct StringInfo *)gad->SpecialInfo)->Extension;

        if (strext->Font)
            return strext->Font;
    }

    return dri->dri_Font;
}

/*****************************************************************************************/

STATIC VOID GetPensAndFont(struct Gadget *gad,
                           UWORD     *pens,
                           struct Window *win,
                           struct RastPort *rp,
                           struct IntuitionBase *IntuitionBase)
{

    struct GfxBase  *GfxBase = GetPrivIBase(IntuitionBase)->GfxBase;
    struct DrawInfo *dri = GetScreenDrawInfo(win->WScreen);

    SetFont(rp, StrGadgetFont(gad, dri));

    if (STRINGEXTENDED(gad))
    {
        struct StringExtend *strext;

        strext = ((struct StringInfo *)gad->SpecialInfo)->Extension;

        if ((gad->Flags & GFLG_SELECTED) == GFLG_SELECTED)
        {
            pens[STRTEXTPEN]    = strext->ActivePens[0];
            pens[STRBACKPEN]    = strext->ActivePens[1];
        }
        else
        {
            pens[STRTEXTPEN]    = strext->Pens[0];
            pens[STRBACKPEN]    = strext->Pens[1];
        }

    }
    else
    {
        /* We don't care to lock the screen because the window we're
        ** drawn on does this. If the window is moved to another
        ** public screen we will be called again to rerender (and get new
        ** valid pens). If the window is closed, and the screen is closed
        ** we won't be here anymore.
        */

#if 0
        if ((gad->Flags & GFLG_SELECTED) == GFLG_SELECTED)
        {
            pens[STRTEXTPEN]    = dri->dri_Pens[TEXTPEN];
            pens[STRBACKPEN]    = dri->dri_Pens[BACKGROUNDPEN];

        }
        else
        {
#endif
            pens[STRTEXTPEN]    = dri->dri_Pens[TEXTPEN];
            pens[STRBACKPEN]    = dri->dri_Pens[BACKGROUNDPEN];
#if 0
        }
#endif

    }
    pens[CURSORPEN] = dri->dri_Pens[FILLPEN];

    FreeScreenDrawInfo(win->WScreen, dri);

    return;
}

/*****************************************************************************************/

ULONG HandleStrInput(   struct Gadget       *gad,
                      struct GadgetInfo *ginfo,
                      struct InputEvent *ievent,
                      UWORD         *imsgcode,
                      struct IntuitionBase  *IntuitionBase)
{
    struct Library      *UtilityBase = GetPrivIBase(IntuitionBase)->UtilityBase;
    struct Library      *KeymapBase = GetPrivIBase(IntuitionBase)->KeymapBase;
    struct SGWork        sgw;
    struct StringInfo   *strinfo = (struct StringInfo *)gad->SpecialInfo;
    struct StringExtend *strext = NULL;
    STRPTR               workbuffer = NULL, longcopy = NULL;
    WORD                 workroom = 0;
    ULONG                command = 0;

    EnterFunc(bug("HandleStrInput(gad=%p, ginfo=%p, ievent=%p)\n",
                  gad, ginfo, ievent));

    if ((ievent->ie_Class == IECLASS_TIMER)) return 0;

    D(bug("Gadget text: %s\n", strinfo->Buffer));

    if (!ginfo)
        ReturnInt("HandleStrInput", ULONG, 0UL);

    UpdateStringInfo(gad);

    /* Initialize SGWork */
    sgw.Gadget  = gad;
    sgw.StringInfo  = strinfo;
    sgw.WorkBuffer  = strinfo->Buffer; /* default */
    sgw.PrevBuffer  = strinfo->Buffer;
    sgw.Modes   = 0;
    sgw.IEvent  = ievent;
    sgw.Code    = 0;
    sgw.BufferPos   = strinfo->BufferPos;
    sgw.NumChars    = strinfo->NumChars;
    /* the two read apart: a refresh by the program between them may have
       counted a shorter text */
    if (sgw.BufferPos > sgw.NumChars)
        sgw.BufferPos = sgw.NumChars;
    if (sgw.BufferPos < 0)
        sgw.BufferPos = 0;
    sgw.Actions = 0;
    sgw.LongInt = strinfo->LongInt;
    sgw.GadgetInfo  = ginfo;
    sgw.EditOp  = EO_NOOP;

    if (STRINGEXTENDED(gad))
    {
        D(bug("HandleStrInput: Extended gadget\n"));
        strext = strinfo->Extension;
        if (strext->WorkBuffer)
        {
            /* The edit hooks get a *copy* of the current buffer contents,
               in the work buffer, which has MaxChars bytes like the buffer.
               A longer text, which only the program can have put into its
               buffer, gets a copy of its own length instead: AmigaOS copies
               all of it too, but past the end of the work buffer. Without
               memory for that, Intuition's own hook edits the buffer
               itself, as for a gadget without a work buffer; but a hook of
               the program, the gadget's own or one installed with
               SetEditHook(), could not reject the change any more, so the
               event does nothing then. */
            /* the count of sgw, which the edit uses: a refresh by the
               program meanwhile may count the text anew */
            if (sgw.NumChars < strinfo->MaxChars && !strinfo->Buffer[sgw.NumChars])
            {
                workbuffer = strext->WorkBuffer;
                workroom = strinfo->MaxChars;
            }
            else
            {
                /* room for every edit that DoSGHKey() allows: the text of
                   that count, and MaxChars at least, as the text may have
                   grown since */
                workroom = sgw.NumChars + 1 > strinfo->MaxChars ? sgw.NumChars + 1
                                                                 : strinfo->MaxChars;
                workbuffer = longcopy = AllocVec(workroom, MEMF_ANY);
                if (!longcopy && (strext->EditHook || GetPrivIBase(IntuitionBase)->GlobalEditHook !=
                                  &GetPrivIBase(IntuitionBase)->DefaultEditHook))
                    ReturnInt("HandleStrInput", ULONG, 0UL);
            }
            if (workbuffer)
            {
                sgw.WorkBuffer = workbuffer;
                CopyStrGadText(workbuffer, strinfo->Buffer, workroom);
            }
        }
        sgw.Modes = strext->InitialModes;
    }

    switch (ievent->ie_Class)
    {
        case IECLASS_RAWMOUSE:
            if (ievent->ie_Code == SELECTDOWN)
            {
                command = SGH_CLICK;
                sgw.Actions = SGA_USE | SGA_REDISPLAY;
                D(bug("HandleStrInput: RAWMOUSE event\n"));
            }
            break;

        case IECLASS_RAWKEY:
        {
            UBYTE buf;

            D(bug("HandleStrInput: RAWKEY event\n"));
            if (ievent->ie_Code & IECODE_UP_PREFIX)
            {
                D(bug("HandleStrInput: filter UP event\n"));
            }
            else
            {
                command = SGH_KEY;
                sgw.Actions = SGA_USE;
                /* the keymap of the gadget only with GACT_ALTKEYMAP, as
                   on AmigaOS 3.2.3 */
                if (1 == MapRawKey(sgw.IEvent, &buf, 1,
                                   (gad->Activation & GACT_ALTKEYMAP) ? strinfo->AltKeyMap : NULL))
                {
                    D(bug("HandleStrInput: sgw.Code 0x%lx\n",buf));
                    sgw.Code = (UWORD)buf;
                }
            }
            break;
        }

    }

    if (!command)
    {
        FreeVec(longcopy);
        ReturnInt("HandleStrInput", ULONG , 0UL);
    }

    /* Call the global editing hook */

    D(bug("calling global hook, Buffer=%s, WorkBuffer=%s\n",
          strinfo->Buffer, sgw.WorkBuffer));
    CallHookPkt(GetPrivIBase(IntuitionBase)->GlobalEditHook, &sgw, &command);

    /* If there is a local edit hook, run it */
    if (strext)
    {
        if (strext->EditHook)
        {
            D(bug("calling local edit hook\n"));
            CallHookPkt(strext->EditHook, &sgw, &command);
        }
    }

    /* Copy possibly changed stuff into stringgad */
    if (sgw.Actions & SGA_USE)
    {
        /* The result of the hooks, back into the buffer, no longer than
           the copy could hold */
        if (workbuffer)
        {
            WORD len = CopyStrGadText(strinfo->Buffer, workbuffer, workroom);

            if (sgw.NumChars > len)
                sgw.NumChars = len;
        }

        strinfo->BufferPos = sgw.BufferPos;
        strinfo->NumChars  = sgw.NumChars;
        strinfo->LongInt   = sgw.LongInt;

#if 0
        if (gad->Activation & GACT_LONGINT)
        {
            kprintf("strinfo->LongInt = %d\n",strinfo->LongInt);
            */
        }
        else
        {
            kprintf("strinfo->Buffer = \"%s\"\n",strinfo->Buffer);
            */
        }
#endif
    }
    FreeVec(longcopy);

    if (sgw.Actions & SGA_BEEP)
    {
        D(bug("SGA_BEEP not yet implemented. (lack of DisplayBeep())\n"));
    }

    if (sgw.Actions & (SGA_END | SGA_NEXTACTIVE | SGA_PREVACTIVE))
    {
        gad->Flags &= ~GFLG_SELECTED;
        *imsgcode = sgw.Code;
        D(bug("HandleStrInput: SGA_END\n"));
    }

    if (sgw.Actions & SGA_REDISPLAY)
    {
        D(bug("HandleStrInput: SGA_REDISPLAY\n"));
        /* Hack for making strgclass work */
        if ((gad->GadgetType & GTYP_GTYPEMASK) == GTYP_CUSTOMGADGET)
        {
            struct RastPort *rp;
            D(bug("HandleStrInput: Rerendering boopsi gadget\n"));

            if ((rp = ObtainGIRPort(ginfo)) != NULL)
            {
                struct gpRender method;

                method.MethodID   = GM_RENDER;
                method.gpr_GInfo  = ginfo;
                method.gpr_RPort  = rp;
                method.gpr_Redraw = GREDRAW_UPDATE;
                
                Custom_DoMethodA(IntuitionBase, gad, (Msg)&method);

                ReleaseGIRPort(rp);
            }
        } /* if gadget is a strgclass object */
        else
        {
            D(bug("HandleStrInput: Rerendering intuition gadget\n"));
            RefreshStrGadget(gad, ginfo->gi_Window, ginfo->gi_Requester, IntuitionBase);
        }
    }

    ReturnInt("HandleStrInput", ULONG, sgw.Actions);
}

/*****************************************************************************************/

STATIC ULONG DoSGHClick(struct SGWork *sgw, struct IntuitionBase *IntuitionBase)
{

    struct GfxBase      *GfxBase = GetPrivIBase(IntuitionBase)->GfxBase;
    struct Gadget       *gad;
    struct StringInfo   *strinfo;
    struct BBox          bbox;

    struct TextFont     *oldfont;
    struct RastPort     *rp;
    struct Window       *window;
    struct Requester    *req;

    WORD                 text_left, text_right;
    WORD                 mousex;

    window = sgw->GadgetInfo->gi_Window;
    req = sgw->GadgetInfo->gi_Requester;

    GetGadgetDomain(sgw->Gadget, window->WScreen, window, NULL, (struct IBox *)&bbox);
    mousex = window->MouseX - bbox.Left;

    EnterFunc(bug("DoSGHClick(sgw=%p)\n", sgw));

    D(bug("Gadget text: %s\n", sgw->WorkBuffer));

    rp      = sgw->GadgetInfo->gi_RastPort;
    oldfont = rp->Font;
    gad     = sgw->Gadget;
    strinfo = (struct StringInfo *)gad->SpecialInfo;

    CalcBBox(window, req, gad, &bbox);

    /* The font the gadget is drawn in, so that the click lands on the
       character shown under the pointer */
    {
        struct DrawInfo *dri;

        dri = GetScreenDrawInfo(window->WScreen);
        SetFont(rp, StrGadgetFont(gad, dri));
        FreeScreenDrawInfo(window->WScreen, dri);
    }

    /* If we are made active, save contents to undobuffer (if any exists) */
    if (!(gad->Flags & GFLG_SELECTED))
    {

        if (strinfo->UndoBuffer)
        {
            D(bug("sghclick: saving into undo buffer\n"));
            CopyStrGadText(strinfo->UndoBuffer, strinfo->Buffer, strinfo->MaxChars);
        }
        gad->Flags |= GFLG_SELECTED;
    }

    /* Get left & righ offsets of strgad text */
    text_left  = GetTextLeft (gad, &bbox, rp, IntuitionBase);
    text_right = GetTextRight(gad, &bbox, rp, IntuitionBase);

    D(bug("sghclick: text_left=%d, text_right=%d\n", text_left, text_right));
    D(bug("sghclick: disppos=%d, dispcount=%d, cursor=%d\n",
          strinfo->DispPos, strinfo->DispCount, sgw->BufferPos));
    D(bug("Gadget text: %s\n", sgw->WorkBuffer));

    /* Check if mouseclick is inside displayed text */
    if ((mousex >= text_left) && (mousex <= text_right))
    {
        /* Find new cursor pos. Uses TextFit() to handle proportional fonts. */

        struct TextExtent te;
        STRPTR            dispstr = strinfo->Buffer + strinfo->DispPos;
        WORD              dispchars = sgw->NumChars - strinfo->DispPos;

        sgw->BufferPos =   strinfo->DispPos
                           + TextFit(rp, dispstr, (dispchars > 0) ? dispchars : 0,
                                     &te, NULL, 1,
                                     mousex - text_left, rp->Font->tf_YSize);
        D(bug("sghclick: click inside text.\n"));
    }
    else /* Click not inside text */
    {
        if (mousex < text_left)
        {
            /* Click on empty space at left. Set cursor to first visible */
            sgw->BufferPos = strinfo->DispPos;
            D(bug("sghclick: click left of text.\n"));
        }
        else
        {
            /* Click on empty space at right. Set cursor to last visible */
            sgw->BufferPos = strinfo->DispPos + strinfo->DispCount;
            D(bug("sghclick: click right of text.\n"));
        }

    } /* if (click is on text or not) */

    /* No further than the end of the text: DispCount is that of the last
       render, which may have shown a longer text, and a global edit hook of
       the program may have shortened the text before passing the click on
       to this one */
    if (sgw->BufferPos > sgw->NumChars)
        sgw->BufferPos = sgw->NumChars;

    D(bug("sghclick: new cursor position: %d\n", sgw->BufferPos));

    sgw->Actions = (SGA_USE|SGA_REDISPLAY);
    sgw->EditOp = EO_MOVECURSOR;

    SetFont(rp, oldfont);

    D(bug("Gadget text: %s\n", sgw->WorkBuffer));

    ReturnInt ("DoSGHClick", ULONG, 1);
}

/*****************************************************************************************/

/* Copy the text of a string gadget into one of its buffers, which the RKRM
   sizes by MaxChars, the undo and the work buffer like the buffer itself: no
   more than MaxChars - 1 characters, and a NUL. Returns the characters
   copied. A buffer that a program gives twice already holds its text, and
   keeps all of it. AmigaOS copies MaxChars characters, without the NUL,
   into the undo buffer. */
WORD CopyStrGadText(STRPTR dest, CONST_STRPTR text, WORD maxchars)
{
    WORD len = 0;

    if (maxchars < 1)
        return 0;
    while (len < maxchars - 1 && text[len])
        len++;
    if (dest != text)
    {
        memmove(dest, text, len);
        dest[len] = '\0';
    }

    return len;
}

/*****************************************************************************************/

VOID MoveCharsLeft(STRPTR str, UWORD first, UWORD last, UWORD steps)
{
    register UWORD i;

    for (i = first; i <= last; i ++)
    {
        str[i - steps] = str[i];
    }
    str[last] = 0;
}

/*****************************************************************************************/

STATIC ULONG DoSGHKey(struct SGWork *sgw, struct IntuitionBase *IntuitionBase)
{

    struct Library      *UtilityBase = GetPrivIBase(IntuitionBase)->UtilityBase;
    struct Gadget       *gad;
    struct StringInfo   *strinfo;
    UBYTE                letter;
    ULONG                qual;

    EnterFunc(bug("DoSGHKey(sgw=%p)\n", sgw));

    gad = sgw->Gadget;
    strinfo = sgw->StringInfo;

    qual = sgw->IEvent->ie_Qualifier;

    D(bug("sghkey: RawKey 0x%lx, sgw_Code 0x%lx\n",sgw->IEvent->ie_Code,sgw->Code));

    sgw->EditOp = EO_NOOP;

    D(bug("sghkey: qual 0x%lx ic_Flags 0x%lx Modes 0x%lx\n",
          qual,
          GetPrivIBase(IntuitionBase)->IControlPrefs.ic_Flags,
          sgw->Modes));

    if (sgw->Code == 0)
    {
        /* RAW Keys */

        letter = sgw->IEvent->ie_Code;

        if ((letter == CURSORLEFT) && (!(qual & SHIFT)))
        {
            D(bug("sghkey: CURSOR_LEFT\n"));
            if (sgw->BufferPos > 0)
            {
                sgw->EditOp = EO_MOVECURSOR;
                sgw->BufferPos --;
            }
        }
        else if ((letter == CURSORRIGHT) && (!(qual & SHIFT)))
        {

            D(bug("sghkey: CURSOR_RIGHT\n"));
            if (sgw->BufferPos < sgw->NumChars)
            {
                sgw->EditOp = EO_MOVECURSOR;
                sgw->BufferPos ++;

            }
        }
        else if ( ((letter == CURSORLEFT) && (qual & SHIFT)) ||
                  (letter == HOMERAW) )
        {
            if (sgw->BufferPos > 0)
            {
                sgw->BufferPos = 0;
                sgw->EditOp = EO_MOVECURSOR;
            }
        }
        else if ( ((letter == CURSORRIGHT) && (qual & SHIFT)) ||
                  (letter == ENDRAW) )
        {
            if (sgw->BufferPos < sgw->NumChars)
            {
                sgw->BufferPos = sgw->NumChars;
                sgw->EditOp = EO_MOVECURSOR;
            }
        }
        else if ((letter == TABRAW) && (qual & SHIFT))
        {
            D(bug("sghkey: SHIFT TAB\n"));
            sgw->EditOp = EO_SPECIAL; /* FIXME: ??? is this correct ??? */
            sgw->Code = 9;
            sgw->Actions = (SGA_USE|SGA_PREVACTIVE);
        }
        else if (letter == HELPRAW)
        {}

    }
    else if (qual & IEQUALIFIER_RCOMMAND)
    {
        /* ANSI key but pressed together with right Amiga key */
    #if 0
        sgw->EditOp  = EO_SPECIAL; /* FIXME: ??? is this correct ??? */
        sgw->Actions = (SGA_USE|SGA_REUSE|SGA_END);
    #endif
        letter = ToUpper(sgw->Code);

    /* stegerg: URGENTCHECMKE. Why disabled for AROS? */
    #ifndef __MORPHOS__
        /* FIXME:  we need to handle RAMIGA-Q,RAMIGA-X here */
    #else
        if(letter == 'Q')
        {
            sgw->EditOp = EO_RESET;

            if (strinfo->UndoBuffer)
            {
                D(bug("sghclick: saving into undo buffer\n"));
                stccpy(strinfo->UndoBuffer, sgw->WorkBuffer,strinfo->MaxChars);
                sgw->BufferPos = strinfo->UndoPos;
                sgw->NumChars = strlen(sgw->WorkBuffer);
            }
        }
        else
        if(letter == 'X')
        {
            /* RCmd-X  clears the input buffer. In fixed field mode
             * jump cursor to the start of the buffer
             */
            D(bug("sghkey: CTRL-X\n"));

            if (sgw->Modes & SGM_FIXEDFIELD)
            {
                D(bug("sghkey: SGM_FIXEDFIELD\n"));
                sgw->BufferPos = 0;
                sgw->EditOp = EO_MOVECURSOR;
            }
            else
            {
                sgw->BufferPos = 0;
                sgw->WorkBuffer[sgw->BufferPos] = '\0';
                sgw->NumChars = sgw->BufferPos;
                sgw->EditOp = EO_CLEAR;
            }
        }
    #endif
    }
    else
    {
        /* ANSI key */

        letter = sgw->Code;

        if ((qual & IEQUALIFIER_CONTROL) &&
            (GetPrivIBase(IntuitionBase)->IControlPrefs.ic_Flags & ICF_STRGAD_FILTER) &&
            !(sgw->Modes & SGM_NOFILTER))
        {
            /* ANSI key but pressed together with the control key */

            D(bug("sghkey: CTRL pressed..filtering on\n"));

            if (letter == CTRL_A)
            {
                /* CTRL-A  jump cursor to start of buffer */
                D(bug("sghkey: CTRL-A\n"));
                sgw->BufferPos = 0;
                sgw->EditOp = EO_MOVECURSOR;
            }
            else
                if (letter == CTRL_B)
                {
                    /* CTRL-B  move the cursor back one char */
                    D(bug("sghkey: CTRL-B\n"));
                    if (sgw->BufferPos > 0)
                    {
                        sgw->BufferPos--;
                        sgw->EditOp = EO_MOVECURSOR;
                    }
                }
                else
                    if (letter == CTRL_D)
                    {
                        /* CTRL-D  delete current char */
                        D(bug("sghkey: CTRL-D\n"));
                        if (sgw->BufferPos != sgw->NumChars)
                        {
                            MoveCharsLeft(sgw->WorkBuffer, sgw->BufferPos + 1, sgw->NumChars - 1, 1);
                            sgw->NumChars --;
                            sgw->EditOp = EO_DELFORWARD;
                        }
                    }
                    else
                        if (letter == CTRL_E)
                        {
                            /* CTRL-E  move the cursor to the end of the line */
                            D(bug("sghkey: CTRL-E\n"));
                            if (sgw->BufferPos != sgw->NumChars)
                            {
                                sgw->BufferPos = sgw->NumChars;
                                sgw->EditOp = EO_MOVECURSOR;
                            }
                        }
                        else
                            if (letter == CTRL_F)
                            {
                                /* CTRL-F  move the cursor forward one char */
                                D(bug("sghkey: CTRL-F\n"));
                                if (sgw->BufferPos != sgw->NumChars)
                                {
                                    sgw->BufferPos++;
                                    sgw->EditOp = EO_MOVECURSOR;
                                }
                            }
                            else
                                if (letter == CTRL_H)
                                {
                                    /* CTRL-H  Delete the character to the left of the cursor
                                     * In fixed field mode, move cursor to previous character
                                     */
                                    D(bug("sghkey: CTRL-A\n"));
                                    if (sgw->Modes & SGM_FIXEDFIELD)
                                    {
                                        D(bug("sghkey: SGM_FIXEDFIELD\n"));
                                        if (sgw->BufferPos > 0)
                                        {
                                            sgw->BufferPos--;
                                            sgw->EditOp = EO_MOVECURSOR;
                                        }
                                    }
                                    else
                                    {
                                        if (sgw->BufferPos > 0)
                                        {
                                            int len;
                                            len = sgw->NumChars - sgw->BufferPos;
                                            memmove(&sgw->WorkBuffer[sgw->BufferPos-1],&sgw->WorkBuffer[sgw->BufferPos],len);

                                            sgw->BufferPos--;
                                            sgw->WorkBuffer[sgw->BufferPos + len] = 0;
                                            sgw->NumChars--;
                                            sgw->EditOp = EO_DELBACKWARD;
                                        }
                                    }
                                }
                                else
                                    if (letter == CTRL_K)
                                    {
                                        /* CTRL-K Delete from the character under the cursor to the end
                                         * of the string
                                         */
                                        D(bug("sghkey: CTRL-K\n"));
                                        sgw->WorkBuffer[sgw->BufferPos] = 0;
                                        sgw->NumChars = sgw->BufferPos;
                                        sgw->EditOp = EO_DELFORWARD;
                                    }
                                    else
                                        if (letter == CTRL_M)
                                        {
                                            /* CTRL-M Equivalent to Return or Enter
                                             */
                                            D(bug("sghkey: CTRL-M\n"));
                                            sgw->EditOp  = EO_ENTER;
                                            sgw->Code    = 0;
                                            sgw->Actions = (SGA_USE|SGA_END);
                                        }
                                        else
                                            if (letter == CTRL_W)
                                            {
                                                /* CTRL-W  Delete the previous word. In fixed field mode, jump
                                                 * cursor to the start of the previous word
                                                 */
                                                D(bug("sghkey: CTRL-W\n"));
                                                if (sgw->Modes & SGM_FIXEDFIELD)
                                                {
                                                    D(bug("sghkey: SGM_FIXEDFIELD\n"));
                                                }
                                                else
                                                {
                                                    if (sgw->BufferPos > 0)
                                                    {
                                                        int len;
                                                        int pos;
                                                        /*
                                                         * search the space before the word; with no space
                                                         * before it, the word starts the buffer
                                                         */
                                                        for (pos=sgw->BufferPos-1;pos > 0;pos--)
                                                        {
                                                            if (sgw->WorkBuffer[pos] == ' ')
                                                            {
                                                                break;
                                                            }
                                                        }
                                                        len = sgw->NumChars - sgw->BufferPos;
                                                        memmove(&sgw->WorkBuffer[pos],&sgw->WorkBuffer[sgw->BufferPos],len);

                                                        sgw->NumChars -= sgw->BufferPos - pos;
                                                        sgw->BufferPos = pos;
                                                        sgw->WorkBuffer[sgw->BufferPos + len] = 0;
                                                        sgw->EditOp = EO_DELBACKWARD;
                                                    }
                                                }
                                            }
                                            else
                                                if (letter == CTRL_U)
                                                {
                                                    /* CTRL-U  Delete from the character to the left of the cursor
                                                     * to the start of the buffer. In fixed field mode, jump cursor to the start
                                                     * of the buffer
                                                     */
                                                    D(bug("sghkey: CTRL-U\n"));
                                                    if (sgw->Modes & SGM_FIXEDFIELD)
                                                    {
                                                        D(bug("sghkey: SGM_FIXEDFIELD\n"));
                                                        sgw->BufferPos = 0;
                                                        sgw->EditOp = EO_MOVECURSOR;
                                                    }
                                                    else
                                                    {
                                                        if (sgw->BufferPos > 0)
                                                        {
                                                            int len;
                                                            len = sgw->NumChars - sgw->BufferPos;
                                                            memmove(&sgw->WorkBuffer[0],&sgw->WorkBuffer[sgw->BufferPos],len);

                                                            sgw->BufferPos = 0;
                                                            sgw->WorkBuffer[len] = 0;
                                                            sgw->NumChars = len;
                                                            sgw->EditOp = EO_DELBACKWARD;
                                                        }
                                                    }
                                                }
                                                else
                                                    if (letter == CTRL_X)
                                                    {
                                                        /* CTRL-X  clears the input buffer (like right amiga-x). In fixed field mode
                                                         * jump cursor to the start of the buffer
                                                         */
                                                        D(bug("sghkey: CTRL-X\n"));
                                                        if (sgw->Modes & SGM_FIXEDFIELD)
                                                        {
                                                            D(bug("sghkey: SGM_FIXEDFIELD\n"));
                                                            sgw->BufferPos = 0;
                                                            sgw->EditOp = EO_MOVECURSOR;
                                                        }
                                                        else
                                                        {
                                                            sgw->BufferPos = 0;
                                                            sgw->WorkBuffer[sgw->BufferPos] = 0;
                                                            sgw->NumChars = sgw->BufferPos;
                                                            sgw->EditOp = EO_CLEAR;
                                                        }
                                                    }
                                                    else
                                                        if (letter == CTRL_Z)
                                                        {
                                                            /* CTRL-Z  Jump cursor to the end of the buffer
                                                             */
                                                            D(bug("sghkey: CTRL-Z\n"));
                                                            sgw->BufferPos = sgw->NumChars;
                                                            sgw->EditOp = EO_MOVECURSOR;
                                                        }
        }
        else
            if (letter == BACKSPACE)
            {
                if (sgw->BufferPos != 0)
                {
                    UWORD first = sgw->BufferPos;
                    UWORD last  = sgw->NumChars - 1;
                    UWORD steps;
                    if (qual &  SHIFT)
                    {
                        steps = sgw->BufferPos;

                        sgw->BufferPos = 0;
                        sgw->NumChars -= steps;
                    }
                    else
                    {
                        sgw->NumChars --;
                        sgw->BufferPos --;
                        steps = 1;
                    }
                    MoveCharsLeft(sgw->WorkBuffer, first, last, steps);
                    sgw->EditOp = EO_DELBACKWARD;
                }
            }
            else if (letter == DELETE)
            {
                /* Check whether cursor is at the trailing 0 */
                if (sgw->BufferPos != sgw->NumChars)
                {
                    if (qual & SHIFT)
                    {
                        sgw->WorkBuffer[sgw->BufferPos] = 0;
                        sgw->NumChars = sgw->BufferPos;
                    }
                    else
                    {
                        MoveCharsLeft(sgw->WorkBuffer, sgw->BufferPos + 1, sgw->NumChars - 1, 1);
                        sgw->NumChars --;
                    }
                    sgw->EditOp = EO_DELFORWARD;
                }
            }
            else if (letter == RETURN)
            {
                D(bug("sghkey: ENTER\n"));
                sgw->EditOp  = EO_ENTER;
                sgw->Code    = 0;
                sgw->Actions = (SGA_USE|SGA_END);
            }
            else if (letter == TAB)
            {
                D(bug("sghkey: TAB\n"));
                sgw->EditOp  = EO_SPECIAL; /* FIXME: ??? is this correct ??? */
                sgw->Actions = (SGA_USE|SGA_NEXTACTIVE);
            }
            else
            {

                /* Validity check of letter */
                if (gad->Activation & GACT_LONGINT)
                {
                    if (letter == '-')
                    {
                        if ((sgw->BufferPos != 0) || ((sgw->NumChars > 0) && (sgw->WorkBuffer[0] == '-')))
                            sgw->EditOp = EO_BADFORMAT;

                    }
                    else if ((letter < 0x30) ||  (letter > 0x39))
                    {
                        sgw->EditOp = EO_BADFORMAT;
                    }
                    else if (sgw->WorkBuffer[sgw->BufferPos] == '-')
                    {
                        sgw->EditOp = EO_BADFORMAT;
                    }
                }
                else /* Integer gadget ? */
                {
                    /* Is key a printable character ? */
                    /* FIXME: Locale should be used here... */
                    if ((letter & 0x60) == 0)
                    {
                        sgw->EditOp = EO_BADFORMAT;
                    }
                } /* if (integer or string gadget) */

                if (sgw->EditOp != EO_BADFORMAT)
                {

                    if ((sgw->Modes & SGM_REPLACE) && (sgw->WorkBuffer[sgw->BufferPos]))
                    {
                        D(bug("sghkey: replacing char at pos %d\n", sgw->BufferPos));

                        sgw->WorkBuffer[sgw->BufferPos] = letter;
                        sgw->EditOp = EO_REPLACECHAR;

                        if (sgw->BufferPos < strinfo->MaxChars - 1)
                            sgw->BufferPos ++;

                    }
                    else
                    {
                        /* Insert mode. Check if there is space for one more character
                        ** NOTE: MaxChars inludes traing \0, so therefore the '- 1'
                        */
                        if (sgw->NumChars < (strinfo->MaxChars - 1))
                        {
                            register UWORD i;

                            D(bug("sghkey: inserting char at pos %d\n", sgw->BufferPos));
                            /* Move characters to the right of insertion point one step to the right */
                            for (i = sgw->NumChars; i > sgw->BufferPos; i --)
                            {
                                sgw->WorkBuffer[i] = sgw->WorkBuffer[i - 1];
                            }

                            /* Insert letter  */
                            sgw->WorkBuffer[i] = letter;
                            sgw->EditOp = EO_INSERTCHAR;
                            sgw->NumChars ++;
                            sgw->BufferPos ++;

                        }
                        else
                        {
                            sgw->EditOp = EO_NOOP;
                        } /* if (enough space for ione mor letter) */
                    } /* if (Replace or Insert mode) */
                } /* If (user pressed valid letter) */
            } /* Vanilla key but not backspace, delete, ... */
    } /* if (key or scancode) */

    /* Null-terminate the new string */
    sgw->WorkBuffer[sgw->NumChars] = 0;

    if (sgw->EditOp != EO_NOOP)
        sgw->Actions |= (SGA_USE|SGA_REDISPLAY);

    /* Integer gadget ? */
    if ((sgw->Actions & SGA_USE) && (gad->Activation & GACT_LONGINT))
    {
        sgw->LongInt = atol(sgw->WorkBuffer);
        D(bug("Updated string number to %d\n", sgw->LongInt));
    }

    ReturnInt ("DoSGHKey", ULONG, 1);
}

/*****************************************************************************************/

AROS_UFH3(ULONG, GlobalEditFunc,
          AROS_UFHA(struct Hook *,      hook,       A0),
          AROS_UFHA(struct SGWork *,        sgw,        A2),
          AROS_UFHA(ULONG *,            command,    A1)
         )
{
    AROS_USERFUNC_INIT

    ULONG retcode = 0;

    switch (*command)
    {
        case SGH_CLICK:
            retcode = DoSGHClick(sgw, (struct IntuitionBase *)hook->h_Data);
            break;

        case SGH_KEY:
            retcode = DoSGHKey  (sgw, (struct IntuitionBase *)hook->h_Data);
            break;
    }

    return retcode;

    AROS_USERFUNC_EXIT
}

/*****************************************************************************************/

VOID RefreshStrGadget(struct Gadget *gad,
                      struct Window     *win,
                      struct Requester  *req,
                      struct IntuitionBase  *IntuitionBase)
{
    struct GadgetInfo    gi;
    struct RastPort     *rp;

    EnterFunc(bug("RefreshStrGadget(gad=%p, win=%s)\n", gad, win->Title));

    SetupGInfo(&gi, win, req, gad, IntuitionBase);

    if ((rp = ObtainGIRPort(&gi)))
    {
        if (gad->GadgetRender)
        {
            if (gad->Flags & GFLG_GADGIMAGE)
            {
                DrawImage(rp,
                          (struct Image *)gad->GadgetRender,
                          gad->LeftEdge,
                          gad->TopEdge);
            }
            else
            {
                DrawBorder(rp,
                           (struct Border *)gad->GadgetRender,
                           gad->LeftEdge,
                           gad->TopEdge);
            }
            /* The gadget can have a linked list of IntuiTexts attached. */
            if(gad->GadgetText)
            {
                struct IBox container;
                GetGadgetIBox(gad, &gi, &container);
                int_PrintIText(rp, gad->GadgetText,
                                   container.Left,
                                   container.Top, FALSE, IntuitionBase);
            }
        }
        ReleaseGIRPort(rp);
    }
    UpdateStrGadget(gad, win, req, IntuitionBase);

    ReturnVoid("RefreshStrGadget");
}

/*****************************************************************************************/

VOID UpdateStringInfo(struct Gadget *gad)
{
    /* Updates the stringinfo in case user has set some fields */

    struct StringInfo *strinfo = (struct StringInfo *)gad->SpecialInfo;

    EnterFunc(bug("UpdateStringInfo(gad=%p)\n", gad));

#if 0
    if (gad->Activation & GACT_LONGINT)
    {
        if ((strinfo->NumChars != 0) || (strinfo->LongInt != 0))
        {
            /* NOTE: The max number of chars written INCLUDES trailing \0 */
            snprintf(strinfo->Buffer, strinfo->MaxChars, "%d", strinfo->LongInt);
        }
    }
#endif

    /* All of the text, also beyond MaxChars - 1 characters, as AmigaOS:
       without a work buffer that fits it, the editing code works on the
       buffer itself, and rendering writes its NUL at NumChars. But no more
       than the 0x7FFE characters of the largest MaxChars, so that
       NumChars + 1 is still a WORD. */
    strinfo->NumChars = strnlen(strinfo->Buffer, 0x7FFE);

    /* The editing code needs the cursor within 0..NumChars */
    if (strinfo->BufferPos > strinfo->NumChars)
    {
        strinfo->BufferPos = strinfo->NumChars;
    }
    else if (strinfo->BufferPos < 0)
    {
        strinfo->BufferPos = 0;
    }

    /* And DispPos, the first character shown, from which a click measures
       the text: it is still that of the last render when the program has
       shortened the text without a refresh, or an edit hook has changed it
       without SGA_REDISPLAY */
    if (strinfo->DispPos > strinfo->NumChars)
    {
        strinfo->DispPos = strinfo->NumChars;
    }
    else if (strinfo->DispPos < 0)
    {
        strinfo->DispPos = 0;
    }

    D(bug("%s gadget contains buffer %s of length %d\n",
          (gad->Activation & GACT_LONGINT) ? "Integer" : "String",
          strinfo->Buffer, strinfo->NumChars));

    ReturnVoid("UpdateStringInfo");
}

/*****************************************************************************************/

VOID UpdateStrGadget(struct Gadget  *gad,
                     struct Window      *win,
                     struct Requester   *req,
                     struct IntuitionBase   *IntuitionBase)
{
    struct GfxBase      *GfxBase = GetPrivIBase(IntuitionBase)->GfxBase;
    struct GadgetInfo    gi;
    struct BBox          bbox;
    struct StringInfo   *strinfo = (struct StringInfo *)gad->SpecialInfo;
    WORD                 text_left;
    UWORD                text_top;
    struct RastPort     *rp;
    STRPTR               dispstr;
    UWORD                pens[NUMPENS];

    EnterFunc(bug("UpdateStrGadget(current text=%s)\n", strinfo->Buffer));

    SetupGInfo(&gi, win, req, gad, IntuitionBase);

    rp = ObtainGIRPort(&gi);
    if (!rp) return;

    GetPensAndFont(gad, pens, win, rp, IntuitionBase);

    CalcBBox(win, req, gad, &bbox);

    /* Update the stringinfo struct in case of user change */
    UpdateStringInfo(gad);

    /* Update the DispPos and DispCount fields so that the gadget renders properly */
    UpdateDisp(gad, &bbox, rp, IntuitionBase);


    dispstr = strinfo->Buffer + strinfo->DispPos;

    /* Clear the background */
    SetAPen(rp, pens[STRBACKPEN]);
    SetDrMd(rp, JAM1);

    RectFill(rp,
             bbox.Left,
             bbox.Top,
             bbox.Left + bbox.Width  - 1,
             bbox.Top  + bbox.Height - 1);

    text_left = GetTextLeft(gad, &bbox, rp, IntuitionBase);


    /* Write the text into the gadget */
    SetABPenDrMd(rp, pens[STRTEXTPEN], pens[STRBACKPEN], JAM2);

    text_top =   bbox.Top
                 + ((bbox.Height - rp->Font->tf_YSize) >> 1)
                 + rp->Font->tf_Baseline;

    Move(rp, text_left, text_top);

    D(bug("usg: Writing text %s of length %d at (%d, %d)\n",
          dispstr, strinfo->DispCount, text_left, text_top));

    Text(rp, dispstr, strinfo->DispCount);

    if (gad->Flags & GFLG_SELECTED)
    {
        UWORD cursoroffset = strinfo->BufferPos - strinfo->DispPos;
        /* Render cursor */
//      SetAfPt(rp,NULL,0);
        D(bug("usg: Number of characters: %d\n", strinfo->NumChars));


        text_left += TextLength(rp, dispstr, cursoroffset);

    #if 1
        if (strinfo->BufferPos < strinfo->NumChars)
        {
            SetABPenDrMd(rp, pens[STRTEXTPEN], pens[CURSORPEN], JAM2);
            Move(rp, text_left, text_top);
            Text(rp,
                 dispstr + cursoroffset,
                 1 );
        }
        else
        {
            struct TextExtent   te;
            
            SetABPenDrMd(rp, pens[STRTEXTPEN], pens[CURSORPEN], JAM2|INVERSVID);
            TextExtent(rp, cursorkey, CURSORKEYLEN,&te);
            
            RectFill(rp,
                text_left+te.te_Extent.MinX,
                text_top+te.te_Extent.MinY,
                text_left+te.te_Extent.MaxX,
                text_top+te.te_Extent.MaxY);
        }
    #else
        Move(rp, text_left, text_top);
        Text(rp,
             ((strinfo->BufferPos < strinfo->NumChars)
              ? dispstr + cursoroffset
              : cursorkey),
             1 );
    #endif
    
    } /* if (gadget selected => render cursor) */

    if (gad->Flags & GFLG_DISABLED )
    {
        struct DrawInfo *dri = GetScreenDrawInfo(win->WScreen);

        RenderDisabledPattern(rp, dri, bbox.Left,
                              bbox.Top,
                              bbox.Left + bbox.Width - 1,
                              bbox.Top + bbox.Height - 1,
                              IntuitionBase );

        if (dri) FreeScreenDrawInfo(win->WScreen, dri);
    }

    ReleaseGIRPort(rp);

    ReturnVoid("UpdateStrGadget");
}
