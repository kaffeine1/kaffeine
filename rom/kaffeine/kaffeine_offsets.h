/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)

    kaffeine_offsets.h — Named constants for m68k struct offsets, opcodes,
    and hardware constants used throughout the kaffeine codebase.

    Values are sourced from m68k_offsets.txt (generated from m68k struct
    definitions) and verified against the actual usage in the code.
*/
#ifndef KAFFEINE_OFFSETS_H
#define KAFFEINE_OFFSETS_H

/* ── M68K struct sizes ───────────────────────────────────────────── */

#define M68K_SIZEOF_PROCESS         228
#define M68K_SIZEOF_CLI             64
#define M68K_SIZEOF_WINDOW          136  /* Window struct from offsets.txt */
#define M68K_SIZEOF_SCREEN          346
#define M68K_SIZEOF_MSGPORT         34
#define M68K_SIZEOF_RASTPORT        100
#define M68K_SIZEOF_LIBRARY         34
#define M68K_SIZEOF_DEVICE          34
#define M68K_SIZEOF_EXECBASE        632
#define M68K_SIZEOF_INTUITEXT       20
#define M68K_SIZEOF_NEWWINDOW       48
#define M68K_SIZEOF_NEWSCREEN       32
#define M68K_SIZEOF_TEXTATTR        8
#define M68K_SIZEOF_FILEINFOBLK     260
#define M68K_SIZEOF_INTUIMESSAGE    52
#define M68K_SIZEOF_NODE            14
#define M68K_SIZEOF_RESIDENT        26

/* ── Process struct offsets (m68k) ───────────────────────────────── */

#define M68K_PR_MSGPORT             92
#define M68K_PR_SEGLIST             128
#define M68K_PR_CURRENTDIR          152
#define M68K_PR_CIS                 156
#define M68K_PR_COS                 160
#define M68K_PR_CONSOLETASK         164
#define M68K_PR_FILESYSTEMTASK      168
#define M68K_PR_CLI                 172
#define M68K_PR_PKTWAIT             180
#define M68K_PR_WINDOWPTR           184
#define M68K_PR_ARGUMENTS           204
#define M68K_PR_CES                 224
#define M68K_PR_PAD                 126

/* ── CLI struct offsets (m68k) ───────────────────────────────────── */

#define M68K_CLI_COMMANDNAME        16
#define M68K_CLI_SETNAME            4
#define M68K_CLI_PROMPT             24
#define M68K_CLI_STRING_SLOT        260 /* length + 255 bytes + NUL, BPTR aligned */
#define M68K_CLI_DEFAULTSTACK       52
#define M68K_CLI_MODULE             60

/* ── ExecBase offsets (m68k) ─────────────────────────────────────── */

#define M68K_EXECBASE_LIBVERSION    20
#define M68K_EXECBASE_LIBREVISION   22
#define M68K_EXECBASE_THISTASK      276
#define M68K_EXECBASE_ATTNFLAGS     296

/* ExecBase embedded list offsets (m68k) */
#define M68K_EXECBASE_MEMLIST       322
#define M68K_EXECBASE_RESOURCELIST  336
#define M68K_EXECBASE_DEVICELIST    350
#define M68K_EXECBASE_INTRLIST      364
#define M68K_EXECBASE_LIBLIST       378
#define M68K_EXECBASE_PORTLIST      392
#define M68K_EXECBASE_TASKREADY     406
#define M68K_EXECBASE_TASKWAIT      420
#define M68K_EXECBASE_SEMAPHORELIST 532

/* ── Library struct offsets (m68k) ───────────────────────────────── */

#define M68K_LIB_LNTYPE            8
#define M68K_LIB_LNNAME            10
#define M68K_LIB_FLAGS             14
#define M68K_LIB_NEGSIZE           16
#define M68K_LIB_POSSIZE           18
#define M68K_LIB_VERSION           20
#define M68K_LIB_REVISION          22
#define M68K_LIB_IDSTRING          24
#define M68K_LIB_OPENCNT           32

/* lib_Flags (exec/libraries.h) */
#define M68K_LIBF_CHANGED          0x02
#define M68K_LIBF_SUMUSED          0x04

/* ── NewWindow struct offsets (m68k) ─────────────────────────────── */

#define M68K_NW_LEFTEDGE            0
#define M68K_NW_TOPEDGE             2
#define M68K_NW_WIDTH               4
#define M68K_NW_HEIGHT              6
#define M68K_NW_DETAILPEN           8
#define M68K_NW_BLOCKPEN            9
#define M68K_NW_IDCMPFLAGS          10
#define M68K_NW_FLAGS               14
#define M68K_NW_FIRSTGADGET         18
#define M68K_NW_CHECKMARK           22
#define M68K_NW_TITLE               26
#define M68K_NW_SCREEN              30
#define M68K_NW_BITMAP              34
#define M68K_NW_MINWIDTH            38
#define M68K_NW_MINHEIGHT           40
#define M68K_NW_MAXWIDTH            42
#define M68K_NW_MAXHEIGHT           44
#define M68K_NW_TYPE                46
#define M68K_ENW_EXTENSION          48  /* ExtNewWindow, with WFLG_NW_EXTENDED */
#define M68K_SIZEOF_EXTNEWWINDOW    52

/* ── Window struct offsets (m68k) ────────────────────────────────── */

#define M68K_WIN_LEFTEDGE           4
#define M68K_WIN_TOPEDGE            6
#define M68K_WIN_WIDTH              8
#define M68K_WIN_HEIGHT             10
#define M68K_WIN_MOUSEY             12
#define M68K_WIN_MOUSEX             14
#define M68K_WIN_MINWIDTH           16
#define M68K_WIN_MINHEIGHT          18
#define M68K_WIN_MAXWIDTH           20
#define M68K_WIN_MAXHEIGHT          22
#define M68K_WIN_FLAGS              24
#define M68K_WIN_MENUSTRIP          28
#define M68K_WIN_TITLE              32
#define M68K_WIN_WSCREEN            46
#define M68K_WIN_RPORT              50
#define M68K_WIN_BORDERLEFT         54
#define M68K_WIN_BORDERTOP          55
#define M68K_WIN_BORDERRIGHT        56
#define M68K_WIN_BORDERBOTTOM       57
#define M68K_WIN_IDCMPFLAGS         82
#define M68K_WIN_USERPORT           86
#define M68K_WIN_DETAILPEN          98
#define M68K_WIN_BLOCKPEN           99
#define M68K_WIN_GZZMOUSEX          108
#define M68K_WIN_GZZMOUSEY          110
#define M68K_WIN_GZZWIDTH           112
#define M68K_WIN_GZZHEIGHT          114

/* ── NewScreen struct offsets (m68k) ─────────────────────────────── */

#define M68K_NS_LEFTEDGE            0
#define M68K_NS_TOPEDGE             2
#define M68K_NS_WIDTH               4
#define M68K_NS_HEIGHT              6
#define M68K_NS_DEPTH               8
#define M68K_NS_DETAILPEN           10
#define M68K_NS_BLOCKPEN            11
#define M68K_NS_VIEWMODES           12
#define M68K_NS_TYPE                14
#define M68K_NS_DEFAULTTITLE        20

/* ── Screen struct offsets (m68k) ────────────────────────────────── */

#define M68K_SCR_LEFTEDGE           8
#define M68K_SCR_TOPEDGE            10
#define M68K_SCR_WIDTH              12
#define M68K_SCR_HEIGHT             14
#define M68K_SCR_FLAGS              20
#define M68K_SCR_TITLE              22
#define M68K_SCR_VIEWPORT           44
#define M68K_SCR_RASTPORT           84

/* ── IntuiText struct offsets (m68k) ─────────────────────────────── */

#define M68K_IT_FRONTPEN            0
#define M68K_IT_BACKPEN             1
#define M68K_IT_DRAWMODE            2
#define M68K_IT_LEFTEDGE            4
#define M68K_IT_TOPEDGE             6
#define M68K_IT_FONT                8
#define M68K_IT_ITEXT               12
#define M68K_IT_NEXTTEXT            16

/* ── FileInfoBlock offsets (m68k) ────────────────────────────────── */

#define M68K_FIB_DISKKEY            0
#define M68K_FIB_DIRENTRYTYPE       4
#define M68K_FIB_FILENAME           8
#define M68K_FIB_FILENAME_SIZE      108
#define M68K_FIB_PROTECTION         116
#define M68K_FIB_ENTRYTYPE          120
#define M68K_FIB_SIZE               124
#define M68K_FIB_NUMBLOCKS          128
#define M68K_FIB_DATE_DAYS          132
#define M68K_FIB_DATE_MINUTE        136
#define M68K_FIB_DATE_TICK          140
#define M68K_FIB_COMMENT            144
#define M68K_FIB_COMMENT_SIZE       80
#define M68K_FIB_OWNERUID           224
#define M68K_FIB_OWNERGID           226

/* ── AnchorPath and AChain offsets (m68k, dos/dosasl.h) ──────────── */

#define M68K_AP_BASE                0
#define M68K_AP_LAST                4
#define M68K_AP_BREAKBITS           8
#define M68K_AP_FOUNDBREAK          12
#define M68K_AP_FLAGS               16
#define M68K_AP_STRLEN              18
#define M68K_AP_INFO                20
#define M68K_AP_BUF                 280
#define M68K_AN_CHILD               0
#define M68K_AN_PARENT              4
#define M68K_AN_LOCK                8
#define M68K_AN_INFO                12
#define M68K_AN_FLAGS               272
#define M68K_AN_STRING              273

/* ── IntuiMessage offsets (m68k) ─────────────────────────────────── */

#define M68K_IMSG_IDCMPWINDOW       44

/* ── TextAttr offsets (m68k) ─────────────────────────────────────── */

#define M68K_TA_NAME                0
#define M68K_TA_YSIZE               4
#define M68K_TA_STYLE               6
#define M68K_TA_FLAGS               7

/* ── Resident struct offsets (m68k) ──────────────────────────────── */

#define M68K_RT_MATCHWORD           0
#define M68K_RT_MATCHTAG            2
#define M68K_RT_ENDSKIP             6
#define M68K_RT_FLAGS               10
#define M68K_RT_VERSION             11
#define M68K_RT_TYPE                12
#define M68K_RT_PRI                 13
#define M68K_RT_NAME                14
#define M68K_RT_IDSTRING            18
#define M68K_RT_INIT                22

/* ── RTF_AUTOINIT init table offsets ─────────────────────────────── */

#define M68K_AUTOINIT_DATASIZE      0
#define M68K_AUTOINIT_FUNCTABLE     4
#define M68K_AUTOINIT_DATAINIT      8
#define M68K_AUTOINIT_INITFUNC      12

/* ── M68K opcodes ────────────────────────────────────────────────── */

#define M68K_OP_ALINE               0xA000
#define M68K_OP_NOP                 0x4E71
#define M68K_OP_RTE                 0x4E73
#define M68K_OP_STOP                0x4E72
#define M68K_OP_JMP_ABS_L           0x4EF9
#define M68K_OP_ADDQ8_SP            0x508F  /* ADDQ.L #8,SP */
#define M68K_OP_JMP_A5              0x4ED5  /* JMP (A5) */

/* ── RTC (Resident Tag) constants ────────────────────────────────── */

#define M68K_RTC_MATCHWORD          0x4AFC
#define M68K_RTF_AUTOINIT           (1 << 7)

/* ── PAL timing constants ────────────────────────────────────────── */

#define PAL_CLOCKS_PER_LINE         227
#define PAL_LINES_PER_FRAME         313

/* ── Caches of CacheClearE() (exec/execbase.h) ───────────────────── */

#define M68K_CACRF_CLEARI           0x00000008  /* CACRF_ClearI */

/* ── Custom chip constants ───────────────────────────────────────── */

/* The 68EC040 model decodes all 32 address bits, so the registers appear
   once and not again every 16 MB. The heap keeps these 4 KB for them. */
#define CUSTOM_BASE                 0x00DFF000
#define CUSTOM_SIZE                 0x00001000
#define CUSTOM_MASK                 0xFFFFF000
#define POTGOR_DEFAULT              0xFF00

/* ── Heap constants ──────────────────────────────────────────────── */

#define KAFFEINE_HEAP_HDR            8   /* heap block header size */
#define KAFFEINE_HEAP_MIN_SPLIT      16  /* minimum remainder to split */

/* ── Jump table slot size ────────────────────────────────────────── */

#define M68K_JT_SLOT_SIZE           6   /* ALINE(2) + NOP(2) + NOP(2) or JMP(2) + addr(4) */

/* ── Hunk type constants ─────────────────────────────────────────── */

#ifndef DOS_DOSHUNKS_H
#define HUNK_HEADER                 0x3F3
#define HUNK_CODE                   0x3E9
#define HUNK_DATA                   0x3EA
#define HUNK_BSS                    0x3EB
#define HUNK_RELOC32                0x3EC
#define HUNK_END                    0x3F2
#endif
#define HUNK_TYPE_MASK              0x3FFFFFFF

/* ── Function table terminator ───────────────────────────────────── */

#define M68K_FUNCTABLE_END          0xFFFFFFFF

/* ── LINEA exception vector ──────────────────────────────────────── */

#define M68K_VEC_LINEA              0x28  /* vector 10 * 4 */

/* ── AttnFlags of the emulated CPU ───────────────────────────────── */

/* AFF_68010 | AFF_68020 | AFF_68030 | AFF_68040, and no FPU flag:
   The M68EC040 model sends FPU instructions to Line-F, where the adapter
   ends the program. A program that saw AFF_68881, AFF_68882 or AFF_FPU40
   would run FPU code, as the startup code of libnix FPU builds does. */
#define M68K_ATTNFLAGS_68040        0x000F

/* ── struct Locale (libraries/locale.h, 168 bytes on m68k) ───────── */

#define M68K_SIZEOF_LOCALE               168
#define M68K_LOC_LOCALENAME              0     /* STRPTR loc_LocaleName */
#define M68K_LOC_LANGUAGENAME            4     /* STRPTR loc_LanguageName */
#define M68K_LOC_PREFLANGUAGES           8     /* STRPTR loc_PrefLanguages[10] */
#define M68K_LOC_PREFLANGUAGES_COUNT     10
#define M68K_LOC_FLAGS                   48    /* ULONG  loc_Flags */
#define M68K_LOC_CODESET                 52    /* ULONG  loc_CodeSet */
#define M68K_LOC_COUNTRYCODE             56    /* ULONG  loc_CountryCode */
#define M68K_LOC_TELEPHONECODE           60    /* ULONG  loc_TelephoneCode */
#define M68K_LOC_GMTOFFSET               64    /* LONG   loc_GMTOffset */
#define M68K_LOC_MEASURINGSYSTEM         68    /* UBYTE  loc_MeasuringSystem */
#define M68K_LOC_CALENDARTYPE            69    /* UBYTE  loc_CalendarType */
#define M68K_LOC_RESERVED0               70    /* UBYTE  loc_Reserved0[2] */
#define M68K_LOC_DATETIMEFORMAT          72    /* STRPTR loc_DateTimeFormat */
#define M68K_LOC_DATEFORMAT              76    /* STRPTR loc_DateFormat */
#define M68K_LOC_TIMEFORMAT              80    /* STRPTR loc_TimeFormat */
#define M68K_LOC_SHORTDATETIMEFORMAT     84    /* STRPTR loc_ShortDateTimeFormat */
#define M68K_LOC_SHORTDATEFORMAT         88    /* STRPTR loc_ShortDateFormat */
#define M68K_LOC_SHORTTIMEFORMAT         92    /* STRPTR loc_ShortTimeFormat */
#define M68K_LOC_DECIMALPOINT            96    /* STRPTR loc_DecimalPoint */
#define M68K_LOC_GROUPSEPARATOR          100   /* STRPTR loc_GroupSeparator */
#define M68K_LOC_FRACGROUPSEPARATOR      104   /* STRPTR loc_FracGroupSeparator */
#define M68K_LOC_GROUPING                108   /* UBYTE *loc_Grouping */
#define M68K_LOC_FRACGROUPING            112   /* UBYTE *loc_FracGrouping */
#define M68K_LOC_MONDECIMALPOINT         116   /* STRPTR loc_MonDecimalPoint */
#define M68K_LOC_MONGROUPSEPARATOR       120   /* STRPTR loc_MonGroupSeparator */
#define M68K_LOC_MONFRACGROUPSEPARATOR   124   /* STRPTR loc_MonFracGroupSeparator */
#define M68K_LOC_MONGROUPING             128   /* UBYTE *loc_MonGrouping */
#define M68K_LOC_MONFRACGROUPING         132   /* UBYTE *loc_MonFracGrouping */
#define M68K_LOC_MONFRACDIGITS           136   /* UBYTE  loc_MonFracDigits */
#define M68K_LOC_MONINTFRACDIGITS        137   /* UBYTE  loc_MonIntFracDigits */
#define M68K_LOC_RESERVED1               138   /* UBYTE  loc_Reserved1[2] */
#define M68K_LOC_MONCS                   140   /* STRPTR loc_MonCS */
#define M68K_LOC_MONSMALLCS              144   /* STRPTR loc_MonSmallCS */
#define M68K_LOC_MONINTCS                148   /* STRPTR loc_MonIntCS */
#define M68K_LOC_MONPOSITIVESIGN         152   /* STRPTR loc_MonPositiveSign */
#define M68K_LOC_MONPOSITIVESPACESEP     156   /* UBYTE  loc_MonPositiveSpaceSep */
#define M68K_LOC_MONPOSITIVESIGNPOS      157   /* UBYTE  loc_MonPositiveSignPos */
#define M68K_LOC_MONPOSITIVECSPOS        158   /* UBYTE  loc_MonPositiveCSPos */
#define M68K_LOC_RESERVED2               159   /* UBYTE  loc_Reserved2 */
#define M68K_LOC_MONNEGATIVESIGN         160   /* STRPTR loc_MonNegativeSign */
#define M68K_LOC_MONNEGATIVESPACESEP     164   /* UBYTE  loc_MonNegativeSpaceSep */
#define M68K_LOC_MONNEGATIVESIGNPOS      165   /* UBYTE  loc_MonNegativeSignPos */
#define M68K_LOC_MONNEGATIVECSPOS        166   /* UBYTE  loc_MonNegativeCSPos */
#define M68K_LOC_RESERVED3               167   /* UBYTE  loc_Reserved3 */

#endif /* KAFFEINE_OFFSETS_H */
