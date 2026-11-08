/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Michele Dipace (@kaffeine1)

    The registry of the screens that programs opened (kaffeine_owned_screens.c),
    compiled from its source, over a model of Exec semaphores: each gate
    counts its shared holders and whether it is owned exclusively, and the
    registry lock whether it is held. One host task cannot wait, so an
    ObtainSemaphore() that would have waited is counted instead: the cases
    show exactly when a closing program waits, and that it never waits, nor
    takes or tries a gate, in the wrong lock order.
*/

#include <cstdio>
#include <map>

#include "kaffeine_intern.h"
#include "kaffeine_owned_screens.h"

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Semaphore { int shared = 0; bool exclusive = false; int inits = 0; };
static std::map<const SignalSemaphore *, Semaphore> semaphores;
static const SignalSemaphore *registry_lock;
static int waits;      /* ObtainSemaphore() calls that would have waited */
static int disorder;   /* a gate taken under the registry lock, or tried outside it */

static bool registry_held() { return registry_lock && semaphores[registry_lock].exclusive; }

extern "C" void InitSemaphore(SignalSemaphore *s)
{
    int inits = semaphores[s].inits;
    semaphores[s] = Semaphore();
    semaphores[s].inits = inits + 1;
}
extern "C" void ObtainSemaphore(SignalSemaphore *s)
{
    Semaphore &m = semaphores[s];
    if (s != registry_lock && registry_held()) ++disorder;
    if (m.exclusive || m.shared) ++waits;
    m.exclusive = true;
}
extern "C" void ReleaseSemaphore(SignalSemaphore *s)
{
    Semaphore &m = semaphores[s];
    if (m.exclusive) m.exclusive = false;
    else { CHECK(m.shared > 0); --m.shared; }
}
extern "C" unsigned int AttemptSemaphoreShared(SignalSemaphore *s)
{
    Semaphore &m = semaphores[s];
    if (s == registry_lock || !registry_held()) ++disorder;
    if (m.exclusive) return 0;
    ++m.shared;
    return 1;
}

struct Fixture
{
    KaffeineLibBase base {};
    const KaffeineOwnedScreenOps &ops = Kaffeine_OwnedScreenOps;
    KaffeineOwnedScreens &r = base.owned_screens;

    Fixture()
    {
        semaphores.clear();
        waits = disorder = 0;
        registry_lock = &base.owned_screens.lock;
        Kaffeine_OwnedScreensInit(&base);
    }
    Semaphore &gate(LONG slot) { return semaphores[&r.slot[slot].gate]; }
    LONG open(Screen *native, ULONG *generation)
    {
        LONG slot = ops.reserve(&base, generation);
        if (slot >= 0) ops.bind(&base, slot, native);
        return slot;
    }
};

/* The registry compares natives and never reads them */
static char natives[KAFFEINE_MAX_OWNED_SCREENS + 1][16];
static Screen *screen(int i) { return reinterpret_cast<Screen *>(natives[i]); }

/* A slot for each screen, with a generation never given again; a full
   registry gives none; RESERVED slots are no screen's yet, and keep every
   generation-zero screen from counting as foreign. */
static void reserve_and_bind()
{
    Fixture f;
    ULONG g1, g2, g;
    LONG s1, s2, s;

    CHECK(f.r.ready && f.ops.epoch(&f.base) == 0);
    for (int i = 0; i < KAFFEINE_MAX_OWNED_SCREENS; ++i) CHECK(f.gate(i).inits == 1);
    s1 = f.ops.reserve(&f.base, &g1);
    CHECK(s1 >= 0 && g1 == 1 && f.r.slot[s1].state == KAFFEINE_OWNED_RESERVED);
    CHECK(!f.ops.generation(&f.base, screen(0)));
    CHECK(!f.ops.foreign(&f.base, screen(1)));
    f.ops.bind(&f.base, s1, screen(0));
    CHECK(f.ops.generation(&f.base, screen(0)) == g1);
    CHECK(f.ops.foreign(&f.base, screen(1)) && !f.ops.foreign(&f.base, screen(0)));
    s2 = f.ops.reserve(&f.base, &g2);
    CHECK(s2 >= 0 && s2 != s1 && g2 == 2);
    f.ops.unreserve(&f.base, s2);
    CHECK(f.r.slot[s2].state == KAFFEINE_OWNED_FREE && !f.r.slot[s2].generation);
    s = f.ops.reserve(&f.base, &g);
    CHECK(s == s2 && g == 3);
    f.ops.unreserve(&f.base, s);
    for (int i = 1; i < KAFFEINE_MAX_OWNED_SCREENS; ++i) CHECK(f.open(screen(i), &g) >= 0);
    CHECK(f.ops.reserve(&f.base, &g) == -1 && !g);
    CHECK(f.ops.epoch(&f.base) != 0 && !waits && !disorder);
}

/* An input hold takes the gate shared without waiting, only for an OPEN
   slot of exactly that generation; drop gives it back. */
static void hold_and_drop()
{
    Fixture f;
    ULONG g;
    LONG slot = f.open(screen(0), &g), held = -1, other = -1;

    CHECK(f.ops.hold(&f.base, screen(0), g, &held) && held == slot && f.gate(slot).shared == 1);
    CHECK(f.ops.hold(&f.base, screen(0), g, &other) && other == slot && f.gate(slot).shared == 2);
    CHECK(!f.ops.hold(&f.base, screen(0), g + 1, &other) && other == -1);
    CHECK(!f.ops.hold(&f.base, screen(1), g, &other) && !f.ops.hold(&f.base, screen(0), 0, &other));
    f.ops.drop(&f.base, held);
    f.ops.drop(&f.base, slot);
    CHECK(f.gate(slot).shared == 0 && !f.gate(slot).exclusive && !waits && !disorder);
}

/* CloseScreen(): CLOSING keeps every new hold out, the drain gets the gate
   once the holds admitted before have dropped, and the slot is free after
   a close, or OPEN again with its generation after Intuition refused. */
static void close_drains()
{
    Fixture f;
    ULONG g, epoch;
    LONG slot = f.open(screen(0), &g), held, late;

    CHECK(f.ops.hold(&f.base, screen(0), g, &held));
    epoch = f.ops.epoch(&f.base);
    CHECK(f.ops.closing(&f.base, slot, g) && f.r.slot[slot].state == KAFFEINE_OWNED_CLOSING);
    CHECK(f.ops.epoch(&f.base) != epoch);
    CHECK(!f.ops.hold(&f.base, screen(0), g, &late) && late == -1 && f.gate(slot).shared == 1);
    /* still the same screen, of the same generation */
    CHECK(f.ops.generation(&f.base, screen(0)) == g && !f.ops.foreign(&f.base, screen(0)));
    f.ops.drop(&f.base, held);
    f.ops.drain(&f.base, slot);
    CHECK(f.gate(slot).exclusive && !waits && !disorder);
    f.ops.finish(&f.base, slot, FALSE, TRUE);
    CHECK(f.r.slot[slot].state == KAFFEINE_OWNED_OPEN && f.r.slot[slot].generation == g);
    CHECK(!f.gate(slot).exclusive && f.ops.hold(&f.base, screen(0), g, &held));
    f.ops.drop(&f.base, held);
    CHECK(f.ops.closing(&f.base, slot, g));
    f.ops.drain(&f.base, slot);
    f.ops.finish(&f.base, slot, TRUE, FALSE);
    CHECK(f.r.slot[slot].state == KAFFEINE_OWNED_FREE && !f.r.slot[slot].generation && !f.r.slot[slot].native);
    CHECK(!f.ops.generation(&f.base, screen(0)) && f.ops.foreign(&f.base, screen(0)));
    CHECK(!f.ops.closing(&f.base, slot, g) && !waits && !disorder);
    /* the gate was never set up again */
    CHECK(f.gate(slot).inits == 1);
}

/* A drain that comes while a hold is still in flight is the one place
   where a closing program waits: for exactly that hold. */
static void drain_waits_for_flight()
{
    Fixture f;
    ULONG g;
    LONG slot = f.open(screen(0), &g), held;

    CHECK(f.ops.hold(&f.base, screen(0), g, &held));
    CHECK(f.ops.closing(&f.base, slot, g));
    f.ops.drain(&f.base, slot);
    CHECK(waits == 1 && !disorder);
}

/* The end of a program: a screen left open keeps its slot CLOSING, with
   its generation, so that it is never another screen's; a second CLOSING
   is the same; a finish without reopen keeps it CLOSING. */
static void owner_exit()
{
    Fixture f;
    ULONG g;
    LONG slot = f.open(screen(0), &g), held;

    CHECK(f.ops.closing(&f.base, slot, g) && f.ops.closing(&f.base, slot, g));
    f.ops.drain(&f.base, slot);
    f.ops.finish(&f.base, slot, FALSE, FALSE);
    CHECK(f.r.slot[slot].state == KAFFEINE_OWNED_CLOSING && f.ops.generation(&f.base, screen(0)) == g);
    CHECK(!f.ops.hold(&f.base, screen(0), g, &held) && !waits && !disorder);
    /* a slot that is free again serves the next screen with its gate as it was */
    {
        ULONG g2, g3;
        LONG s2 = f.open(screen(1), &g2), s3;
        CHECK(f.ops.closing(&f.base, s2, g2));
        f.ops.drain(&f.base, s2);
        f.ops.finish(&f.base, s2, TRUE, FALSE);
        s3 = f.open(screen(2), &g3);
        CHECK(s3 == s2 && g3 > g2 && f.gate(s3).inits == 1 && !f.gate(s3).shared && !f.gate(s3).exclusive);
    }
}

/* Two slots of one native, the older one left CLOSING by a program that
   ended: the newer generation names the screen. */
static void newest_generation()
{
    Fixture f;
    ULONG g1, g2;
    LONG s1 = f.open(screen(0), &g1), s2;

    CHECK(f.ops.closing(&f.base, s1, g1));
    s2 = f.open(screen(0), &g2);
    CHECK(s2 != s1 && g2 > g1 && f.ops.generation(&f.base, screen(0)) == g2);
}

int main()
{
    reserve_and_bind();
    hold_and_drop();
    close_drains();
    drain_waits_for_flight();
    owner_exit();
    newest_generation();
    std::printf("owned screens: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
