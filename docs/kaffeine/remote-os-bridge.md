# Remote desktop OS bridge

Copyright (C) 2026, The AROS Development Team. All rights reserved.
Author: Michele Dipace (@kaffeine1)

These adapters support classic OS clients that inspect native Intuition screens,
read RTG pixels, inject input events and start native commands. They translate
OS interfaces; the application and CPU backends need no application-specific patch.

`LockIBase` exports a bounded classic read-only graph of the current native
IntuitionBase, screens, windows, layers and classic gadgets/menus. Native fields
are read through SDK C members, not generated native structure offsets. Only
scalar values and protected guest identities enter the graph; native pointers,
planes, callbacks and BOOPSI objects do not. Nested locks belong to the current
guest task, which stays scheduled until its outer unlock. Task removal and domain
teardown release a remaining native lock. Native I/O and GUI mutations are refused
inside that scope; the scope admits only the explicitly registered read operations
and their bounded allocation/time helpers.

`GetVPModeID`, `GetBitMapAttr`, `GetCyberMapAttr` and `ReadPixelArray` resolve typed
screen/bitmap/RastPort identities. `GetDisplayInfoData` (NULL handle, `DTAG_DISP`,
`DTAG_DIMS`, `DTAG_NAME` and `DTAG_MNTR`) also runs in that scope, as native Intuition
queries display data under IBase itself: AROS's call takes no lock but the display
mode database's semaphore, shared and released inside, and none of that semaphore's
holders waits for IBase. Its generated forwarder and the other display vectors stay
refused there. Of `DoIO`, only keyboard.device `KBD_READMATRIX` of an idle open
request runs in that scope: AROS copies the matrix inside BeginIO() with no lock and
no reply, so the call neither waits nor reaches input.device or Intuition. Timer
delays, input events and every other request stay refused while IBase is held. Pixel output goes through private native
scratch and bounded row copyback, preserving untouched clipping, padding and
prefixes. Supported formats are the native packed RGB families. Unsupported
LUT8/GREY/RAW formats and native memory-address attributes are not exposed.
`ScreenToFront` accepts a current typed screen or a screen snapshot revalidated by
an exact native public visitor lease. An owned live window also supplies a typed
borrow of its default screen; the last window releases that borrow unless another
screen holder remains. Stale or unleased private foreign screens are refused.

`input.device` `IND_WRITEEVENT` follows the native single-event contract: its
linked successor and `io_Length` are ignored. The bridge validates one classic
22-byte event, copies accepted scalar fields and dead-key history, and performs
one synchronous native `DoIO`. Pixel positions use a private `IEPointerPixel`
and a native public screen lease held until completion. No native handler rewrite
is copied back; only the documented link/timestamp changes return after success.
Unknown/pointer-bearing classes, tablet payloads and unleased screens are refused.
A same-domain target requesting synchronous Intuition verify replies can still
block synchronous input; that interaction is not qualified here.

`keymap.library` `MapANSI` supports the native default map. Its length counts
code/qualifier pairs. Private copies preserve partial writes even on native 0/-1
results, while untouched capacity retains its original bytes. Negative counts,
overlap, protected storage and custom guest keymaps are refused. The native bound
of at most three pairs per input byte avoids staging unused caller capacity.

Synchronous `SystemTagList` translates `NP_Error` to `SYS_Error` and supports
`NP_CloseError=FALSE`. A bounded `NP_Path` uses private exact-allocation reservations
and lock-transfer leases. Native nodes contain only duplicated native locks.
Because native synchronous CLI_SYSTEM inherits its parent's path, a private path
is temporarily installed on the current task's own native broker CLI, restored on
every exit. Guest nodes/locks are consumed only after launch succeeds; failures
restore their ownership. Async/custom-shell path ownership and transferring error
streams remain refused. No guest BPTR or string pointer survives a native wait.

The target fixtures distinguish owned guest windows from an independent finite
native window. The latter proves actual pixel readback, text and mouse delivery
on a 24-bit Workbench. QEMU, physical deployment, TLS, custom keymaps and complete
library coverage are separate qualifications; passing a protocol reply alone is
not evidence that an input event reached its intended window.
