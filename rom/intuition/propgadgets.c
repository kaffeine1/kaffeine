/*
    Copyright (C) 1995-2025, The AROS Development Team. All rights reserved.
    Copyright (C) 2001-2003, The MorphOS Development Team. All Rights Reserved.
*/

#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>
#include <proto/layers.h>
#include <proto/utility.h>
#include <graphics/gfxmacros.h>
#include <intuition/cghooks.h>
#include <intuition/intuition.h>
#include <intuition/imageclass.h>
#include <intuition/windecorclass.h>
#include <cybergraphx/cybergraphics.h>
#include <graphics/rpattr.h>
#include "intuition_intern.h"
#include "propgadgets.h"
#ifdef SKINS
#include "intuition_customize.h"
#include "intuition_customizesupport.h"
#include "morphos/proprender.h"
#endif
#include "gadgets.h"

#undef DEBUG
#define DEBUG 0
#   include <aros/debug.h>

BOOL isonborder(struct Gadget *gadget,struct Window *window);

static void RenderPropBackground(struct Gadget *gad, struct Window *win, struct DrawInfo *dri,
                                 struct Rectangle *rect, struct Rectangle *proprect,
                                 struct Rectangle *knobrect, struct PropInfo *pi,
                                 struct RastPort *rp, BOOL onborder,
                                 struct IntuitionBase *IntuitionBase)
{
    struct GfxBase *GfxBase = GetPrivIBase(IntuitionBase)->GfxBase;
    struct Library *UtilityBase = GetPrivIBase(IntuitionBase)->UtilityBase;

    if (onborder)
    {
        struct wdpDrawBorderPropBack msg;

        msg.MethodID        = WDM_DRAW_BORDERPROPBACK;

        msg.wdp_Window      = win;
    msg.wdp_TrueColor   = (((struct IntScreen *)win->WScreen)->DInfo.dri_Flags & DRIF_DIRECTCOLOR) ? TRUE : FALSE;
    msg.wdp_UserBuffer  = ((struct IntWindow *)win)->DecorUserBuffer;

        msg.wdp_RPort       = rp;
        msg.wdp_Gadget      = gad;
        msg.wdp_RenderRect  = rect;
        msg.wdp_PropRect    = proprect;
        msg.wdp_KnobRect    = knobrect;
        msg.wdp_Flags       = 0;
        msg.wdp_Dri         = dri;

        DoMethodA(((struct IntScreen *)(win->WScreen))->WinDecorObj, (Msg)&msg);
    }
    else
    {
        static CONST UWORD pattern[] = {0x5555,0xAAAA};
        struct Hook *dhook = NULL;

        if ((gad->Flags & GFLG_EXTENDED) != 0)
        {
            if ((((struct ExtGadget *) gad)->MoreFlags & GMORE_BOOPSIGADGET) != 0)
            {
                GetAttr(PGA_DisplayHook, (Object *) gad, (IPTR *)&dhook);
            }
        }
        if (dhook) {

            struct wdpDrawBorderPropBack msg;

            msg.MethodID            = WDM_DRAW_BORDERPROPBACK;

            msg.wdp_Window      = win;
            msg.wdp_TrueColor   = (((struct IntScreen *)win->WScreen)->DInfo.dri_Flags & DRIF_DIRECTCOLOR) ? TRUE : FALSE;
            msg.wdp_RPort           = rp;
            msg.wdp_Gadget      = gad;
            msg.wdp_RenderRect  = rect;
            msg.wdp_PropRect    = proprect;
            msg.wdp_KnobRect    = knobrect;
            msg.wdp_Flags           = 0;
            msg.wdp_Dri         = dri;

            CallHookPkt(dhook, (Object *) gad, (Msg)&msg);
        }
        else
        {
            SetDrMd(rp, JAM2);

            if (pi->Flags & PROPNEWLOOK)
            {
                SetAfPt(rp, pattern, 1);
                SetAPen(rp, dri->dri_Pens[SHADOWPEN]);
                SetBPen(rp, dri->dri_Pens[BACKGROUNDPEN]);

                RectFill(rp, rect->MinX, rect->MinY, rect->MaxX, rect->MaxY);
                SetAfPt(rp, NULL, 0);
            }
            else
            {
                SetAPen(rp, dri->dri_Pens[BACKGROUNDPEN]);
                RectFill(rp, rect->MinX, rect->MinY, rect->MaxX, rect->MaxY);
            }
        }
    }
}


/* The arithmetic of AmigaOS 3.2.3, which programs read back from the
   PropInfo and the knob Image. An AUTOKNOB on an axis that moves takes its
   body's share of the interior, rounded up, at least min pixels and at most
   the interior. */
static WORD knob_share(WORD size, UWORD body, WORD min)
{
    LONG share = ((LONG)size * body + MAXBODY - 1) / MAXBODY;

    if (share < min)
        share = min;
    if (share > size)
        share = size;
    return (WORD)share;
}

/* The place of the knob within the interior for pot, in room pixels of
   travel: (pot + 1) times room divided by 65536, rounded down, a room
   below 0 included */
static WORD knob_offset(UWORD pot, LONG room)
{
    QUAD q = ((QUAD)pot + 1) * room;

    return (WORD)(q >= 0 ? q >> 16 : -((-q + 0xFFFF) >> 16));
}

/* The pot of a knob place pixels from where it starts, of room pixels to
   travel; MAXPOT where it cannot travel */
static UWORD knob_pot(LONG place, LONG room)
{
    if (room <= 0 || place >= room)
        return MAXPOT;
    if (place <= 0)
        return 0;
    return (UWORD)((ULONG)place * MAXPOT / (ULONG)room);
}

/* A rectangle cut to the box of its gadget */
static void clip_box(struct BBox *r, const struct BBox *box)
{
    LONG right = r->Left + r->Width, bottom = r->Top + r->Height;

    if (r->Left < box->Left)
        r->Left = box->Left;
    if (r->Top < box->Top)
        r->Top = box->Top;
    if (right > box->Left + box->Width)
        right = box->Left + box->Width;
    if (bottom > box->Top + box->Height)
        bottom = box->Top + box->Height;
    r->Width = right - r->Left;
    r->Height = bottom - r->Top;
}

/* The step of a page for a body, 0 for 0 */
static UWORD pot_res(UWORD body)
{
    if (body > MAXBODY / 2)
        return MAXPOT;
    return (UWORD)(((ULONG)body << 16) / (MAXBODY - body));
}

VOID HandlePropSelectDown(struct Gadget *gadget, struct Window *w, struct Requester *req,
                          UWORD mouse_x, UWORD mouse_y, struct IntuitionBase *IntuitionBase)
{
    struct BBox          knob;
    struct PropInfo     *pi;
    UWORD                dx, dy, flags;

    pi = (struct PropInfo *)gadget->SpecialInfo;

    if (!pi)
        return;

    CalcBBox (w, req, gadget, &knob);

    /* This func gets mouse coords relative to gadget box */

    mouse_x += knob.Left;
    mouse_y += knob.Top;

    if (!CalcKnobSize (gadget, &knob))
        return;

    GetPrivIBase(IntuitionBase)->prop_clickoffset_x = mouse_x - knob.Left;
    GetPrivIBase(IntuitionBase)->prop_clickoffset_y = mouse_y - knob.Top;

    dx = pi->HorizPot;
    dy = pi->VertPot;

    if (pi->Flags & FREEHORIZ)
    {
        if (mouse_x < knob.Left)
        {
            if (dx > pi->HPotRes)
                dx -= pi->HPotRes;
            else
                dx = 0;
        }
        else if (mouse_x >= knob.Left + knob.Width)
        {
            if (dx < MAXPOT - pi->HPotRes)
                dx += pi->HPotRes;
            else
                dx = MAXPOT;
        }
    }

    if (pi->Flags & FREEVERT)
    {
        if (mouse_y < knob.Top)
        {
            if (dy > pi->VPotRes)
                dy -= pi->VPotRes;
            else
                dy = 0;
        }
        else if (mouse_y >= knob.Top + knob.Height)
        {
            if (dy < MAXPOT - pi->VPotRes)
                dy += pi->VPotRes;
            else
                dy = MAXPOT;
        }
    }

    flags = pi->Flags;

    /* jDc: when clicked on frame, prop gadget also gets activated! */
    if (!(flags & PROPBORDERLESS))
    {
        /* across the axis that does not move, as far as the borders reach,
           which the frame lies in; a pixel aside for a knob that moves both
           ways */
        if ((pi->Flags & FREEVERT) && (pi->Flags & FREEHORIZ))
        {
            knob.Left --;
            knob.Width += 2;
        }
        else if (pi->Flags & FREEVERT)
        {
            knob.Left -= pi->LeftBorder;
            knob.Width += 2 * pi->LeftBorder;
        } else {
            knob.Top -= pi->TopBorder;
            knob.Height += 2 * pi->TopBorder;
        }
    }

    if (mouse_x >= knob.Left &&
        mouse_y >= knob.Top &&
        mouse_x < knob.Left + knob.Width &&
        mouse_y < knob.Top + knob.Height)
    {
        flags |= KNOBHIT;
    }
    else
    {
        flags &= ~KNOBHIT;
    }

    gadget->Flags |= GFLG_SELECTED;

    D(bug("New HPot: %d, new VPot: %d\n", dx, dy));

    NewModifyProp(gadget, w, req, flags, dx, dy, pi->HorizBody, pi->VertBody, 1);

    return;
}

VOID HandlePropSelectUp(struct Gadget *gadget, struct Window *w,
                        struct Requester *req, struct IntuitionBase *IntuitionBase)
{
    struct PropInfo * pi;

    pi = (struct PropInfo *)gadget->SpecialInfo;

    /* KNOBHIT stays, as on AmigaOS 3.2.3, where a program reads it after
       the GADGETUP to tell a drag of the knob from a click beside it; the
       knob looks pressed only while the gadget is selected. */
    gadget->Flags &= ~GFLG_SELECTED;

    if (pi) RefreshPropGadget (gadget, w, req, IntuitionBase);

/*    if (pi)
        NewModifyProp (gadget
                       , w
                       , req
                       , pi->Flags &= ~KNOBHIT
                       , pi->HorizPot
                       , pi->VertPot
                       , pi->HorizBody
                       , pi->VertBody
                       , 1
                      ); */

    return;
}

VOID HandlePropMouseMove(struct Gadget *gadget, struct Window *w,struct Requester *req,
                         LONG dx, LONG dy, struct IntuitionBase *IntuitionBase)
{
    struct BBox      knob;
    struct PropInfo *pi;
    LONG             hroom, vroom;
    UWORD            hpot, vpot;

    pi = (struct PropInfo *)gadget->SpecialInfo;

    /* Has propinfo and the mouse was over the knob */
    if (pi && (pi->Flags & KNOBHIT))
    {
        CalcBBox (w, req, gadget, &knob);

        if (!CalcKnobSize (gadget, &knob))
            return;

        /* Move the knob the same amount, ie.
        knob.Left += dx; knob.Top += dy;

        knob.Left = knob.Left
        + (pi->CWidth - knob.Width)
        * pi->HorizPot / MAXPOT;

        ie. dx = (pi->CWidth - knob.Width)
        * pi->HorizPot / MAXPOT;

        or

        pi->HorizPot = (dx * MAXPOT) /
        (pi->CWidth - knob.Width);
        */

        /* stegerg: dx and dy are not delta values
                anymore but relative to gadget
                box */

        /* the place of the knob within the interior, and the pot of each
           axis the share of the room that it has there, as on AmigaOS 3.2.3:
           on an axis that does not move the knob stays at the start, and an
           axis without room, as that of an AUTOKNOB, gives MAXPOT */
        dx = dx - GetPrivIBase(IntuitionBase)->prop_clickoffset_x - pi->LeftBorder;
        dy = dy - GetPrivIBase(IntuitionBase)->prop_clickoffset_y - pi->TopBorder;
        hroom = pi->CWidth - 2 * pi->LeftBorder - knob.Width;
        vroom = pi->CHeight - 2 * pi->TopBorder - knob.Height;
        hpot = knob_pot((pi->Flags & FREEHORIZ) ? dx : 0, hroom);
        vpot = knob_pot((pi->Flags & FREEVERT) ? dy : 0, vroom);

        if (hpot != pi->HorizPot || vpot != pi->VertPot)
        {
            NewModifyProp (gadget, w, req, pi->Flags, hpot, vpot, pi->HorizBody, pi->VertBody, 1);
        }

    } /* Has PropInfo and Mouse is over knob */

    return;
}

int CalcKnobSize (struct Gadget * propGadget, struct BBox * knobbox)
{
    struct PropInfo *pi;
    struct Image    *im;
    WORD             bx, by, left, top, width, height, kw, kh;

    pi = (struct PropInfo *)propGadget->SpecialInfo;
    im = (struct Image *)propGadget->GadgetRender;

    /* As AmigaOS 3.2.3 lays the gadget out: the container is its box, and
       the knob moves within it less the borders, none for PROPBORDERLESS, a
       pixel for PROPNEWLOOK and 4 by 2 for the old look */
    if (pi->Flags & PROPBORDERLESS)
        bx = by = 0;
    else if (pi->Flags & PROPNEWLOOK)
        bx = by = 1;
    else
    {
        bx = 4;
        by = 2;
    }
    pi->LeftBorder = bx;
    pi->TopBorder  = by;
    pi->CWidth     = knobbox->Width;
    pi->CHeight    = knobbox->Height;
    left   = knobbox->Left + bx;
    top    = knobbox->Top + by;
    width  = knobbox->Width - 2 * bx;
    height = knobbox->Height - 2 * by;

    /* A knob image of the program's own keeps its size; an AUTOKNOB fills
       the interior on an axis that does not move, and takes its body's
       share of it on one that does; a pixel at least */
    if (!(pi->Flags & AUTOKNOB) && im)
    {
        kw = im->Width;
        kh = im->Height;
    }
    else
    {
        kw = (pi->Flags & FREEHORIZ) ? knob_share(width, pi->HorizBody, KNOBHMIN) : width;
        kh = (pi->Flags & FREEVERT) ? knob_share(height, pi->VertBody, KNOBVMIN) : height;
        if (kw < 1)
            kw = 1;
        if (kh < 1)
            kh = 1;
    }
    knobbox->Left   = left;
    knobbox->Top    = top;
    knobbox->Width  = kw;
    knobbox->Height = kh;
    if (pi->Flags & FREEHORIZ)
        knobbox->Left += knob_offset(pi->HorizPot, (LONG)width - kw);
    if (pi->Flags & FREEVERT)
        knobbox->Top += knob_offset(pi->VertPot, (LONG)height - kh);

    pi->HPotRes = pot_res(pi->HorizBody);
    pi->VPotRes = pot_res(pi->VertBody);

    /* AmigaOS 3.2.3 keeps the knob of a classic gadget in its Image, where
       programs read it: the place within the borders, and the size of an
       AUTOKNOB */
    if (im && !((propGadget->Flags & GFLG_EXTENDED) &&
                (((struct ExtGadget *)propGadget)->MoreFlags & GMORE_BOOPSIGADGET)))
    {
        im->LeftEdge = knobbox->Left - left;
        im->TopEdge  = knobbox->Top - top;
        if (pi->Flags & AUTOKNOB)
        {
            im->Width  = kw;
            im->Height = kh;
        }
    }

    return TRUE;
} /* CalcKnobSize */


void RefreshPropGadget (struct Gadget * gadget, struct Window * window,
                        struct Requester * req, struct IntuitionBase * IntuitionBase)
{
    struct GfxBase      *GfxBase = GetPrivIBase(IntuitionBase)->GfxBase;
    struct PropInfo     *pi;
    struct DrawInfo     *dri;
    struct GadgetInfo    gi;
    struct RastPort     *rp = 0;
    struct BBox          bbox, kbox;
    BOOL                 onborder;

    D(bug("RefreshPropGadget(gad=%p, win=%s, req=%p)\n", gadget, window->Title, req));

    onborder = (IS_BORDER_GADGET(gadget) || isonborder(gadget,window));

    if ((dri = GetScreenDrawInfo(window->WScreen)))
    {

        SetupGInfo(&gi, window, req, gadget, IntuitionBase);

        if ((rp = ObtainGIRPort(&gi)))
        {
            for (;;)
            {
                BOOL umn;

                CalcBBox (window, req, gadget, &bbox);
                kbox = bbox;

                if (bbox.Width <= 0 || bbox.Height <= 0)
                    break;

                pi = (struct PropInfo *)gadget->SpecialInfo;
                if (!pi)
                    break;

                umn = CalcKnobSize (gadget, &kbox);

                SetDrMd (rp, JAM2);
                if (!(pi->Flags & PROPBORDERLESS))
                {
                    SetAPen(rp,dri->dri_Pens[SHADOWPEN]);
                    drawrect(rp,bbox.Left,
                             bbox.Top,
                             bbox.Left + bbox.Width - 1,
                             bbox.Top + bbox.Height - 1,
                             IntuitionBase);
                             
                    bbox.Left ++; bbox.Top ++;
                    bbox.Width -= 2; bbox.Height -= 2;
                }

                if (umn)
                {
                    RefreshPropGadgetKnob (gadget, &bbox, &kbox, window, req, IntuitionBase);
                }
                else
                {
                    struct Rectangle tmprect;
                    
                    tmprect.MinX = bbox.Left;
                    tmprect.MaxX = bbox.Left + bbox.Width - 1;
                    tmprect.MinY = bbox.Top;
                    tmprect.MaxY = bbox.Top + bbox.Height - 1;

                    RenderPropBackground(gadget, window, dri, &tmprect, &tmprect, NULL,
                                         pi, rp, onborder, IntuitionBase);
                } // if (CalcKnob
                break;
            }

        } /* if ((rp = ObtainGIRPort(&gi))) */

        if (rp && gadget->Flags & GFLG_DISABLED)
        {
            CalcBBox (window, req, gadget, &bbox);

            RenderDisabledPattern(rp, (struct DrawInfo *)dri, bbox.Left,
                                  bbox.Top,
                                  bbox.Left + bbox.Width - 1,
                                  bbox.Top + bbox.Height - 1,
                                  IntuitionBase);
        }

        if (rp) ReleaseGIRPort(rp);

        FreeScreenDrawInfo(window->WScreen, (struct DrawInfo *)dri);

    } /* if ((dri = GetScreenDrawInfo(window->WScreen))) */

    ReturnVoid("RefreshPropGadget");
} /* RefreshPropGadget */


void RefreshPropGadgetKnob (struct Gadget * gadget, struct BBox * clear,
                            struct BBox * knob, struct Window * window, struct Requester * req,
                            struct IntuitionBase * IntuitionBase)
{
    struct GfxBase      *GfxBase = GetPrivIBase(IntuitionBase)->GfxBase;
    struct Library      *UtilityBase = GetPrivIBase(IntuitionBase)->UtilityBase;
    struct DrawInfo     *dri;
    struct RastPort     *rp;
    struct PropInfo     *pi;
    struct GadgetInfo    gi;
    struct BBox          box, kclip, cclip;
    UWORD                flags;
    BOOL                 onborder, empty;

    D(bug("RefresPropGadgetKnob(flags=%d, clear=%p, knob = %p, win=%s)\n",
          flags, clear, knob, window->Title));

    pi = (struct PropInfo *)gadget->SpecialInfo;
    flags = pi->Flags;
    onborder = (IS_BORDER_GADGET(gadget) || isonborder(gadget,window));

    /* A gadget smaller than its borders may have its knob partly or wholly
       outside: only what lies within its frame is drawn or cleared, and an
       image of the program's own only when all of it lies there; a knob
       that is not drawn hides nothing of the background that its old place
       leaves. */
    CalcBBox (window, req, gadget, &box);
    if (!(flags & PROPBORDERLESS))
    {
        box.Left++;
        box.Top++;
        box.Width -= 2;
        box.Height -= 2;
    }
    kclip = *knob;
    clip_box(&kclip, &box);
    empty = kclip.Width < 1 || kclip.Height < 1 ||
            (!(flags & AUTOKNOB) &&
             (kclip.Left != knob->Left || kclip.Top != knob->Top ||
              kclip.Width != knob->Width || kclip.Height != knob->Height));
    if (empty)
        kclip.Width = kclip.Height = 0;
    knob = &kclip;
    if (clear)
    {
        cclip = *clear;
        clip_box(&cclip, &box);
        clear = &cclip;
    }

    /* a window decorator draws the background with the knob: none for no
       knob, as before */
    if (empty && (!clear || onborder)) return;

    if ((dri = GetScreenDrawInfo(window->WScreen)))
    {
        SetupGInfo(&gi, window, req, gadget, IntuitionBase);

        if ((rp = ObtainGIRPort(&gi)))
        {
            struct BBox      bbox;
            struct Rectangle brect;
            struct Rectangle krect;
            
            CalcBBox (window, req, gadget, &bbox);
            
            if ((bbox.Width < 1) || (bbox.Height < 1))
            {
                ReleaseGIRPort(rp);
                FreeScreenDrawInfo(window->WScreen, (struct DrawInfo *)dri);

                return;
            }
            
            brect.MinX = bbox.Left;
            brect.MinY = bbox.Top;
            brect.MaxX = bbox.Left + bbox.Width - 1;
            brect.MaxY = bbox.Top + bbox.Height - 1;
            
            krect.MinX = knob->Left;
            krect.MinY = knob->Top;
            krect.MaxX = knob->Left + knob->Width - 1;
            krect.MaxY = knob->Top + knob->Height - 1;
            
            SetDrMd (rp, JAM2);

            if (clear)
            {
                struct Rectangle a, b, clearrects[4];
                WORD             i, nrects;

            #if (!(PROP_RENDER_OPTIMIZATION))
                if (!(flags & PROPBORDERLESS))
                {
                    clear->Left ++; clear->Top ++;
                    clear->Width -= 2; clear->Height -= 2;
                }
            #endif

            #if 0
                D(bug("RefresPropGadgetKnob: clear Left %d Top %d Width %d Height %d\n",
                      clear->Left,
                      clear->Top,
                      clear->Width,
                      clear->Height));

                D(bug("RefresPropGadgetKnob: knob Left %d Top %d Width %d Height %d\n",
                      knob->Left,
                      knob->Top,
                      knob->Width,
                      knob->Height));
            #endif
            
                a.MinX = clear->Left;
                a.MinY = clear->Top;
                a.MaxX = clear->Left + clear->Width - 1;
                a.MaxY = clear->Top + clear->Height - 1;

                b.MinX = knob->Left;
                b.MinY = knob->Top;
                b.MaxX = knob->Left + knob->Width - 1;
                b.MaxY = knob->Top + knob->Height - 1;
                
            #if 0
                D(bug("RefresPropGadgetKnob: a MinX %d MinY %d MaxX %d MaxY %d\n",
                      a.MinX,
                      a.MinY,
                      a.MaxX,
                      a.MaxY));

                D(bug("RefresPropGadgetKnob: a MinX %d MinY %d MaxX %d MaxY %d\n",
                      b.MinX,
                      b.MinY,
                      b.MaxX,
                      b.MaxY));
            #endif
            
                if (empty)
                {
                    clearrects[0] = a;
                    nrects = 1;
                }
                else
                    nrects = SubtractRectFromRect(&a, &b, clearrects);

                D(bug("RefresPropGadgetKnob: nrects %d\n",
                      nrects));
            #if 0
                D(bug("RefresPropGadgetKnob: clearrects[0] MinX %d MinY %d MaxX %d MaxY %d\n",
                      clearrects[0].MinX,
                      clearrects[0].MinY,
                      clearrects[0].MaxX,
                      clearrects[0].MaxY));

                D(bug("RefresPropGadgetKnob: clearrects[1] MinX %d MinY %d MaxX %d MaxY %d\n",
                      clearrects[1].MinX,
                      clearrects[1].MinY,
                      clearrects[1].MaxX,
                      clearrects[1].MaxY));

                D(bug("RefresPropGadgetKnob: clearrects[2] MinX %d MinY %d MaxX %d MaxY %d\n",
                      clearrects[2].MinX,
                      clearrects[2].MinY,
                      clearrects[2].MaxX,
                      clearrects[2].MaxY));

                D(bug("RefresPropGadgetKnob: clearrects[3] MinX %d MinY %d MaxX %d MaxY %d\n",
                      clearrects[3].MinX,
                      clearrects[3].MinY,
                      clearrects[3].MaxX,
                      clearrects[3].MaxY));
            #endif

                /*kprintf("\n=== oldknob = %d,%d-%d,%d   newknob = %d,%d-%d,%d\n",
                    a.MinX,
                    a.MinY,
                    a.MaxX,
                    a.MaxY,
                    b.MinX,
                    b.MinY,
                    b.MaxX,
                    b.MaxY);*/

                for(i = 0; i < nrects; i++)
                {
                    RenderPropBackground(gadget, window, dri, &clearrects[i], &brect, &krect,
                                         pi, rp, onborder, IntuitionBase);
                }

            } /* if (clear) */

            if (empty)
            {
                /* no knob to draw */
            }
            else if (flags & AUTOKNOB)
            {
                /* KNOBHIT stays after the drag: pressed only while selected */
                int hit = ((flags & KNOBHIT) != 0) && (gadget->Flags & GFLG_SELECTED);

                D(bug("RefresPropGadgetKnob: draw knob Left %d Top %d Width %d Height %d\n",
                      knob->Left,
                      knob->Top,
                      knob->Width,
                      knob->Height));

                if (onborder)
                {
                    struct wdpDrawBorderPropKnob msg;
                    struct Rectangle             knobrect;
                    knobrect.MinX = knob->Left;
                    knobrect.MinY = knob->Top;
                    knobrect.MaxX = knob->Left + knob->Width - 1;
                    knobrect.MaxY = knob->Top + knob->Height - 1;

                    msg.MethodID        = WDM_DRAW_BORDERPROPKNOB;
                    msg.wdp_TrueColor   = (((struct IntScreen *)window->WScreen)->DInfo.dri_Flags & DRIF_DIRECTCOLOR) ? TRUE : FALSE;
                    msg.wdp_UserBuffer  = ((struct IntWindow *)window)->DecorUserBuffer;
                    msg.wdp_Window      = window;
                    msg.wdp_RPort       = rp;
                    msg.wdp_Gadget      = gadget;
                    msg.wdp_RenderRect  = &knobrect;
                    msg.wdp_PropRect    = &brect;
                    msg.wdp_Flags       = hit ? WDF_DBPK_HIT : 0;
                    msg.wdp_Dri         = dri;

                    DoMethodA(((struct IntScreen *)(window->WScreen))->WinDecorObj, (Msg)&msg);

                } /* gadget inside window border */
                else
                {
                    struct Hook *dhook = NULL;

                    if ((gadget->Flags & GFLG_EXTENDED) != 0)
                    {
                        if ((((struct ExtGadget *) gadget)->MoreFlags & GMORE_BOOPSIGADGET) != 0)
                        {
                            GetAttr(PGA_DisplayHook, (Object *) gadget, (IPTR *)&dhook);
                        }
                    }
                    if (dhook) {
                        struct wdpDrawBorderPropKnob msg;
                        struct Rectangle                 knobrect;
                        knobrect.MinX = knob->Left;
                        knobrect.MinY = knob->Top;
                        knobrect.MaxX = knob->Left + knob->Width - 1;
                        knobrect.MaxY = knob->Top + knob->Height - 1;
                        msg.MethodID            = WDM_DRAW_BORDERPROPKNOB;
                        msg.wdp_TrueColor   = (((struct IntScreen *)window->WScreen)->DInfo.dri_Flags & DRIF_DIRECTCOLOR) ? TRUE : FALSE;
                        msg.wdp_Window          = window;
                        msg.wdp_RPort           = rp;
                        msg.wdp_Gadget          = gadget;
                        msg.wdp_RenderRect  = &knobrect;
                        msg.wdp_PropRect        = &brect;
                        msg.wdp_Flags           = hit ? WDF_DBPK_HIT : 0;
                        msg.wdp_Dri         = dri;

                        CallHookPkt(dhook, (Object *) gadget, (Msg)&msg);
                    }
                    else
                    {

                        BOOL stdlook = TRUE;

                        #ifdef SKINS
                        stdlook = RenderPropKnob(window,dri,rp,pi,knob,hit,IntuitionBase);
                        #endif

                        if (stdlook)
                        {
                            if (flags & PROPBORDERLESS)
                            {
                                SetAPen(rp,dri->dri_Pens[SHADOWPEN]);

                                /* paint black right and bottom edges */

                                RectFill(rp,knob->Left + knob->Width - 1,
                                         knob->Top,
                                         knob->Left + knob->Width - 1,
                                         knob->Top + knob->Height - 1);

                                RectFill(rp,knob->Left,
                                         knob->Top + knob->Height - 1,
                                         knob->Left + knob->Width - 2,
                                         knob->Top + knob->Height - 1);

                                knob->Width--;
                                knob->Height--;

                            } /* propborderless */
                            else
                            {
                                SetAPen(rp,dri->dri_Pens[SHADOWPEN]);

                                if (flags & FREEHORIZ)
                                {
                                    /* black line at the left and at the right */

                                    RectFill(rp,knob->Left,
                                             knob->Top,
                                             knob->Left,
                                             knob->Top + knob->Height - 1);

                                    RectFill(rp,knob->Left + knob->Width - 1,
                                             knob->Top,
                                             knob->Left + knob->Width - 1,
                                             knob->Top + knob->Height - 1);

                                    knob->Left++,
                                    knob->Width -= 2;
                                }
                                if (flags & FREEVERT)
                                {
                                    /* black line at the top and at the bottom */

                                    RectFill(rp,knob->Left,
                                             knob->Top,
                                             knob->Left + knob->Width - 1,
                                             knob->Top);

                                    RectFill(rp,knob->Left,
                                             knob->Top + knob->Height - 1,
                                             knob->Left + knob->Width - 1,
                                             knob->Top + knob->Height - 1);

                                    knob->Top++;
                                    knob->Height -= 2;
                                }

                            } /* not propborderless */


                            SetAPen(rp, dri->dri_Pens[SHINEPEN]);

                            /* interior */
                            RectFill(rp,knob->Left,
                                     knob->Top,
                                     knob->Left + knob->Width - 1,
                                     knob->Top + knob->Height - 1);
                        } /* stdlook */
                    } /* gadget has no display (render) hook */
                } /* gadget not inside window border */

            } /* if (flags & AUTOKNOB) */
            else
            {
                struct Image *image = (struct Image *)gadget->GadgetRender;

            #if 0 /* CHECKME */
                if (knob->Top + image->Height <= bbox.Top + bbox.Height &&
                    knob->Left + image->Width <= bbox.Left + bbox.Width)
            #endif
                {
                #if 1
                    /* The place within the borders, as CalcKnobSize() keeps
                       it for a classic gadget */
                    WORD x = bbox.Left + pi->LeftBorder, y = bbox.Top + pi->TopBorder;

                    image->LeftEdge = knob->Left - x;
                    image->TopEdge = knob->Top - y;
                    // image->Width = knob->Width;
                    // image->Height = knob->Height;
                    
                    DrawImageState(rp,
                                   image,
                                   x,
                                   y,
                                   IDS_NORMAL,
                                   (struct DrawInfo *)dri);
                #else
                    image->LeftEdge = 0;
                    image->TopEdge = 0;

                    DrawImageState(rp,
                                   image,
                                   knob->Left,
                                   knob->Top,
                                   IDS_NORMAL,
                                   (struct DrawInfo *)dri);
                #endif
                }
            }

            if (gadget->Flags & GFLG_DISABLED)
            {
                RenderDisabledPattern(rp, (struct DrawInfo *)dri,
                                      brect.MinX, brect.MinY,
                                      brect.MaxX, brect.MaxY, IntuitionBase);
            }

            ReleaseGIRPort(rp);

        } /* if ((rp = ObtainGIRPort(&gi))) */

        FreeScreenDrawInfo(window->WScreen, (struct DrawInfo *)dri);

    } /* if ((dri = GetScreenDrawInfo(window->WScreen))) */

    ReturnVoid("RefreshPropGadgetKnob");

} /* RefreshPropGadgetKnob */

BOOL isonborder(struct Gadget *gadget, struct Window *window)
{
    if ((window->BorderBottom > 2) && (gadget->Flags & GFLG_RELBOTTOM))
    if (window->Height + gadget->TopEdge >= window->Height - window->BorderBottom) return TRUE;

    if ((window->BorderRight > 2) && (gadget->Flags & GFLG_RELRIGHT))
    if (window->Width + gadget->LeftEdge >= window->Width - window->BorderRight) return TRUE;

    return FALSE;
}
