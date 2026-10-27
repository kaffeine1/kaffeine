#pragma once
#include <utility/tagitem.h>
/* The tags of the BOOPSI gadget classes that GadTools takes, as AROS's */
#define GA_Dummy        (TAG_USER + 0x30000)
#define GA_Disabled     (GA_Dummy + 14)
#define GA_Immediate    (GA_Dummy + 21)
#define GA_RelVerify    (GA_Dummy + 22)
#define GA_TabCycle     (GA_Dummy + 36)
#define PGA_Dummy       (TAG_USER + 0x31000)
#define PGA_Freedom     (PGA_Dummy + 1)
#define STRINGA_Dummy   (TAG_USER + 0x32000)
#define STRINGA_ReplaceMode   (STRINGA_Dummy + 13)
#define STRINGA_Justification (STRINGA_Dummy + 16)
#define STRINGA_ExitHelp      (STRINGA_Dummy + 19)
#define LORIENT_HORIZ   1
#define LORIENT_VERT    2
#define LAYOUTA_Dummy   (TAG_USER + 0x38000)
#define LAYOUTA_Spacing (LAYOUTA_Dummy + 2)
