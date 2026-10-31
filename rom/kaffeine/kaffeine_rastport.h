/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    RastPorts and fonts of 68k code.
*/
#ifndef KAFFEINE_RASTPORT_H
#define KAFFEINE_RASTPORT_H

#include "kaffeine_intern.h"

struct RastPort;
struct Screen;
struct TextFont;

/* Types of the shadows of Intuition and graphics objects in ctx->shadow_map */
enum
{
    KAFFEINE_SHADOW_WINDOW = 1,
    KAFFEINE_SHADOW_RASTPORT = 3,
    KAFFEINE_SHADOW_USERPORT = 4,
    KAFFEINE_SHADOW_SCREEN = 5,
    KAFFEINE_SHADOW_VIEWPORT = 6,
    KAFFEINE_SHADOW_INTUIMESSAGE = 8,
    /* the copy of an IntuiMessage that GadTools hands out */
    KAFFEINE_SHADOW_GTMESSAGE = 9,
    /* an IntuiMessage still in its port, as WaitPort() shows it */
    KAFFEINE_SHADOW_WAITMESSAGE = 10,
    /* the handle of a GadTools VisualInfo */
    KAFFEINE_SHADOW_VISUALINFO = 11,
    /* the 68k mirror of a gadget of a GadTools list */
    KAFFEINE_SHADOW_GADGET = 12,
    /* the 68k Screen of a public screen that the program locked
       (kaffeine_screens.c) */
    KAFFEINE_SHADOW_PUBSCREEN = 13,
    /* a gadget that the program built, on a window as its native copy */
    KAFFEINE_SHADOW_CLASSIC = 14,
    /* A requester window needs FreeSysRequest(), including at teardown. */
    KAFFEINE_SHADOW_REQUESTER_WINDOW = 15
};

/* The 68k RastPort and TextFont of the NDK */
#define M68K_SIZEOF_TEXTFONT 52
#define M68K_SIZEOF_TEXTEXTENT 12

#ifdef __cplusplus
extern "C" {
#endif

/* The native object behind a guest shadow of that type, or NULL. */
void *Kaffeine_ShadowNative(struct KaffeineContext *ctx, ULONG guest, UWORD type);
/* Register a shadow of that type; FALSE when the table is full. */
BOOL Kaffeine_ShadowAdd(struct KaffeineContext *ctx, ULONG guest, void *native, UWORD type);
/* The guest shadow of that type of a native object, or 0. */
ULONG Kaffeine_ShadowGuest(struct KaffeineContext *ctx, const void *native, UWORD type);
/* Forget the shadow of that type at a guest address. */
void Kaffeine_ShadowForget(struct KaffeineContext *ctx, ULONG guest, UWORD type);

/* The native screen of a 68k one: a screen that the program opened, or one
   that it locked with LockPubScreen(). NULL for anything else. */
struct Screen *Kaffeine_ScreenNative(struct KaffeineContext *ctx, ULONG guest);
/* The 68k screen that the program has of a native one: one that it opened,
   else the one of LockPubScreen(), or 0. */
ULONG Kaffeine_ScreenGuest(struct KaffeineContext *ctx, const struct Screen *native);

/* The 68k TextFont of a native font, with its name, monochrome bitmap and
   BE character tables in one owned guest block. All-null native data tables
   preserve metadata-only fonts; color and partial bitmap objects fail.
   Metrics are filled in again each time. There is one per native font. The
   font that a window or a screen comes with stays for the whole program;
   a font that the program opened goes once it is closed and no 68k RastPort
   shows it. 0 for NULL, without memory or with a full font table. */
ULONG Kaffeine_FontGuest(struct KaffeineContext *ctx, struct TextFont *font);
/* Keep a borrowed system font used by a caller-owned RastPort until cleanup. */
void Kaffeine_FontPin(struct KaffeineContext *ctx, ULONG guest);
/* The native font of a 68k TextFont, or NULL. */
struct TextFont *Kaffeine_FontNative(struct KaffeineContext *ctx, ULONG guest);
/* The 68k TextFont of a native font if there is one, or 0. */
ULONG Kaffeine_FontFind(struct KaffeineContext *ctx, const struct TextFont *font);
/* OpenFont() and CloseFont() of the program; the last close frees a 68k
   TextFont that no 68k RastPort shows and no window or screen came with. */
void Kaffeine_FontOpened(struct KaffeineContext *ctx, ULONG guest);
void Kaffeine_FontClosed(struct KaffeineContext *ctx, ULONG guest);
/* Adopt one successful native open, closing it on any publication failure. */
ULONG Kaffeine_FontAdopt(struct KaffeineContext *ctx, struct TextFont *font);
/* A private transaction acquiring exactly this native font, not a matching
 * substitute. Begin holds one native open and an unpublished typed view.
 * End(TRUE) pins it for Context lifetime using existing opens/retained state;
 * End(FALSE) balances that open and removes only a fresh unpublished view.
 * The driver remains serial between Begin and End; keep this lease native. */
struct KaffeineFontHold
{
    struct TextFont *native;
    ULONG guest;
    BOOL fresh, active;
};
BOOL Kaffeine_FontHoldBegin(struct KaffeineContext *, struct TextFont *, struct KaffeineFontHold *);
void Kaffeine_FontHoldEnd(struct KaffeineContext *, struct KaffeineFontHold *, BOOL publish);
/* Close all owned opens and release every snapshot at context teardown. */
void Kaffeine_FontsCleanup(struct KaffeineContext *ctx);

/* Fill the 100 bytes at guest as the 68k RastPort of native and register
   it, for a RastPort inside another guest structure. Its font stays for the
   whole program. */
BOOL Kaffeine_RastPortAttach(struct KaffeineContext *ctx, ULONG guest, struct RastPort *native);
/* A 68k RastPort of its own in the guest heap for native; 0 without memory. */
ULONG Kaffeine_RastPortCreate(struct KaffeineContext *ctx, struct RastPort *native);
/* Forget the 68k RastPort of native, and free a closed font that only it
   showed; Delete frees the RastPort too, for one that Create made. */
void Kaffeine_RastPortDetach(struct KaffeineContext *ctx, ULONG guest);
void Kaffeine_RastPortDelete(struct KaffeineContext *ctx, ULONG guest);

/* The native RastPort behind a 68k one, or NULL when guest is not the 68k
   RastPort of a window or a screen. The fields that the program may have
   written, such as the pens, the drawing mode and the pen position, go to
   the native RastPort first, and right after the thunk the dispatcher
   copies the native fields back. */
struct RastPort *Kaffeine_RastPortIn(struct KaffeineContext *ctx, ULONG guest);
/* Copy the fields of a native RastPort to its 68k RastPort. Only the public
   Flags bits are shown; the program's other bits stay. */
void Kaffeine_RastPortOut(struct KaffeineContext *ctx, ULONG guest, struct RastPort *native);

/* A temporary native RastPort for the pointer-free state of a caller-owned
   classic RastPort, whose 100 bytes the caller has validated: cleared, then
   every classic scalar from the guest, Flags masked to the public bits.
   Layer, BitMap, Font and the other pointers stay NULL; no 68k pointer is
   read. StateOut writes back, big-endian, only the scalars that differ
   between before and rp; pointers, RP_User, the reserved bytes and the
   program's private Flags bits stay as they are. */
void Kaffeine_RastPortStateIn(struct KaffeineContext *ctx, ULONG guest, struct RastPort *rp);
void Kaffeine_RastPortStateOut(struct KaffeineContext *ctx, ULONG guest,
                               const struct RastPort *before, const struct RastPort *rp);

#ifdef __cplusplus
}
#endif

#endif /* KAFFEINE_RASTPORT_H */
