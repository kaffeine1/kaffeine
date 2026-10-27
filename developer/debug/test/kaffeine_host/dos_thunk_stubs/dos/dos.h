#pragma once
#include <exec/types.h>
#include <dos/bptr.h>

#define DOS_FILEHANDLE 0
#define DOS_EXALLCONTROL 1
#define DOS_FIB 2
#define DOS_STDPKT 3
#define DOS_RDARGS 5
#define DOSTRUE (-1)
#define DOSFALSE 0
#define ERROR_NO_FREE_STORE 103
#define ERROR_BAD_NUMBER 115
#define ERROR_BAD_TEMPLATE 114
#define ERROR_OBJECT_NOT_FOUND 205
#define ERROR_ACTION_NOT_KNOWN 209
#define ERROR_SEEK_ERROR 219
#define OFFSET_BEGINNING (-1)
#define OFFSET_CURRENT 0
#define OFFSET_END 1
#define ERROR_LINE_TOO_LONG 120
#define ERROR_NO_MORE_ENTRIES 232
#define ERROR_NOT_IMPLEMENTED 236

struct DateStamp
{
    LONG ds_Days;
    LONG ds_Minute;
    LONG ds_Tick;
};

/* The native AROS FIB32 has a pointer-sized disk key even when its file size
   fields are 32-bit. On our 64-bit hosts this deliberately differs from the
   260-byte classic guest layout. */
struct FileInfoBlock
{
    IPTR fib_DiskKey;
    LONG fib_DirEntryType;
    UBYTE fib_FileName[108];
    LONG fib_Protection;
    LONG fib_EntryType;
    LONG fib_Size;
    LONG fib_NumBlocks;
    struct DateStamp fib_Date;
    UBYTE fib_Comment[80];
    UWORD fib_OwnerUID;
    UWORD fib_OwnerGID;
    UBYTE fib_Reserved[32];
};

#define ERROR_OBJECT_WRONG_TYPE 212
#define TICKS_PER_SECOND 50
#define ITEM_EQUAL -2
#define ITEM_ERROR -1
#define ITEM_NOTHING 0
#define ITEM_UNQUOTED 1
#define ITEM_QUOTED 2
struct InfoData
{
    LONG id_NumSoftErrors, id_UnitNumber, id_DiskState;
    LONG id_NumBlocks, id_NumBlocksUsed, id_BytesPerBlock, id_DiskType;
    BPTR id_VolumeNode;
    LONG id_InUse;
};
