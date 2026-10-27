#pragma once
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#ifdef __cplusplus
extern "C" {
#endif
struct IntuiMessage *GT_FilterIMsg(struct IntuiMessage *imsg);
struct Menu *CreateMenusA(struct NewMenu *newmenu, struct TagItem *tagList);
void FreeMenus(struct Menu *menu);
BOOL LayoutMenusA(struct Menu *menu, APTR vi, struct TagItem *tagList);
BOOL LayoutMenuItemsA(struct MenuItem *menuitem, APTR vi, struct TagItem *tagList);
APTR GetVisualInfoA(struct Screen *screen, struct TagItem *tagList);
void FreeVisualInfo(APTR vi);
struct RastPort;
void DrawBevelBoxA(struct RastPort *rport, WORD left, WORD top, WORD width, WORD height,
                   struct TagItem *taglist);
struct IntuiMessage *GT_PostFilterIMsg(struct IntuiMessage *imsg);
struct Gadget *CreateContext(struct Gadget **glistpointer);
struct Gadget *CreateGadgetA(ULONG kind, struct Gadget *previous, struct NewGadget *ng,
                             struct TagItem *taglist);
void FreeGadgets(struct Gadget *glist);
struct Requester;
LONG GT_GetGadgetAttrsA(struct Gadget *gad, struct Window *win, struct Requester *req,
                        struct TagItem *taglist);
void GT_SetGadgetAttrsA(struct Gadget *gad, struct Window *win, struct Requester *req,
                        struct TagItem *taglist);
#ifdef __cplusplus
}
#endif
