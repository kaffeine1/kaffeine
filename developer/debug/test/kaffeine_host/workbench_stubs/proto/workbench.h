#pragma once
#include <workbench/workbench.h>
#ifdef __cplusplus
extern "C" {
#endif
struct AppWindow *AddAppWindowA(IPTR id, IPTR userdata, struct Window *window,
                              struct MsgPort *port, struct TagItem *tags);
BOOL RemoveAppWindow(struct AppWindow *window);
struct AppIcon *AddAppIconA(IPTR id, IPTR userdata, CONST_STRPTR text,
                          struct MsgPort *port, BPTR lock, struct DiskObject *icon,
                          struct TagItem *tags);
BOOL RemoveAppIcon(struct AppIcon *icon);
struct AppMenuItem *AddAppMenuItemA(IPTR id, IPTR userdata, APTR text,
                                  struct MsgPort *port, struct TagItem *tags);
BOOL RemoveAppMenuItem(struct AppMenuItem *item);
BOOL WBInfo(BPTR lock, CONST_STRPTR name, struct Screen *screen);
#ifdef __cplusplus
}
#endif
