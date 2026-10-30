#pragma once
#include <exec/types.h>
/* Native fields used by backfill entry provenance. Existing layer tests
   still record acquisition separately; only focused recipe tests invoke
   native backfill recipes and provide a real owner/count collaborator. */
struct Task;
struct SignalSemaphore { ULONG opaque; struct Task *ss_Owner; WORD ss_NestCount; };
