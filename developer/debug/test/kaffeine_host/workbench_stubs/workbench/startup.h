#pragma once
#include <dos/bptr.h>
struct WBArg
{
    BPTR wa_Lock;
    BYTE *wa_Name;
};
