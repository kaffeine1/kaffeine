#pragma once
/* one task on the host */
#define AROS_ATOMIC_OR(var, mask) ((var) |= (mask))
#define AROS_ATOMIC_AND(var, mask) ((var) &= (mask))
