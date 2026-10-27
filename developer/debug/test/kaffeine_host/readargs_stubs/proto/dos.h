#pragma once
/* dos.library for the native argument parser and kaffeine_readargs.c. The
   parser itself is built from rom/dos by dos_readargs_host.c; the test
   provides the streams and the error code. */
#include <exec/libraries.h>
#include <dos/bptr.h>
#include <dos/rdargs.h>

struct DosLibrary { struct Library lib; };
struct TagItem;

#define DOSTRUE (-1L)
#define DOSFALSE 0L
#define DOS_RDARGS 5
#define RETURN_FAIL 20
#define ERROR_NO_FREE_STORE 103
#define ERROR_BAD_TEMPLATE 114
#define ERROR_BAD_NUMBER 115
#define ERROR_REQUIRED_ARG_MISSING 116
#define ERROR_KEY_NEEDS_ARG 117
#define ERROR_TOO_MANY_ARGS 118
#define ERROR_LINE_TOO_LONG 120
#define ERROR_OBJECT_WRONG_TYPE 212

#define ITEM_EQUAL    -2
#define ITEM_ERROR    -1
#define ITEM_NOTHING   0
#define ITEM_UNQUOTED  1
#define ITEM_QUOTED    2

#ifdef __cplusplus
extern "C" {
#endif

/* rom/dos */
struct RDArgs *ReadArgs(CONST_STRPTR tmpl, SIPTR *array, struct RDArgs *rdargs);
void FreeArgs(struct RDArgs *args);
LONG ReadItem(STRPTR buffer, LONG maxchars, struct CSource *input);
LONG FindArg(CONST_STRPTR tmpl, CONST_STRPTR keyword);
LONG StrToLong(CONST_STRPTR string, LONG *value);

/* the test */
APTR AllocDosObject(ULONG type, const struct TagItem *tags);
void FreeDosObject(ULONG type, APTR object);
SIPTR SetIoErr(SIPTR code);
SIPTR IoErr(void);
BPTR Input(void);
BPTR Output(void);
LONG IsInteractive(BPTR file);
STRPTR FGets(BPTR file, STRPTR buffer, ULONG size);
LONG FGetC(BPTR file);
LONG UnGetC(BPTR file, LONG character);
LONG FPuts(BPTR file, CONST_STRPTR string);
LONG Flush(BPTR file);

#ifdef __cplusplus
}
#endif
