#pragma once

#include <exec/types.h>

struct Locale
{
    STRPTR  loc_LocaleName;
    STRPTR  loc_LanguageName;
    STRPTR  loc_PrefLanguages[10];
    ULONG   loc_Flags;

    ULONG   loc_CodeSet;
    ULONG   loc_CountryCode;
    ULONG   loc_TelephoneCode;
    LONG    loc_GMTOffset;
    UBYTE   loc_MeasuringSystem;
    UBYTE   loc_CalendarType;
    UBYTE   loc_Reserved0[2];

    STRPTR  loc_DateTimeFormat;
    STRPTR  loc_DateFormat;
    STRPTR  loc_TimeFormat;

    STRPTR  loc_ShortDateTimeFormat;
    STRPTR  loc_ShortDateFormat;
    STRPTR  loc_ShortTimeFormat;

    STRPTR  loc_DecimalPoint;
    STRPTR  loc_GroupSeparator;
    STRPTR  loc_FracGroupSeparator;
    UBYTE  *loc_Grouping;
    UBYTE  *loc_FracGrouping;

    STRPTR  loc_MonDecimalPoint;
    STRPTR  loc_MonGroupSeparator;
    STRPTR  loc_MonFracGroupSeparator;
    UBYTE  *loc_MonGrouping;
    UBYTE  *loc_MonFracGrouping;
    UBYTE   loc_MonFracDigits;
    UBYTE   loc_MonIntFracDigits;
    UBYTE   loc_Reserved1[2];

    STRPTR  loc_MonCS;
    STRPTR  loc_MonSmallCS;
    STRPTR  loc_MonIntCS;

    STRPTR  loc_MonPositiveSign;
    UBYTE   loc_MonPositiveSpaceSep;
    UBYTE   loc_MonPositiveSignPos;
    UBYTE   loc_MonPositiveCSPos;
    UBYTE   loc_Reserved2;

    STRPTR  loc_MonNegativeSign;
    UBYTE   loc_MonNegativeSpaceSep;
    UBYTE   loc_MonNegativeSignPos;
    UBYTE   loc_MonNegativeCSPos;
    UBYTE   loc_Reserved3;
};

struct Catalog;

#define MS_ISO          0
#define MS_AMERICAN     1
#define MS_IMPERIAL     2
#define MS_BRITISH      3

#define CT_7SUN         0
#define CT_7MON         1
#define CT_7TUE         2
#define CT_7WED         3
#define CT_7THU         4
#define CT_7FRI         5
#define CT_7SAT         6

#define SS_NOSPACE      0
#define SS_SPACE        1

#define SP_PARENS       0
#define SP_PREC_ALL     1
#define SP_SUCC_ALL     2
#define SP_PREC_CURR    3
#define SP_SUCC_CURR    4

#define CSP_PRECEDES    0
#define CSP_SUCCEEDS    1

#define SC_ASCII        0
#define SC_COLLATE1     1
#define SC_COLLATE2     2

#define DAY_1           1
#define DAY_2           2
#define DAY_3           3
#define DAY_4           4
#define DAY_5           5
#define DAY_6           6
#define DAY_7           7

#define ABDAY_1         8
#define ABDAY_2         9
#define ABDAY_3         10
#define ABDAY_4         11
#define ABDAY_5         12
#define ABDAY_6         13
#define ABDAY_7         14

#define MON_1           15
#define MON_2           16
#define MON_3           17
#define MON_4           18
#define MON_5           19
#define MON_6           20
#define MON_7           21
#define MON_8           22
#define MON_9           23
#define MON_10          24
#define MON_11          25
#define MON_12          26

#define ABMON_1         27
#define ABMON_2         28
#define ABMON_3         29
#define ABMON_4         30
#define ABMON_5         31
#define ABMON_6         32
#define ABMON_7         33
#define ABMON_8         34
#define ABMON_9         35
#define ABMON_10        36
#define ABMON_11        37
#define ABMON_12        38

#define YESSTR          39
#define NOSTR           40

#define AM_STR          41
#define PM_STR          42

#define SOFTHYPHEN      43
#define HARDHYPHEN      44

#define OPENQUOTE       45
#define CLOSEQUOTE      46

#define YESTERDAYSTR    47
#define TODAYSTR        48
#define TOMORROWSTR     49
#define FUTURESTR       50

#define MAXSTRMSG       52
#define NDK_MAXSTRMSG   71
