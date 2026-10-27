#pragma once
#include <exec/types.h>
/* Opaque native storage. The layer suite records acquisition separately. */
struct SignalSemaphore { ULONG opaque; };
