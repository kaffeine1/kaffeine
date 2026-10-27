#pragma once
#include <exec/ports.h>
#include <exec/devices.h>
struct IORequest
{
    struct Message io_Message;
    struct Device *io_Device;
    void *io_Unit;
    UWORD io_Command;
    UBYTE io_Flags;
    BYTE io_Error;
};
struct IOStdReq
{
    struct Message io_Message;
    struct Device *io_Device;
    void *io_Unit;
    UWORD io_Command;
    UBYTE io_Flags;
    BYTE io_Error;
    ULONG io_Actual;
    ULONG io_Length;
    APTR io_Data;
    ULONG io_Offset;
};
#define IOF_QUICK 1
