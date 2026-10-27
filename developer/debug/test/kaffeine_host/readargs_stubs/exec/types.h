#pragma once
/* The host stub types, with strings as in AROS: STRPTR points to UBYTE in
   C and to char in C++. The native parser depends on that, and also uses
   BYTE and TEXT. */
#ifdef __cplusplus
#define HOST_STRING_BYTE char
#else
#define HOST_STRING_BYTE UBYTE
#endif
typedef unsigned char UBYTE; typedef unsigned short UWORD;
typedef unsigned int ULONG; typedef signed int LONG;
typedef unsigned long IPTR; typedef signed long SIPTR;
typedef int BOOL; typedef void *APTR;
typedef HOST_STRING_BYTE *STRPTR;
typedef const HOST_STRING_BYTE *CONST_STRPTR;
typedef signed char BYTE;
typedef unsigned char TEXT;
#undef HOST_STRING_BYTE
#define TRUE 1
#define FALSE 0
#ifndef NULL
#ifdef __cplusplus
#define NULL nullptr
#else
#define NULL ((void*)0)
#endif
#endif
