#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <proto/exec.h>
#include "kaffeine_intern.h"

extern "C" APTR AllocMem(ULONG size, ULONG flags)
{
    APTR p = std::malloc(size);
    if (p && (flags & MEMF_CLEAR)) std::memset(p, 0, size);
    return p;
}
extern "C" void FreeMem(APTR p, ULONG) { std::free(p); }
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *) { std::abort(); }
extern "C" ULONG Kaffeine_OpenM68KLibrary(KaffeineContext *, void *, const char *, ULONG) { std::abort(); }
extern "C" BOOL Kaffeine_CloseM68KLibrary(KaffeineContext *, void *, ULONG) { return 0; }

int main(int argc, char **argv)
{
    for (ULONG n : {1000u, 4000u, 16000u, 64000u})
    {
        KaffeineContext *ctx = Kaffeine_CreateContext(nullptr);
        std::vector<ULONG> live;
        auto t0 = std::chrono::steady_clock::now();
        for (ULONG i = 0; i < n; i++)
            live.push_back(Kaffeine_HeapAlloc(ctx, 24 + (i % 5) * 8, 0));
        auto t1 = std::chrono::steady_clock::now();
        for (ULONG i = 0; i < n; i += 2)
            Kaffeine_HeapFree(ctx, live[i], 0);
        for (ULONG i = 0; i < n; i += 2)
            live[i] = Kaffeine_HeapAlloc(ctx, 24, 0);
        auto t2 = std::chrono::steady_clock::now();
        for (ULONG a : live)
            Kaffeine_HeapFree(ctx, a, 0);
        auto t3 = std::chrono::steady_clock::now();
        auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        std::printf("%6u live blocks: alloc %8.1f ms, free/realloc half %8.1f ms, free all %8.1f ms\n",
                    n, ms(t0, t1), ms(t1, t2), ms(t2, t3));
        Kaffeine_DestroyContext(ctx);
    }
    return 0;
}
