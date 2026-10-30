/* Copyright (C) 2026, The AROS Development Team. All rights reserved. */
#ifndef KAFFEINE_SEGMENTS_H
#define KAFFEINE_SEGMENTS_H
#include <exec/types.h>
#include <dos/bptr.h>
struct KaffeineContext;
struct KaffeineLibBase;
struct KaffeineSegment;
#ifdef __cplusplus
extern "C" {
#endif

/* Positive opaque handles, disjoint from BPTRs of the 32 MiB guest arena.
   Tickets are never reused while the library is resident, across contexts. */
#define KAFFEINE_SEGMENT_TOKEN_FIRST 0x20000001UL
#define KAFFEINE_SEGMENT_TOKEN_LAST  0x3FFFFFFEUL
#define KAFFEINE_SEGMENT_LIMIT 64
struct KaffeineSegmentLoad { struct KaffeineSegment *entry; };
struct KaffeineSegmentLoan {
    BPTR native;
    ULONG token;
    struct KaffeineSegment *entry;
    BOOL active;
};
struct KaffeineSegmentTransfer {
    BPTR native;
    ULONG token;
    struct KaffeineSegment *entry;
    BOOL active;
};
void Kaffeine_SegmentsInit(struct KaffeineLibBase *);
BOOL Kaffeine_SegmentToken(ULONG);
/* Reserve storage and a unique ticket before native LoadSeg/NewLoadSeg.
   End consumes that reservation and one native result, including rollback.
   Successful publication and native load failure preserve loader IoErr. */
BOOL Kaffeine_SegmentLoadBegin(struct KaffeineContext *, struct KaffeineSegmentLoad *);
ULONG Kaffeine_SegmentLoadEnd(struct KaffeineContext *, struct KaffeineSegmentLoad *, BPTR);
/* Only positively identified ELF or 68k HUNK with relocations may run.
   A release retires the handle immediately; an active loan defers unloading. */
BOOL Kaffeine_SegmentLoanBegin(struct KaffeineContext *, ULONG, struct KaffeineSegmentLoan *);
void Kaffeine_SegmentLoanEnd(struct KaffeineContext *, struct KaffeineSegmentLoan *);
BOOL Kaffeine_SegmentRelease(struct KaffeineContext *, ULONG);
/* Only an owned, idle 68k HUNK with relocations can be transferred. Begin
   hides the handle. End(FALSE) restores it without allocation. End(TRUE)
   only retires private metadata: the child may have unloaded it already.
   Leases are private, must not be copied and must end before ctx destruction. */
BOOL Kaffeine_SegmentTransferBegin(struct KaffeineContext *, ULONG, struct KaffeineSegmentTransfer *);
void Kaffeine_SegmentTransferEnd(struct KaffeineContext *, struct KaffeineSegmentTransfer *, BOOL);
void Kaffeine_SegmentsCleanup(struct KaffeineContext *);
#ifdef __cplusplus
}
#endif
#endif
