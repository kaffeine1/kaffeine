#pragma once
#include <intuition/intuition.h>
#ifdef __cplusplus
extern "C" {
#endif
struct NewWindow;
struct TagItem;
struct List;
struct Screen *LockPubScreen(CONST_STRPTR name);
void UnlockPubScreen(UBYTE *name, struct Screen *screen);
struct List *LockPubScreenList(void);
void UnlockPubScreenList(void);
UBYTE *NextPubScreen(struct Screen *screen, UBYTE *nameBuffer);
struct Screen *GetDefaultPubScreen(UBYTE *nameBuffer);
struct NewScreen;
struct Screen *OpenScreenTagList(struct NewScreen *newScreen, struct TagItem *tagList);
BOOL CloseScreen(struct Screen *screen);
struct Window *OpenWindowTagList(struct NewWindow *newWindow, struct TagItem *tagList);
BOOL ModifyIDCMP(struct Window *window, ULONG flags);
void CloseWindow(struct Window *window);
BOOL SetMenuStrip(struct Window *window, struct Menu *menu);
BOOL ResetMenuStrip(struct Window *window, struct Menu *menu);
void ClearMenuStrip(struct Window *window);
struct MenuItem *ItemAddress(struct Menu *menustrip, UWORD menunumber);
void OffMenu(struct Window *window, UWORD menunumber);
void OnMenu(struct Window *window, UWORD menunumber);
struct Window *BuildEasyRequestArgs(struct Window *window, struct EasyStruct *easyStruct,
                                    ULONG idcmp, RAWARG args);
LONG SysReqHandler(struct Window *window, ULONG *idcmpPtr, BOOL waitInput);
void FreeSysRequest(struct Window *window);
void DisposeObject(APTR object);
/* BOOPSI, as AROS declares it in intuition/classusr.h */
#ifndef INTUITION_CLASSUSR_H
typedef ULONG Object;
typedef struct { ULONG MethodID; } *Msg;
#endif
struct Requester;
BOOL ActivateGadget(struct Gadget *gadget, struct Window *window, struct Requester *requester);
ULONG SetAttrsA(APTR object, struct TagItem *tagList);
ULONG GetAttr(ULONG attrID, Object *object, IPTR *storagePtr);
IPTR SetGadgetAttrsA(struct Gadget *gadget, struct Window *window, struct Requester *requester,
                     struct TagItem *tagList);
IPTR DoGadgetMethodA(struct Gadget *gad, struct Window *win, struct Requester *req, Msg msg);
void OnGadget(struct Gadget *gadget, struct Window *window, struct Requester *requester);
void OffGadget(struct Gadget *gadget, struct Window *window, struct Requester *requester);
void ModifyProp(struct Gadget *gadget, struct Window *window, struct Requester *requester,
                UWORD flags, UWORD horizPot, UWORD vertPot, UWORD horizBody, UWORD vertBody);
void NewModifyProp(struct Gadget *gadget, struct Window *window, struct Requester *requester,
                   UWORD flags, UWORD horizPot, UWORD vertPot, UWORD horizBody, UWORD vertBody,
                   WORD numGad);
UWORD AddGList(struct Window *window, struct Gadget *gadget, UWORD position, WORD numGad,
               struct Requester *requester);
UWORD AddGadget(struct Window *window, struct Gadget *gadget, UWORD position);
UWORD RemoveGList(struct Window *window, struct Gadget *gadget, WORD numGad);
void RefreshGList(struct Gadget *gadgets, struct Window *window, struct Requester *requester,
                  WORD numGad);
/* Depth calls and the IntuitionBase lock, for the layer bridge */
void WindowToBack(struct Window *window);
void WindowToFront(struct Window *window);
ULONG LockIBase(ULONG what);
void UnlockIBase(ULONG ibLock);
#ifdef __cplusplus
}
#endif
