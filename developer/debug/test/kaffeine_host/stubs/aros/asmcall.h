#pragma once

/* The host has a single C calling convention. Hook calls use its canonical
   pointer-sized signature, including when locale passes a char or ULONG. */
#define AROS_UFHA(type, name, reg) type name
#define AROS_UFH2S(type, name, a, b) static type name(a, b)
#define AROS_UFH3(type, name, a, b, c) type name(a, b, c)
#define AROS_UFH3S(type, name, a, b, c) static type name(a, b, c)
#define AROS_USERFUNC_INIT
#define AROS_USERFUNC_EXIT
#define AROS_UFCA(type, value, reg) (value)
#define AROS_UFC3(type, entry, hook, object, message) \
    (((IPTR (*)(struct Hook *, APTR, IPTR))(entry)) \
     ((struct Hook *)(hook), (APTR)(object), (IPTR)(message)))
#define AROS_UFC3NR(type, entry, hook, object, message) \
    ((void)AROS_UFC3(type, entry, hook, object, message))
