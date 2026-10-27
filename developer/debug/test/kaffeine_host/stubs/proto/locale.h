#pragma once

#include <exec/types.h>
#include <libraries/locale.h>
#include <dos/dos.h>
#include <utility/hooks.h>

#ifdef __cplusplus
extern "C" {
#endif

void CloseCatalog(struct Catalog *catalog);
void CloseLocale(struct Locale *locale);
ULONG ConvToLower(struct Locale *locale, ULONG character);
ULONG ConvToUpper(struct Locale *locale, ULONG character);
void FormatDate(const struct Locale *locale, CONST_STRPTR format,
                const struct DateStamp *date, const struct Hook *hook);
BOOL ParseDate(const struct Locale *locale, struct DateStamp *date,
               CONST_STRPTR format, const struct Hook *hook);
CONST_STRPTR GetCatalogStr(struct Catalog *catalog, LONG stringNum,
                           CONST_STRPTR defaultString);
CONST_STRPTR GetLocaleStr(struct Locale *locale, ULONG stringNum);
BOOL IsAlNum(struct Locale *locale, ULONG character);
BOOL IsAlpha(struct Locale *locale, ULONG character);
BOOL IsCntrl(struct Locale *locale, ULONG character);
BOOL IsDigit(struct Locale *locale, ULONG character);
BOOL IsGraph(struct Locale *locale, ULONG character);
BOOL IsLower(struct Locale *locale, ULONG character);
BOOL IsPrint(struct Locale *locale, ULONG character);
BOOL IsPunct(struct Locale *locale, ULONG character);
BOOL IsSpace(struct Locale *locale, ULONG character);
BOOL IsUpper(struct Locale *locale, ULONG character);
BOOL IsXDigit(struct Locale *locale, ULONG character);
struct Catalog *OpenCatalogA(struct Locale *locale, CONST_STRPTR name,
                             const void *tags);
struct Locale *OpenLocale(CONST_STRPTR name);
ULONG StrConvert(struct Locale *locale, CONST_STRPTR string, APTR buffer,
                 ULONG bufferSize, ULONG type);
LONG StrnCmp(struct Locale *locale, CONST_STRPTR string1,
             CONST_STRPTR string2, LONG length, ULONG type);

#ifdef __cplusplus
}
#endif
