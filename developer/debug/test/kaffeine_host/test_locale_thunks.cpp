/*
 * Host unit tests for rom/kaffeine/kaffeine_thunks_locale.c.
 *
 * The production C files are compiled by the Makefile and linked into this
 * binary. Every vector is invoked through Kaffeine_DispatchLibraryCall() at
 * its literal locale.library LVO, with poisoned generated fallbacks so an
 * uncopied or misregistered slot fails immediately.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include <exec/memory.h>
#include <libraries/locale.h>
#include <proto/exec.h>
#include <proto/locale.h>

#include "kaffeine_cpu.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_thunks.h"
#include "kaffeine_thunks_locale.h"
#include "kaffeine_thunks_memory.h"

static int g_failures = 0;

#define CHECK(cond, ...)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL [%s:%d]: ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                          \
            std::fprintf(stderr, "\n");                                 \
            g_failures++;                                               \
        }                                                               \
    } while (0)

/* Literal positive LVO offsets from NDK 3.2 locale_lib.fd and locale.conf. */
enum : ULONG {
    LVO_CloseCatalog  =  36,
    LVO_CloseLocale   =  42,
    LVO_ConvToLower   =  48,
    LVO_ConvToUpper   =  54,
    LVO_FormatDate    =  60,
    LVO_FormatString  =  66,
    LVO_GetCatalogStr =  72,
    LVO_GetLocaleStr  =  78,
    LVO_IsAlNum       =  84,
    LVO_IsAlpha       =  90,
    LVO_IsCntrl       =  96,
    LVO_IsDigit       = 102,
    LVO_IsGraph       = 108,
    LVO_IsLower       = 114,
    LVO_IsPrint       = 120,
    LVO_IsPunct       = 126,
    LVO_IsSpace       = 132,
    LVO_IsUpper       = 138,
    LVO_IsXDigit      = 144,
    LVO_OpenCatalogA  = 150,
    LVO_OpenLocale    = 156,
    LVO_ParseDate     = 162,
    LVO_StrConvert    = 174,
    LVO_StrnCmp       = 180,

    LVO_AllocMem      = 198,
    LVO_FreeMem       = 210,
    LVO_AvailMem      = 216,
};

/* Native heap bookkeeping for kaffeine_memory.c compiled with MEMORY_CPPFLAGS. */
static std::map<APTR, ULONG> g_native_blocks;

extern "C" APTR AllocMem(ULONG size, ULONG flags)
{
    APTR block = std::malloc(size ? size : 1);
    if (block) {
        std::memset(block, (flags & MEMF_CLEAR) ? 0 : 0xCD, size ? size : 1);
        g_native_blocks.emplace(block, size);
    }
    return block;
}

extern "C" void FreeMem(APTR block, ULONG size)
{
    auto it = g_native_blocks.find(block);
    CHECK(it != g_native_blocks.end() && it->second == size,
          "FreeMem mismatch for block %p size %u", block, size);
    if (it != g_native_blocks.end()) {
        g_native_blocks.erase(it);
        std::free(block);
    }
}

/* Unused collaborators of HUNK loading in kaffeine_memory.c. */
extern "C" ULONG Kaffeine_OpenLibrary(KaffeineContext *, const char *)
{
    std::abort();
}

extern "C" ULONG Kaffeine_OpenM68KLibrary(KaffeineContext *, void *, const char *, ULONG)
{
    std::abort();
}

extern "C" BOOL Kaffeine_CloseM68KLibrary(KaffeineContext *, void *, ULONG)
{
    return 0;
}

/* Controlled native stubs and call counters for locale.library. */
struct FakeNativeLocale {
    struct Locale loc;
    bool in_use;
    char requested_name[64];
};

static FakeNativeLocale g_native_pool[16];
static bool g_force_open_locale_null = false;
static bool g_null_optional_strings = false;
static int g_open_locale_calls = 0;
static int g_close_locale_calls = 0;
static int g_open_catalog_calls = 0;
static int g_close_catalog_calls = 0;
static int g_get_catalog_str_calls = 0;
static int g_get_locale_str_calls = 0;
static int g_conv_to_lower_calls = 0;
static int g_conv_to_upper_calls = 0;
static int g_is_calls[11];
static int g_str_convert_calls = 0;
static int g_strncmp_calls = 0;
static const struct Locale *g_last_native_locale = nullptr;

/* AROS keeps the grouping vectors as fixed UBYTE[10] arrays; a country
   file can fill one completely, without a zero, as kMonFracGrouping. */
static const UBYTE kGrouping[10] = { 3, 2, 0 };
static const UBYTE kFracGrouping[10] = { 4, 0 };
static const UBYTE kMonGrouping[10] = { 3, 0 };
static const UBYTE kMonFracGrouping[10] = { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };

static void populate_fake_locale(struct Locale *loc)
{
    std::memset(loc, 0, sizeof(*loc));
    loc->loc_LocaleName = const_cast<STRPTR>("deutsch_deutschland.locale");
    loc->loc_LanguageName = const_cast<STRPTR>("deutsch.language");
    loc->loc_PrefLanguages[0] = const_cast<STRPTR>("deutsch");
    loc->loc_PrefLanguages[1] = const_cast<STRPTR>("englisch");
    loc->loc_PrefLanguages[2] = const_cast<STRPTR>("franzoesisch");
    for (int i = 3; i < 10; i++)
        loc->loc_PrefLanguages[i] = nullptr;

    loc->loc_Flags = 0x12345678UL;
    loc->loc_CodeSet = 4UL;
    loc->loc_CountryCode = 0x44455500UL;
    loc->loc_TelephoneCode = 49UL;
    loc->loc_GMTOffset = -60;
    loc->loc_MeasuringSystem = MS_IMPERIAL;
    loc->loc_CalendarType = CT_7MON;
    loc->loc_Reserved0[0] = 0xAA;
    loc->loc_Reserved0[1] = 0x55;

    loc->loc_DateTimeFormat = const_cast<STRPTR>("%A %d %B %Y %H:%M:%S");
    loc->loc_DateFormat = const_cast<STRPTR>("%A %d %B %Y");
    loc->loc_TimeFormat = const_cast<STRPTR>("%H:%M:%S");
    loc->loc_ShortDateTimeFormat = const_cast<STRPTR>("%d.%m.%Y %H:%M");
    loc->loc_ShortDateFormat = const_cast<STRPTR>("%d.%m.%Y");
    loc->loc_ShortTimeFormat = const_cast<STRPTR>("%H:%M");

    loc->loc_DecimalPoint = const_cast<STRPTR>(",");
    loc->loc_GroupSeparator = const_cast<STRPTR>(".");
    loc->loc_FracGroupSeparator = const_cast<STRPTR>(" ");
    loc->loc_Grouping = const_cast<UBYTE *>(kGrouping);
    loc->loc_FracGrouping = const_cast<UBYTE *>(kFracGrouping);

    loc->loc_MonDecimalPoint = const_cast<STRPTR>(";");
    loc->loc_MonGroupSeparator = const_cast<STRPTR>("'");
    loc->loc_MonFracGroupSeparator = const_cast<STRPTR>("_");
    loc->loc_MonGrouping = const_cast<UBYTE *>(kMonGrouping);
    loc->loc_MonFracGrouping = const_cast<UBYTE *>(kMonFracGrouping);
    loc->loc_MonFracDigits = 2;
    loc->loc_MonIntFracDigits = 3;
    loc->loc_Reserved1[0] = 0x11;
    loc->loc_Reserved1[1] = 0x22;

    loc->loc_MonCS = const_cast<STRPTR>("EUR");
    loc->loc_MonSmallCS = const_cast<STRPTR>("ct");
    loc->loc_MonIntCS = const_cast<STRPTR>("EUR ");

    loc->loc_MonPositiveSign = const_cast<STRPTR>("+");
    loc->loc_MonPositiveSpaceSep = SS_SPACE;
    loc->loc_MonPositiveSignPos = SP_PREC_ALL;
    loc->loc_MonPositiveCSPos = CSP_SUCCEEDS;
    loc->loc_Reserved2 = 0x33;

    loc->loc_MonNegativeSign = const_cast<STRPTR>("-");
    loc->loc_MonNegativeSpaceSep = SS_NOSPACE;
    loc->loc_MonNegativeSignPos = SP_SUCC_CURR;
    loc->loc_MonNegativeCSPos = CSP_PRECEDES;
    loc->loc_Reserved3 = 0x44;

    if (g_null_optional_strings) {
        loc->loc_FracGroupSeparator = nullptr;
        loc->loc_MonSmallCS = nullptr;
    }
}

static void reset_native_stubs()
{
    std::memset(g_native_pool, 0, sizeof(g_native_pool));
    g_force_open_locale_null = false;
    g_null_optional_strings = false;
    g_open_locale_calls = 0;
    g_close_locale_calls = 0;
    g_open_catalog_calls = 0;
    g_close_catalog_calls = 0;
    g_get_catalog_str_calls = 0;
    g_get_locale_str_calls = 0;
    g_conv_to_lower_calls = 0;
    g_conv_to_upper_calls = 0;
    std::memset(g_is_calls, 0, sizeof(g_is_calls));
    g_str_convert_calls = 0;
    g_strncmp_calls = 0;
    g_last_native_locale = nullptr;
}

static int active_native_locales()
{
    int count = 0;
    for (const auto &entry : g_native_pool) {
        if (entry.in_use) count++;
    }
    return count;
}

static bool is_valid_native_locale(const struct Locale *locale)
{
    for (const auto &entry : g_native_pool) {
        if (entry.in_use && &entry.loc == locale)
            return true;
    }
    return false;
}

extern "C" {

void FormatDate(const struct Locale *, CONST_STRPTR, const struct DateStamp *,
                const struct Hook *) { std::abort(); }
BOOL ParseDate(const struct Locale *, struct DateStamp *, CONST_STRPTR,
               const struct Hook *) { std::abort(); }

void CloseCatalog(struct Catalog *catalog)
{
    (void)catalog;
    g_close_catalog_calls++;
}

struct Catalog *OpenCatalogA(struct Locale *locale, CONST_STRPTR name,
                             const void *tags)
{
    (void)locale;
    (void)name;
    (void)tags;
    g_open_catalog_calls++;
    return nullptr;
}

CONST_STRPTR GetCatalogStr(struct Catalog *catalog, LONG stringNum,
                           CONST_STRPTR defaultString)
{
    (void)catalog;
    (void)stringNum;
    g_get_catalog_str_calls++;
    return defaultString;
}

struct Locale *OpenLocale(CONST_STRPTR name)
{
    g_open_locale_calls++;
    if (g_force_open_locale_null)
        return nullptr;
    if (name && std::strcmp(name, "nonexistent.locale") == 0)
        return nullptr;

    for (auto &entry : g_native_pool) {
        if (!entry.in_use) {
            entry.in_use = true;
            populate_fake_locale(&entry.loc);
            if (name) {
                std::snprintf(entry.requested_name,
                              sizeof(entry.requested_name), "%s", name);
                entry.loc.loc_LocaleName = entry.requested_name;
            }
            return &entry.loc;
        }
    }
    return nullptr;
}

void CloseLocale(struct Locale *locale)
{
    g_close_locale_calls++;
    CHECK(is_valid_native_locale(locale),
          "CloseLocale called with invalid or already-closed native pointer %p",
          (void *)locale);
    for (auto &entry : g_native_pool) {
        if (entry.in_use && &entry.loc == locale) {
            entry.in_use = false;
            return;
        }
    }
}

ULONG ConvToLower(struct Locale *locale, ULONG character)
{
    g_conv_to_lower_calls++;
    g_last_native_locale = locale;
    CHECK(is_valid_native_locale(locale), "ConvToLower invalid native locale");
    if (character >= 'A' && character <= 'Z')
        return character + ('a' - 'A');
    return character;
}

ULONG ConvToUpper(struct Locale *locale, ULONG character)
{
    g_conv_to_upper_calls++;
    g_last_native_locale = locale;
    CHECK(is_valid_native_locale(locale), "ConvToUpper invalid native locale");
    if (character >= 'a' && character <= 'z')
        return character - ('a' - 'A');
    return character;
}

CONST_STRPTR GetLocaleStr(struct Locale *locale, ULONG stringNum)
{
    g_get_locale_str_calls++;
    g_last_native_locale = locale;
    CHECK(is_valid_native_locale(locale), "GetLocaleStr invalid native locale");
    switch (stringNum) {
        case DAY_1:      return "Sunday";
        case DAY_2:      return "Monday";
        case MON_1:      return "January";
        case YESSTR:     return "Yes";
        case NOSTR:      return "No";
        case FUTURESTR:  return "Future";
        case 70:         return "NDK-Slot-70";
        default:         return nullptr;
    }
}

BOOL IsAlNum(struct Locale *locale, ULONG c)
{
    g_is_calls[0]++;
    g_last_native_locale = locale;
    CHECK(is_valid_native_locale(locale), "IsAlNum invalid native locale");
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

BOOL IsAlpha(struct Locale *locale, ULONG c)
{
    g_is_calls[1]++;
    g_last_native_locale = locale;
    CHECK(is_valid_native_locale(locale), "IsAlpha invalid native locale");
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

BOOL IsCntrl(struct Locale *locale, ULONG c)
{
    g_is_calls[2]++;
    g_last_native_locale = locale;
    CHECK(is_valid_native_locale(locale), "IsCntrl invalid native locale");
    return c < 0x20 || c == 0x7F;
}

BOOL IsDigit(struct Locale *locale, ULONG c)
{
    g_is_calls[3]++;
    g_last_native_locale = locale;
    CHECK(is_valid_native_locale(locale), "IsDigit invalid native locale");
    return c >= '0' && c <= '9';
}

BOOL IsGraph(struct Locale *locale, ULONG c)
{
    g_is_calls[4]++;
    g_last_native_locale = locale;
    CHECK(is_valid_native_locale(locale), "IsGraph invalid native locale");
    return c > 0x20 && c < 0x7F;
}

BOOL IsLower(struct Locale *locale, ULONG c)
{
    g_is_calls[5]++;
    g_last_native_locale = locale;
    CHECK(is_valid_native_locale(locale), "IsLower invalid native locale");
    return c >= 'a' && c <= 'z';
}

BOOL IsPrint(struct Locale *locale, ULONG c)
{
    g_is_calls[6]++;
    g_last_native_locale = locale;
    CHECK(is_valid_native_locale(locale), "IsPrint invalid native locale");
    return c >= 0x20 && c < 0x7F;
}

BOOL IsPunct(struct Locale *locale, ULONG c)
{
    g_is_calls[7]++;
    g_last_native_locale = locale;
    CHECK(is_valid_native_locale(locale), "IsPunct invalid native locale");
    return c == '!' || c == '.' || c == ',';
}

BOOL IsSpace(struct Locale *locale, ULONG c)
{
    g_is_calls[8]++;
    g_last_native_locale = locale;
    CHECK(is_valid_native_locale(locale), "IsSpace invalid native locale");
    return c == ' ' || c == '\t' || c == '\n';
}

BOOL IsUpper(struct Locale *locale, ULONG c)
{
    g_is_calls[9]++;
    g_last_native_locale = locale;
    CHECK(is_valid_native_locale(locale), "IsUpper invalid native locale");
    return c >= 'A' && c <= 'Z';
}

BOOL IsXDigit(struct Locale *locale, ULONG c)
{
    g_is_calls[10]++;
    g_last_native_locale = locale;
    CHECK(is_valid_native_locale(locale), "IsXDigit invalid native locale");
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
           (c >= 'a' && c <= 'f');
}

ULONG StrConvert(struct Locale *locale, CONST_STRPTR string, APTR buffer,
                 ULONG bufferSize, ULONG type)
{
    g_str_convert_calls++;
    g_last_native_locale = locale;
    CHECK(is_valid_native_locale(locale), "StrConvert invalid native locale");
    if (type > SC_COLLATE2 || !bufferSize || !buffer)
        return 0;

    UBYTE *dst = static_cast<UBYTE *>(buffer);
    ULONG count = 0;
    while (--bufferSize && *string) {
        UBYTE ch = static_cast<UBYTE>(*string++);
        if (ch >= 'a' && ch <= 'z')
            ch = static_cast<UBYTE>(ch - ('a' - 'A'));
        *dst++ = ch;
        count++;
    }
    *dst = '\0';
    return count;
}

LONG StrnCmp(struct Locale *locale, CONST_STRPTR s1, CONST_STRPTR s2,
             LONG length, ULONG type)
{
    g_strncmp_calls++;
    g_last_native_locale = locale;
    CHECK(is_valid_native_locale(locale), "StrnCmp invalid native locale");
    if (!length || type > SC_COLLATE2)
        return 0;

    while (length--) {
        UBYTE c1 = static_cast<UBYTE>(*s1);
        UBYTE c2 = static_cast<UBYTE>(*s2);
        if (c1 >= 'a' && c1 <= 'z') c1 = static_cast<UBYTE>(c1 - ('a' - 'A'));
        if (c2 >= 'a' && c2 <= 'z') c2 = static_cast<UBYTE>(c2 - ('a' - 'A'));
        if (c1 != c2)
            return static_cast<LONG>(c1) - static_cast<LONG>(c2);
        if (!*s1)
            return 0;
        s1++;
        s2++;
    }
    return 0;
}

} /* extern "C" */

struct TestCPU {
    ULONG d[8] = {};
    ULONG a[8] = {};
    UWORD sr = 0xA715;
    ULONG pc = 0x43210;
};

static ULONG get_d(void *s, int n) { return static_cast<TestCPU *>(s)->d[n]; }
static ULONG get_a(void *s, int n) { return static_cast<TestCPU *>(s)->a[n]; }
static void set_d(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->d[n] = v; }
static void set_a(void *s, int n, ULONG v) { static_cast<TestCPU *>(s)->a[n] = v; }
static UWORD get_sr(void *s) { return static_cast<TestCPU *>(s)->sr; }
static void set_sr(void *s, UWORD v) { static_cast<TestCPU *>(s)->sr = v; }
static ULONG get_pc(void *s) { return static_cast<TestCPU *>(s)->pc; }
static void set_pc(void *s, ULONG v) { static_cast<TestCPU *>(s)->pc = v; }
static void execute(void *) { std::abort(); }

static const KaffeineCPUOps kCpuOps = {
    get_d, get_a, set_d, set_a, get_sr, set_sr, get_pc, set_pc, execute, nullptr
};

/* Poisoned generated fallback table: every manual vector must override its
   entry here, and unreplaced hook vectors stay distinguishable. */
static int g_poison_calls = 0;

static IPTR poisoned_gen_thunk(struct KaffeineContext *ctx, void *cpu)
{
    (void)ctx;
    (void)cpu;
    g_poison_calls++;
    return (IPTR)0xDEADCAFEUL;
}

static const struct M68KThunkEntry kPoisonedLocaleGen[] = {
    {  30, poisoned_gen_thunk },
    {  36, poisoned_gen_thunk },
    {  42, poisoned_gen_thunk },
    {  48, poisoned_gen_thunk },
    {  54, poisoned_gen_thunk },
    {  60, poisoned_gen_thunk },
    {  66, poisoned_gen_thunk },
    {  72, poisoned_gen_thunk },
    {  78, poisoned_gen_thunk },
    {  84, poisoned_gen_thunk },
    {  90, poisoned_gen_thunk },
    {  96, poisoned_gen_thunk },
    { 102, poisoned_gen_thunk },
    { 108, poisoned_gen_thunk },
    { 114, poisoned_gen_thunk },
    { 120, poisoned_gen_thunk },
    { 126, poisoned_gen_thunk },
    { 132, poisoned_gen_thunk },
    { 138, poisoned_gen_thunk },
    { 144, poisoned_gen_thunk },
    { 150, poisoned_gen_thunk },
    { 156, poisoned_gen_thunk },
    { 162, poisoned_gen_thunk },
    { 168, poisoned_gen_thunk },
    { 174, poisoned_gen_thunk },
    { 180, poisoned_gen_thunk },
    { 186, poisoned_gen_thunk },
    { 192, poisoned_gen_thunk },
    { 198, poisoned_gen_thunk },
};
static const ULONG kPoisonedLocaleGenCount =
    sizeof(kPoisonedLocaleGen) / sizeof(kPoisonedLocaleGen[0]);

static const struct M68KThunkEntry kExecMemoryManual[] = {
    KAFFEINE_EXEC_MEMORY_THUNKS
};
static const ULONG kExecMemoryManualCount =
    sizeof(kExecMemoryManual) / sizeof(kExecMemoryManual[0]);

struct RegSnapshot {
    ULONG d[8];
    ULONG a[8];
};

static RegSnapshot sentinel_regs()
{
    RegSnapshot r{};
    for (int i = 0; i < 8; i++) {
        r.d[i] = 0xD0000000UL | (static_cast<ULONG>(i) << 20) | 0x000ACE00UL | static_cast<ULONG>(i);
        r.a[i] = 0xA0000000UL | (static_cast<ULONG>(i) << 20) | 0x000BEE00UL | static_cast<ULONG>(i);
    }
    return r;
}

struct LocaleFixture {
    static constexpr ULONG kStack = 0x200;
    static constexpr ULONG kReturnPc = 0x76543210UL;
    static constexpr ULONG kExecBase = 0x10000UL;
    static constexpr ULONG kLocaleBase = 0x20000UL;

    /* Members are initialized in declaration order: the native block count
       must be taken before this fixture allocates its own context, and
       fixtures may nest. */
    size_t native_blocks_before;
    KaffeineContext *ctx_ptr;
    KaffeineContext &ctx;
    TestCPU state{};
    KaffeineCPU cpu = { &kCpuOps, &state };

    explicit LocaleFixture(ULONG mem_size = 64 * 1024,
                           ULONG heap_start = 0x4000,
                           ULONG heap_end = 0xC000)
        : native_blocks_before((reset_native_stubs(),
                                g_poison_calls = 0,
                                g_native_blocks.size())),
          ctx_ptr(static_cast<KaffeineContext *>(
              AllocMem(sizeof(KaffeineContext), MEMF_CLEAR))),
          ctx(*ctx_ptr)
    {
        ctx.mem_size = mem_size;
        ctx.mem = static_cast<UBYTE *>(AllocMem(ctx.mem_size + KAFFEINE_MEM_GUARD, MEMF_CLEAR));
        ctx.heap_start = heap_start;
        ctx.heap_end = heap_end;
        ctx.heap_free = heap_start;
        ctx.running = TRUE;

        bool ok = Kaffeine_HeapInit(&ctx) != FALSE;
        CHECK(ok, "Kaffeine_HeapInit failed");

        ctx.num_libs = 2;
        auto &exec_lib = ctx.libs[0];
        exec_lib.m68k_addr = kExecBase;
        exec_lib.jt_start = kExecBase - 6 * 512;
        std::strcpy(exec_lib.name, "exec.library");
        exec_lib.thunks = kExecMemoryManual;
        exec_lib.num_thunks = kExecMemoryManualCount;
        exec_lib.gen_thunks = nullptr;
        exec_lib.num_gen_thunks = 0;

        auto &loc_lib = ctx.libs[1];
        loc_lib.m68k_addr = kLocaleBase;
        loc_lib.jt_start = kLocaleBase - 6 * 512;
        std::strcpy(loc_lib.name, "locale.library");
        loc_lib.thunks = kaffeine_thunks_locale_manual;
        loc_lib.num_thunks = kaffeine_thunks_locale_manual_count;
        loc_lib.gen_thunks = kPoisonedLocaleGen;
        loc_lib.num_gen_thunks = kPoisonedLocaleGenCount;
    }

    ~LocaleFixture()
    {
        Kaffeine_LocaleCleanup(&ctx);
        Kaffeine_DestroyContext(ctx_ptr);
        CHECK(g_native_blocks.size() == native_blocks_before,
              "Leaked %zu native heap metadata block(s)",
              g_native_blocks.size() - native_blocks_before);
    }

    ULONG dispatch_call(ULONG lib_base, ULONG lvo, RegSnapshot &regs,
                        bool void_call, const char *label)
    {
        regs.a[6] = lib_base;
        regs.a[7] = kStack;
        for (int i = 0; i < 8; i++) {
            state.d[i] = regs.d[i];
            state.a[i] = regs.a[i];
        }
        state.sr = 0xA715;
        state.pc = 0x87654321UL;
        m68k_write32(&ctx, kStack, kReturnPc);

        int poison_before = g_poison_calls;
        KaffeineDispatchResult disp_res{};
        BOOL handled = Kaffeine_DispatchLibraryCall(&ctx, &cpu, lib_base - lvo,
                                                   &disp_res);
        CHECK(handled == TRUE, "%s LVO -%u was not handled", label, lvo);
        CHECK(disp_res.return_pc == kReturnPc && !disp_res.supervisor &&
              state.a[7] == kStack + 4,
              "%s LVO -%u did not pop return address cleanly", label, lvo);
        CHECK(g_poison_calls == poison_before,
              "%s LVO -%u fell back to poisoned generated thunk", label, lvo);

        for (int i = void_call ? 0 : 1; i < 8; i++) {
            CHECK(state.d[i] == regs.d[i],
                  "%s clobbered D%d: got 0x%08x, expected 0x%08x",
                  label, i, state.d[i], regs.d[i]);
        }
        for (int i = 0; i < 7; i++) {
            CHECK(state.a[i] == regs.a[i],
                  "%s clobbered A%d: got 0x%08x, expected 0x%08x",
                  label, i, state.a[i], regs.a[i]);
        }
        CHECK(state.sr == 0xA715 && state.pc == 0x87654321UL,
              "%s clobbered SR or PC", label);
        return state.d[0];
    }

    ULONG call_locale(ULONG lvo, RegSnapshot &regs, bool void_call = false,
                      const char *label = "locale call")
    {
        return dispatch_call(kLocaleBase, lvo, regs, void_call, label);
    }

    ULONG call_exec(ULONG lvo, RegSnapshot &regs, bool void_call = false,
                    const char *label = "exec call")
    {
        return dispatch_call(kExecBase, lvo, regs, void_call, label);
    }

    ULONG avail_mem()
    {
        RegSnapshot r = sentinel_regs();
        r.d[1] = MEMF_PUBLIC;
        return call_exec(LVO_AvailMem, r, false, "AvailMem");
    }

    void write_cstr(ULONG addr, const char *s)
    {
        std::memcpy(ctx.mem + addr, s, std::strlen(s) + 1);
    }
};

static void test_table()
{
    const ULONG want[] = {
        LVO_CloseCatalog, LVO_CloseLocale, LVO_ConvToLower, LVO_ConvToUpper,
        LVO_FormatDate, LVO_FormatString,
        LVO_GetCatalogStr, LVO_GetLocaleStr,
        LVO_IsAlNum, LVO_IsAlpha, LVO_IsCntrl, LVO_IsDigit, LVO_IsGraph,
        LVO_IsLower, LVO_IsPrint, LVO_IsPunct, LVO_IsSpace, LVO_IsUpper,
        LVO_IsXDigit,
        LVO_OpenCatalogA, LVO_OpenLocale, LVO_ParseDate, LVO_StrConvert, LVO_StrnCmp
    };
    const ULONG count = sizeof(want) / sizeof(want[0]);
    CHECK(kaffeine_thunks_locale_manual_count == count,
          "manual count mismatch: got %u, expected %u",
          kaffeine_thunks_locale_manual_count, count);
    for (ULONG i = 0; i < count; i++) {
        CHECK(kaffeine_thunks_locale_manual[i].lvo == want[i] &&
              kaffeine_thunks_locale_manual[i].thunk != nullptr,
              "manual table slot %u mismatch (lvo %u)", i,
              kaffeine_thunks_locale_manual[i].lvo);
    }
    CHECK(kaffeine_thunks_locale_manual[count].lvo == 0 &&
          kaffeine_thunks_locale_manual[count].thunk == nullptr,
          "manual table missing trailing {0, NULL} sentinel");
}

static void check_guest_string_field(LocaleFixture &f, ULONG loc_addr,
                                     ULONG block_end, ULONG field_off,
                                     const char *expected, const char *name)
{
    ULONG str_addr = m68k_read32(&f.ctx, loc_addr + field_off);
    if (!expected) {
        CHECK(str_addr == 0, "%s expected NULL (0), got 0x%08x", name, str_addr);
        return;
    }
    ULONG len = static_cast<ULONG>(std::strlen(expected)) + 1;
    CHECK(str_addr >= loc_addr + M68K_SIZEOF_LOCALE &&
          str_addr + len <= block_end,
          "%s pointer 0x%08x outside locale block [0x%08x, 0x%08x)",
          name, str_addr, loc_addr + M68K_SIZEOF_LOCALE, block_end);
    if (str_addr < f.ctx.mem_size && f.ctx.mem_size - str_addr >= len) {
        CHECK(std::memcmp(f.ctx.mem + str_addr, expected, len) == 0,
              "%s content mismatch: got '%s', expected '%s'",
              name, reinterpret_cast<const char *>(f.ctx.mem + str_addr),
              expected);
    }
}

static void check_guest_bytes_field(LocaleFixture &f, ULONG loc_addr,
                                    ULONG block_end, ULONG field_off,
                                    const UBYTE *expected, ULONG len,
                                    const char *name)
{
    ULONG arr_addr = m68k_read32(&f.ctx, loc_addr + field_off);
    CHECK(arr_addr >= loc_addr + M68K_SIZEOF_LOCALE &&
          arr_addr + len < block_end,
          "%s pointer 0x%08x outside locale block [0x%08x, 0x%08x)",
          name, arr_addr, loc_addr + M68K_SIZEOF_LOCALE, block_end);
    if (arr_addr < f.ctx.mem_size && f.ctx.mem_size - arr_addr >= len) {
        CHECK(std::memcmp(f.ctx.mem + arr_addr, expected, len) == 0,
              "%s byte array mismatch", name);
        CHECK(f.ctx.mem_size - arr_addr > len && f.ctx.mem[arr_addr + len] == 0,
              "%s is not followed by a zero byte", name);
    }
}

static void test_catalog_fallback()
{
    LocaleFixture f;
    /* Pre-write return PC at kStack so the dispatch pop doesn't look like a
       thunk write in the guest memory snapshot. */
    m68k_write32(&f.ctx, LocaleFixture::kStack, LocaleFixture::kReturnPc);
    std::vector<UBYTE> snap(f.ctx.mem, f.ctx.mem + f.ctx.mem_size);

    /* OpenCatalogA returns NULL without calling native code or reading memory. */
    RegSnapshot r = sentinel_regs();
    r.a[0] = 0x12345678UL;
    r.a[1] = 0xCAFEBABEUL;
    r.a[2] = 0xFFFFFFFCUL;
    ULONG cat = f.call_locale(LVO_OpenCatalogA, r, false, "OpenCatalogA");
    CHECK(cat == 0, "OpenCatalogA must return NULL, got 0x%08x", cat);
    CHECK(g_open_catalog_calls == 0, "OpenCatalogA must not call native code");
    CHECK(std::memcmp(f.ctx.mem, snap.data(), f.ctx.mem_size) == 0,
          "OpenCatalogA modified guest memory");

    /* CloseCatalog is a no-op that preserves D0 and all other registers. */
    r = sentinel_regs();
    r.a[0] = 0xDEADBEEFUL;
    f.call_locale(LVO_CloseCatalog, r, true, "CloseCatalog");
    CHECK(g_close_catalog_calls == 0, "CloseCatalog must not call native code");
    CHECK(std::memcmp(f.ctx.mem, snap.data(), f.ctx.mem_size) == 0,
          "CloseCatalog modified guest memory");

    /* GetCatalogStr returns A1 unchanged without reading A1 or calling native code. */
    const ULONG test_a1_values[] = {
        0x00001234UL,
        0x00000000UL,
        f.ctx.mem_size,
        0xFFFFFFFEUL,
    };
    for (ULONG def_addr : test_a1_values) {
        r = sentinel_regs();
        r.a[0] = 0xBAD0C0DEUL;
        r.d[0] = 42;
        r.a[1] = def_addr;
        ULONG res = f.call_locale(LVO_GetCatalogStr, r, false, "GetCatalogStr");
        CHECK(res == def_addr,
              "GetCatalogStr must return A1 (0x%08x) unchanged, got 0x%08x",
              def_addr, res);
    }
    CHECK(g_get_catalog_str_calls == 0, "GetCatalogStr must not call native code");
    CHECK(std::memcmp(f.ctx.mem, snap.data(), f.ctx.mem_size) == 0,
          "GetCatalogStr modified guest memory");
}

static void test_open_locale_fields_and_strings()
{
    LocaleFixture f;
    ULONG free_before = f.avail_mem();

    RegSnapshot r = sentinel_regs();
    r.a[0] = 0; /* NULL -> default locale */
    ULONG loc_addr = f.call_locale(LVO_OpenLocale, r, false, "OpenLocale(NULL)");
    CHECK(loc_addr != 0, "OpenLocale(NULL) failed");
    CHECK(g_open_locale_calls == 1, "OpenLocale should call native OpenLocale once");
    CHECK(active_native_locales() == 1, "Expected 1 active native locale");

    ULONG block_size = m68k_read32(&f.ctx, loc_addr - KAFFEINE_HEAP_HDR);
    ULONG block_end = (loc_addr - KAFFEINE_HEAP_HDR) + block_size;
    CHECK(block_end <= f.ctx.heap_end, "Locale block exceeds guest heap");

    /* Verify every string and byte-array pointer in the 168-byte guest struct. */
    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_LOCALENAME,
                             "deutsch_deutschland.locale", "loc_LocaleName");
    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_LANGUAGENAME,
                             "deutsch.language", "loc_LanguageName");
    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_PREFLANGUAGES + 0,
                             "deutsch", "loc_PrefLanguages[0]");
    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_PREFLANGUAGES + 4,
                             "englisch", "loc_PrefLanguages[1]");
    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_PREFLANGUAGES + 8,
                             "franzoesisch", "loc_PrefLanguages[2]");
    for (int i = 3; i < M68K_LOC_PREFLANGUAGES_COUNT; i++) {
        check_guest_string_field(f, loc_addr, block_end,
                                 M68K_LOC_PREFLANGUAGES + static_cast<ULONG>(i * 4),
                                 nullptr, "loc_PrefLanguages[null]");
    }

    /* Verify scalar fields and reserved bytes. */
    CHECK(m68k_read32(&f.ctx, loc_addr + M68K_LOC_FLAGS) == 0x12345678UL,
          "loc_Flags mismatch");
    CHECK(m68k_read32(&f.ctx, loc_addr + M68K_LOC_CODESET) == 4UL,
          "loc_CodeSet mismatch");
    CHECK(m68k_read32(&f.ctx, loc_addr + M68K_LOC_COUNTRYCODE) == 0x44455500UL,
          "loc_CountryCode mismatch");
    CHECK(m68k_read32(&f.ctx, loc_addr + M68K_LOC_TELEPHONECODE) == 49UL,
          "loc_TelephoneCode mismatch");
    CHECK(static_cast<LONG>(m68k_read32(&f.ctx, loc_addr + M68K_LOC_GMTOFFSET)) == -60,
          "loc_GMTOffset mismatch");
    CHECK(f.ctx.mem[loc_addr + M68K_LOC_MEASURINGSYSTEM] == MS_IMPERIAL,
          "loc_MeasuringSystem mismatch");
    CHECK(f.ctx.mem[loc_addr + M68K_LOC_CALENDARTYPE] == CT_7MON,
          "loc_CalendarType mismatch");
    CHECK(f.ctx.mem[loc_addr + M68K_LOC_RESERVED0] == 0xAA &&
          f.ctx.mem[loc_addr + M68K_LOC_RESERVED0 + 1] == 0x55,
          "loc_Reserved0 mismatch");

    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_DATETIMEFORMAT,
                             "%A %d %B %Y %H:%M:%S", "loc_DateTimeFormat");
    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_DATEFORMAT,
                             "%A %d %B %Y", "loc_DateFormat");
    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_TIMEFORMAT,
                             "%H:%M:%S", "loc_TimeFormat");
    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_SHORTDATETIMEFORMAT,
                             "%d.%m.%Y %H:%M", "loc_ShortDateTimeFormat");
    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_SHORTDATEFORMAT,
                             "%d.%m.%Y", "loc_ShortDateFormat");
    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_SHORTTIMEFORMAT,
                             "%H:%M", "loc_ShortTimeFormat");

    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_DECIMALPOINT,
                             ",", "loc_DecimalPoint");
    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_GROUPSEPARATOR,
                             ".", "loc_GroupSeparator");
    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_FRACGROUPSEPARATOR,
                             " ", "loc_FracGroupSeparator");
    check_guest_bytes_field(f, loc_addr, block_end, M68K_LOC_GROUPING,
                            kGrouping, sizeof(kGrouping), "loc_Grouping");
    check_guest_bytes_field(f, loc_addr, block_end, M68K_LOC_FRACGROUPING,
                            kFracGrouping, sizeof(kFracGrouping), "loc_FracGrouping");

    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_MONDECIMALPOINT,
                             ";", "loc_MonDecimalPoint");
    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_MONGROUPSEPARATOR,
                             "'", "loc_MonGroupSeparator");
    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_MONFRACGROUPSEPARATOR,
                             "_", "loc_MonFracGroupSeparator");
    check_guest_bytes_field(f, loc_addr, block_end, M68K_LOC_MONGROUPING,
                            kMonGrouping, sizeof(kMonGrouping), "loc_MonGrouping");
    check_guest_bytes_field(f, loc_addr, block_end, M68K_LOC_MONFRACGROUPING,
                            kMonFracGrouping, sizeof(kMonFracGrouping),
                            "loc_MonFracGrouping");

    CHECK(f.ctx.mem[loc_addr + M68K_LOC_MONFRACDIGITS] == 2,
          "loc_MonFracDigits mismatch");
    CHECK(f.ctx.mem[loc_addr + M68K_LOC_MONINTFRACDIGITS] == 3,
          "loc_MonIntFracDigits mismatch");
    CHECK(f.ctx.mem[loc_addr + M68K_LOC_RESERVED1] == 0x11 &&
          f.ctx.mem[loc_addr + M68K_LOC_RESERVED1 + 1] == 0x22,
          "loc_Reserved1 mismatch");

    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_MONCS,
                             "EUR", "loc_MonCS");
    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_MONSMALLCS,
                             "ct", "loc_MonSmallCS");
    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_MONINTCS,
                             "EUR ", "loc_MonIntCS");

    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_MONPOSITIVESIGN,
                             "+", "loc_MonPositiveSign");
    CHECK(f.ctx.mem[loc_addr + M68K_LOC_MONPOSITIVESPACESEP] == SS_SPACE,
          "loc_MonPositiveSpaceSep mismatch");
    CHECK(f.ctx.mem[loc_addr + M68K_LOC_MONPOSITIVESIGNPOS] == SP_PREC_ALL,
          "loc_MonPositiveSignPos mismatch");
    CHECK(f.ctx.mem[loc_addr + M68K_LOC_MONPOSITIVECSPOS] == CSP_SUCCEEDS,
          "loc_MonPositiveCSPos mismatch");
    CHECK(f.ctx.mem[loc_addr + M68K_LOC_RESERVED2] == 0x33,
          "loc_Reserved2 mismatch");

    check_guest_string_field(f, loc_addr, block_end, M68K_LOC_MONNEGATIVESIGN,
                             "-", "loc_MonNegativeSign");
    CHECK(f.ctx.mem[loc_addr + M68K_LOC_MONNEGATIVESPACESEP] == SS_NOSPACE,
          "loc_MonNegativeSpaceSep mismatch");
    CHECK(f.ctx.mem[loc_addr + M68K_LOC_MONNEGATIVESIGNPOS] == SP_SUCC_CURR,
          "loc_MonNegativeSignPos mismatch");
    CHECK(f.ctx.mem[loc_addr + M68K_LOC_MONNEGATIVECSPOS] == CSP_PRECEDES,
          "loc_MonNegativeCSPos mismatch");
    CHECK(f.ctx.mem[loc_addr + M68K_LOC_RESERVED3] == 0x44,
          "loc_Reserved3 mismatch");

    /* CloseLocale releases the guest block and closes the native locale. */
    r = sentinel_regs();
    r.a[0] = loc_addr;
    f.call_locale(LVO_CloseLocale, r, true, "CloseLocale");
    CHECK(active_native_locales() == 0, "Native locale should be closed");
    CHECK(f.avail_mem() == free_before,
          "Guest heap not fully restored after CloseLocale");

    /* Named locale at exact end of guest memory succeeds; 1 byte past fails. */
    const char *exact_name = "italiano.locale";
    ULONG exact_len = static_cast<ULONG>(std::strlen(exact_name)) + 1;
    ULONG exact_addr = f.ctx.mem_size - exact_len;
    f.write_cstr(exact_addr, exact_name);

    g_null_optional_strings = true;
    r = sentinel_regs();
    r.a[0] = exact_addr;
    ULONG named_loc = f.call_locale(LVO_OpenLocale, r, false, "OpenLocale(exact)");
    CHECK(named_loc != 0, "OpenLocale with string ending at mem_size failed");
    ULONG named_end = (named_loc - KAFFEINE_HEAP_HDR) +
                      m68k_read32(&f.ctx, named_loc - KAFFEINE_HEAP_HDR);
    check_guest_string_field(f, named_loc, named_end, M68K_LOC_LOCALENAME,
                             exact_name, "named loc_LocaleName");
    check_guest_string_field(f, named_loc, named_end, M68K_LOC_FRACGROUPSEPARATOR,
                             nullptr, "null loc_FracGroupSeparator");
    check_guest_string_field(f, named_loc, named_end, M68K_LOC_MONSMALLCS,
                             nullptr, "null loc_MonSmallCS");

    r = sentinel_regs();
    r.a[0] = named_loc;
    f.call_locale(LVO_CloseLocale, r, true, "CloseLocale(named)");

    /* Overwrite NUL at mem_size - 1 so string runs 1 byte past guest memory. */
    f.ctx.mem[f.ctx.mem_size - 1] = 'X';
    int calls_before = g_open_locale_calls;
    r = sentinel_regs();
    r.a[0] = exact_addr;
    CHECK(f.call_locale(LVO_OpenLocale, r, false, "OpenLocale(unterminated)") == 0,
          "OpenLocale with unterminated guest string must return NULL");
    CHECK(g_open_locale_calls == calls_before,
          "OpenLocale must not call native OpenLocale for unterminated string");

    /* Out-of-bounds name address also returns NULL without calling native code. */
    r = sentinel_regs();
    r.a[0] = f.ctx.mem_size;
    CHECK(f.call_locale(LVO_OpenLocale, r, false, "OpenLocale(oob)") == 0,
          "OpenLocale(mem_size) must return NULL");
    CHECK(g_open_locale_calls == calls_before,
          "OpenLocale(mem_size) must not call native OpenLocale");
}

static void test_close_locale_and_freemem_protection()
{
    LocaleFixture f;
    ULONG free_initial = f.avail_mem();

    RegSnapshot r = sentinel_regs();
    r.a[0] = 0;
    ULONG loc_addr = f.call_locale(LVO_OpenLocale, r, false, "OpenLocale");
    CHECK(loc_addr != 0, "OpenLocale failed");
    ULONG free_with_locale = f.avail_mem();
    CHECK(free_with_locale < free_initial, "Locale did not consume guest heap");

    /* CloseLocale(NULL) does nothing and preserves D0. */
    m68k_write32(&f.ctx, LocaleFixture::kStack, LocaleFixture::kReturnPc);
    std::vector<UBYTE> snap(f.ctx.mem, f.ctx.mem + f.ctx.mem_size);
    r = sentinel_regs();
    r.a[0] = 0;
    f.call_locale(LVO_CloseLocale, r, true, "CloseLocale(NULL)");
    CHECK(g_close_locale_calls == 0, "CloseLocale(NULL) called native CloseLocale");
    CHECK(std::memcmp(f.ctx.mem, snap.data(), f.ctx.mem_size) == 0,
          "CloseLocale(NULL) modified guest memory");

    /* Allocate a normal guest AllocMem block and try CloseLocale on it. */
    r = sentinel_regs();
    r.d[0] = M68K_SIZEOF_LOCALE;
    r.d[1] = MEMF_PUBLIC | MEMF_CLEAR;
    ULONG user_block = f.call_exec(LVO_AllocMem, r, false, "AllocMem");
    CHECK(user_block != 0, "AllocMem failed");
    ULONG free_with_user = f.avail_mem();

    const ULONG foreign_addrs[] = {
        user_block,
        loc_addr + 4,
        0x00001000UL,
        f.ctx.mem_size + 16,
    };
    for (ULONG bad : foreign_addrs) {
        r = sentinel_regs();
        r.a[0] = bad;
        f.call_locale(LVO_CloseLocale, r, true, "CloseLocale(foreign)");
    }
    CHECK(g_close_locale_calls == 0,
          "CloseLocale on foreign addresses must not call native CloseLocale");
    CHECK(f.avail_mem() == free_with_user,
          "CloseLocale on foreign addresses must not free guest memory");

    /* 68k FreeMem() on the open guest Locale block must be refused. */
    r = sentinel_regs();
    r.a[1] = loc_addr;
    r.d[0] = M68K_SIZEOF_LOCALE;
    f.call_exec(LVO_FreeMem, r, false, "FreeMem(locale)");
    CHECK(f.avail_mem() == free_with_user,
          "68k FreeMem must not release a HEAP_LOCALE block");

    /* Even if the 68k program clobbers the guest heap header before loc_addr,
       FreeMem still cannot free it and CloseLocale still releases it cleanly. */
    m68k_write32(&f.ctx, loc_addr - KAFFEINE_HEAP_HDR, M68K_SIZEOF_LOCALE);
    m68k_write32(&f.ctx, loc_addr - 4, 0);
    r = sentinel_regs();
    r.a[1] = loc_addr;
    r.d[0] = M68K_SIZEOF_LOCALE;
    f.call_exec(LVO_FreeMem, r, false, "FreeMem(forged locale)");
    CHECK(f.avail_mem() == free_with_user,
          "68k FreeMem with forged header must not release HEAP_LOCALE");

    /* Free the normal user block first. */
    r = sentinel_regs();
    r.a[1] = user_block;
    r.d[0] = M68K_SIZEOF_LOCALE;
    f.call_exec(LVO_FreeMem, r, false, "FreeMem(user_block)");
    CHECK(f.avail_mem() == free_with_locale, "User block was not freed");

    /* Now CloseLocale(loc_addr) releases the locale block and native locale. */
    r = sentinel_regs();
    r.a[0] = loc_addr;
    f.call_locale(LVO_CloseLocale, r, true, "CloseLocale(valid)");
    CHECK(g_close_locale_calls == 1, "CloseLocale should call native CloseLocale");
    CHECK(active_native_locales() == 0, "Native locale still active after close");
    CHECK(f.avail_mem() == free_initial, "Heap not restored after CloseLocale");

    /* Closing the same locale address a second time is a no-op. */
    r = sentinel_regs();
    r.a[0] = loc_addr;
    f.call_locale(LVO_CloseLocale, r, true, "CloseLocale(twice)");
    CHECK(g_close_locale_calls == 1,
          "Second CloseLocale must not call native CloseLocale again");
    CHECK(f.avail_mem() == free_initial,
          "Second CloseLocale must not alter heap");
}

static void test_eight_locale_limit_and_heap_exhaustion()
{
    LocaleFixture f;
    ULONG free_initial = f.avail_mem();
    ULONG locales[KAFFEINE_MAX_LOCALES]{};

    for (int i = 0; i < KAFFEINE_MAX_LOCALES; i++) {
        RegSnapshot r = sentinel_regs();
        r.a[0] = 0;
        locales[i] = f.call_locale(LVO_OpenLocale, r, false, "OpenLocale(slot)");
        CHECK(locales[i] != 0, "OpenLocale slot %d failed", i);
    }
    CHECK(active_native_locales() == KAFFEINE_MAX_LOCALES,
          "Expected %d active native locales", KAFFEINE_MAX_LOCALES);

    /* 9th OpenLocale must fail without leaking a native locale. */
    RegSnapshot r = sentinel_regs();
    r.a[0] = 0;
    ULONG ninth = f.call_locale(LVO_OpenLocale, r, false, "OpenLocale(9th)");
    CHECK(ninth == 0, "9th OpenLocale must return NULL, got 0x%08x", ninth);
    CHECK(active_native_locales() == KAFFEINE_MAX_LOCALES,
          "9th OpenLocale leaked a native locale");

    /* Closing slot 3 allows opening a new locale in that slot. */
    r = sentinel_regs();
    r.a[0] = locales[3];
    f.call_locale(LVO_CloseLocale, r, true, "CloseLocale(slot 3)");
    CHECK(active_native_locales() == KAFFEINE_MAX_LOCALES - 1,
          "Slot 3 native locale not closed");

    r = sentinel_regs();
    r.a[0] = 0;
    locales[3] = f.call_locale(LVO_OpenLocale, r, false, "OpenLocale(reopen 3)");
    CHECK(locales[3] != 0, "Reopening slot 3 failed");
    CHECK(active_native_locales() == KAFFEINE_MAX_LOCALES,
          "Expected 8 active locales after reopen");

    for (int i = 0; i < KAFFEINE_MAX_LOCALES; i++) {
        r = sentinel_regs();
        r.a[0] = locales[i];
        f.call_locale(LVO_CloseLocale, r, true, "CloseLocale(all)");
    }
    CHECK(active_native_locales() == 0, "All native locales should be closed");
    CHECK(f.avail_mem() == free_initial, "Heap not restored after closing all 8");

    /* Small-heap fixture: when guest heap cannot fit struct Locale + strings,
       OpenLocale must close the native locale and return 0. */
    LocaleFixture tiny(16 * 1024, 0x2000, 0x2080);
    r = sentinel_regs();
    r.a[0] = 0;
    ULONG oom_loc = tiny.call_locale(LVO_OpenLocale, r, false, "OpenLocale(OOM)");
    CHECK(oom_loc == 0, "OpenLocale must return NULL when guest heap is full");
    CHECK(active_native_locales() == 0,
          "OpenLocale must close native locale when guest allocation fails");
}

static void test_get_locale_str_caching_and_release()
{
    LocaleFixture f;
    ULONG free_initial = f.avail_mem();

    RegSnapshot r = sentinel_regs();
    r.a[0] = 0;
    ULONG loc_addr = f.call_locale(LVO_OpenLocale, r, false, "OpenLocale");
    CHECK(loc_addr != 0, "OpenLocale failed");
    ULONG free_after_open = f.avail_mem();

    /* NULL and foreign locale addresses return NULL without calling native code. */
    r = sentinel_regs();
    r.a[0] = 0;
    r.d[0] = DAY_1;
    CHECK(f.call_locale(LVO_GetLocaleStr, r, false, "GetLocaleStr(NULL)") == 0,
          "GetLocaleStr(NULL) must return NULL");
    r = sentinel_regs();
    r.a[0] = loc_addr + 4;
    r.d[0] = DAY_1;
    CHECK(f.call_locale(LVO_GetLocaleStr, r, false, "GetLocaleStr(foreign)") == 0,
          "GetLocaleStr(foreign) must return NULL");
    CHECK(g_get_locale_str_calls == 0,
          "GetLocaleStr with invalid locale must not call native code");

    /* Valid string numbers (including 70, the highest NDK 3.2 index < 71). */
    r = sentinel_regs();
    r.a[0] = loc_addr;
    r.d[0] = DAY_1;
    ULONG day1_addr = f.call_locale(LVO_GetLocaleStr, r, false, "GetLocaleStr(DAY_1)");
    CHECK(day1_addr != 0, "GetLocaleStr(DAY_1) failed");
    CHECK(std::strcmp(reinterpret_cast<const char *>(f.ctx.mem + day1_addr),
                      "Sunday") == 0,
          "GetLocaleStr(DAY_1) content mismatch");
    CHECK(g_get_locale_str_calls == 1, "Expected 1 native GetLocaleStr call");

    ULONG free_after_day1 = f.avail_mem();
    CHECK(free_after_day1 < free_after_open,
          "GetLocaleStr copy should allocate from guest heap");

    /* Repeated call returns the cached guest copy without allocating again. */
    r = sentinel_regs();
    r.a[0] = loc_addr;
    r.d[0] = DAY_1;
    ULONG day1_again = f.call_locale(LVO_GetLocaleStr, r, false,
                                     "GetLocaleStr(DAY_1 cached)");
    CHECK(day1_again == day1_addr,
          "Second GetLocaleStr(DAY_1) must return cached address");
    CHECK(g_get_locale_str_calls == 1,
          "Cached GetLocaleStr must not call native GetLocaleStr again");
    CHECK(f.avail_mem() == free_after_day1,
          "Cached GetLocaleStr must not allocate another block");

    /* NDK 3.2 upper bound: stringNum 70 succeeds; 71 and above return NULL. */
    r = sentinel_regs();
    r.a[0] = loc_addr;
    r.d[0] = 70;
    ULONG s70_addr = f.call_locale(LVO_GetLocaleStr, r, false, "GetLocaleStr(70)");
    CHECK(s70_addr != 0, "GetLocaleStr(70) failed");
    CHECK(std::strcmp(reinterpret_cast<const char *>(f.ctx.mem + s70_addr),
                      "NDK-Slot-70") == 0,
          "GetLocaleStr(70) content mismatch");

    ULONG free_after_s70 = f.avail_mem();
    const ULONG bad_nums[] = { 0, 51, 71, 72, 1000, 0xFFFFFFFFUL };
    for (ULONG num : bad_nums) {
        r = sentinel_regs();
        r.a[0] = loc_addr;
        r.d[0] = num;
        CHECK(f.call_locale(LVO_GetLocaleStr, r, false, "GetLocaleStr(bad)") == 0,
              "GetLocaleStr(%u) must return NULL", num);
    }
    CHECK(f.avail_mem() == free_after_s70,
          "Failed GetLocaleStr calls must not allocate guest memory");

    /* 68k FreeMem() cannot free a GetLocaleStr() block. */
    r = sentinel_regs();
    r.a[1] = day1_addr;
    r.d[0] = 7;
    f.call_exec(LVO_FreeMem, r, false, "FreeMem(GetLocaleStr copy)");
    CHECK(f.avail_mem() == free_after_s70,
          "68k FreeMem must not free a GetLocaleStr block");

    /* CloseLocale frees both the locale block and all cached GetLocaleStr copies. */
    r = sentinel_regs();
    r.a[0] = loc_addr;
    f.call_locale(LVO_CloseLocale, r, true, "CloseLocale");
    CHECK(f.avail_mem() == free_initial,
          "CloseLocale must free all GetLocaleStr blocks and the Locale block");
}

static void test_strconvert_and_strncmp_bounds()
{
    LocaleFixture f;

    RegSnapshot r = sentinel_regs();
    r.a[0] = 0;
    ULONG loc_addr = f.call_locale(LVO_OpenLocale, r, false, "OpenLocale");
    CHECK(loc_addr != 0, "OpenLocale failed");

    /* Place a 4-byte string ("abc\0") ending right at mem_size. */
    ULONG str_exact = f.ctx.mem_size - 4;
    f.write_cstr(str_exact, "abc");

    /* Place a 4-byte output buffer ending right at mem_size - 4. */
    ULONG buf_exact = f.ctx.mem_size - 8;
    std::memset(f.ctx.mem + buf_exact, 0xCC, 4);

    r = sentinel_regs();
    r.a[0] = loc_addr;
    r.a[1] = str_exact;
    r.a[2] = buf_exact;
    r.d[0] = 4;
    r.d[1] = SC_ASCII;
    ULONG conv_len = f.call_locale(LVO_StrConvert, r, false, "StrConvert(exact)");
    CHECK(conv_len == 3, "StrConvert expected 3, got %u", conv_len);
    CHECK(std::memcmp(f.ctx.mem + buf_exact, "ABC\0", 4) == 0,
          "StrConvert output mismatch");

    /* Buffer ending exactly at mem_size (using an earlier input string). */
    ULONG str_in = 0x1000;
    f.write_cstr(str_in, "xy");
    ULONG buf_at_end = f.ctx.mem_size - 3;
    r = sentinel_regs();
    r.a[0] = loc_addr;
    r.a[1] = str_in;
    r.a[2] = buf_at_end;
    r.d[0] = 3;
    r.d[1] = SC_COLLATE1;
    CHECK(f.call_locale(LVO_StrConvert, r, false, "StrConvert(buf at end)") == 2,
          "StrConvert with buffer at end of guest memory failed");
    CHECK(std::memcmp(f.ctx.mem + buf_at_end, "XY\0", 3) == 0,
          "StrConvert at end of guest memory wrote wrong bytes");

    /* Buffer extending 1 byte past mem_size must fail without writing. */
    m68k_write32(&f.ctx, LocaleFixture::kStack, LocaleFixture::kReturnPc);
    std::vector<UBYTE> snap(f.ctx.mem, f.ctx.mem + f.ctx.mem_size);
    int conv_before = g_str_convert_calls;
    r = sentinel_regs();
    r.a[0] = loc_addr;
    r.a[1] = str_in;
    r.a[2] = f.ctx.mem_size - 2;
    r.d[0] = 3; /* 1 byte past mem_size */
    r.d[1] = SC_ASCII;
    CHECK(f.call_locale(LVO_StrConvert, r, false, "StrConvert(buf +1 past)") == 0,
          "StrConvert with buffer 1 byte past mem_size must return 0");
    CHECK(g_str_convert_calls == conv_before,
          "StrConvert with out-of-bounds buffer must not call native code");
    CHECK(std::memcmp(f.ctx.mem, snap.data(), f.ctx.mem_size) == 0,
          "StrConvert with out-of-bounds buffer modified guest memory");

    /* bufferSize == 0 writes nothing, even if buffer is at mem_size - 1. */
    r = sentinel_regs();
    r.a[0] = loc_addr;
    r.a[1] = str_in;
    r.a[2] = f.ctx.mem_size - 1;
    r.d[0] = 0;
    r.d[1] = SC_ASCII;
    CHECK(f.call_locale(LVO_StrConvert, r, false, "StrConvert(size 0)") == 0,
          "StrConvert with bufferSize 0 must return 0");
    CHECK(std::memcmp(f.ctx.mem, snap.data(), f.ctx.mem_size) == 0,
          "StrConvert with bufferSize 0 modified guest memory");

    /* Unterminated input string (1 byte past mem_size) must be rejected. */
    f.ctx.mem[f.ctx.mem_size - 1] = 'Z';
    conv_before = g_str_convert_calls;
    r = sentinel_regs();
    r.a[0] = loc_addr;
    r.a[1] = f.ctx.mem_size - 3;
    r.a[2] = 0x1200;
    r.d[0] = 16;
    r.d[1] = SC_ASCII;
    CHECK(f.call_locale(LVO_StrConvert, r, false, "StrConvert(unterm str)") == 0,
          "StrConvert with unterminated input string must return 0");
    CHECK(g_str_convert_calls == conv_before,
          "StrConvert with unterminated string must not call native code");

    /* Foreign or NULL locale in StrConvert returns 0. */
    r.a[0] = 0;
    r.a[1] = str_in;
    CHECK(f.call_locale(LVO_StrConvert, r, false, "StrConvert(NULL loc)") == 0,
          "StrConvert(NULL locale) must return 0");

    /* StrnCmp: strings ending right at mem_size succeed. */
    ULONG s1_addr = 0x1100;
    ULONG s2_exact = f.ctx.mem_size - 6;
    f.write_cstr(s1_addr, "Hello");
    f.write_cstr(s2_exact, "hello");

    r = sentinel_regs();
    r.a[0] = loc_addr;
    r.a[1] = s1_addr;
    r.a[2] = s2_exact;
    r.d[0] = 5;
    r.d[1] = SC_ASCII;
    LONG cmp = static_cast<LONG>(f.call_locale(LVO_StrnCmp, r, false,
                                               "StrnCmp(equal exact)"));
    CHECK(cmp == 0, "StrnCmp('Hello', 'hello', 5) expected 0, got %d", (int)cmp);

    /* Negative length (-1, used by LocStricmp) compares until NUL. */
    f.write_cstr(s1_addr, "Alpha");
    f.write_cstr(s2_exact, "Beta!");
    r = sentinel_regs();
    r.a[0] = loc_addr;
    r.a[1] = s1_addr;
    r.a[2] = s2_exact;
    r.d[0] = static_cast<ULONG>(-1);
    r.d[1] = SC_COLLATE1;
    cmp = static_cast<LONG>(f.call_locale(LVO_StrnCmp, r, false, "StrnCmp(-1)"));
    CHECK(cmp < 0, "StrnCmp('Alpha', 'Beta!', -1) expected < 0, got %d", (int)cmp);

    /* Unterminated string2 (1 byte past mem_size) must return 0 without calling native code. */
    f.ctx.mem[f.ctx.mem_size - 1] = '!';
    int cmp_before = g_strncmp_calls;
    r = sentinel_regs();
    r.a[0] = loc_addr;
    r.a[1] = s1_addr;
    r.a[2] = s2_exact;
    r.d[0] = 2; /* Even with small positive length, unterminated string is rejected. */
    r.d[1] = SC_ASCII;
    CHECK(f.call_locale(LVO_StrnCmp, r, false, "StrnCmp(unterm s2)") == 0,
          "StrnCmp with unterminated string2 must return 0");
    CHECK(g_strncmp_calls == cmp_before,
          "StrnCmp with unterminated string2 must not call native code");

    /* NULL or foreign locale in StrnCmp returns 0. */
    f.ctx.mem[f.ctx.mem_size - 1] = '\0';
    r.a[0] = loc_addr + 8;
    CHECK(f.call_locale(LVO_StrnCmp, r, false, "StrnCmp(foreign loc)") == 0,
          "StrnCmp(foreign locale) must return 0");
}

static void test_character_functions()
{
    LocaleFixture f;

    RegSnapshot r = sentinel_regs();
    r.a[0] = 0;
    ULONG loc_addr = f.call_locale(LVO_OpenLocale, r, false, "OpenLocale");
    CHECK(loc_addr != 0, "OpenLocale failed");

    /* ConvToLower and ConvToUpper with open locale, NULL, and foreign locale. */
    r = sentinel_regs();
    r.a[0] = loc_addr;
    r.d[0] = 'Q';
    CHECK(f.call_locale(LVO_ConvToLower, r, false, "ConvToLower(open)") == 'q',
          "ConvToLower('Q') expected 'q'");

    r = sentinel_regs();
    r.a[0] = loc_addr;
    r.d[0] = 'm';
    CHECK(f.call_locale(LVO_ConvToUpper, r, false, "ConvToUpper(open)") == 'M',
          "ConvToUpper('m') expected 'M'");

    int lower_before = g_conv_to_lower_calls;
    int upper_before = g_conv_to_upper_calls;
    const ULONG bad_locales[] = { 0, loc_addr + 4, 0x00001234UL };
    for (ULONG bad : bad_locales) {
        r = sentinel_regs();
        r.a[0] = bad;
        r.d[0] = 'Q';
        CHECK(f.call_locale(LVO_ConvToLower, r, false, "ConvToLower(bad)") == 'Q',
              "ConvToLower with invalid locale must return character unchanged");
        r = sentinel_regs();
        r.a[0] = bad;
        r.d[0] = 'm';
        CHECK(f.call_locale(LVO_ConvToUpper, r, false, "ConvToUpper(bad)") == 'm',
              "ConvToUpper with invalid locale must return character unchanged");
    }
    CHECK(g_conv_to_lower_calls == lower_before &&
          g_conv_to_upper_calls == upper_before,
          "ConvToLower/ConvToUpper with invalid locale must not call native code");

    /* All 11 Is* functions at their literal LVOs. */
    struct IsCase {
        ULONG lvo;
        const char *name;
        ULONG match_char;
        ULONG nonmatch_char;
        int call_idx;
    };
    const IsCase cases[] = {
        { LVO_IsAlNum,  "IsAlNum",  '7', '!',  0 },
        { LVO_IsAlpha,  "IsAlpha",  'Z', '7',  1 },
        { LVO_IsCntrl,  "IsCntrl",  '\n', 'A', 2 },
        { LVO_IsDigit,  "IsDigit",  '9', 'A',  3 },
        { LVO_IsGraph,  "IsGraph",  '#', ' ',  4 },
        { LVO_IsLower,  "IsLower",  'g', 'G',  5 },
        { LVO_IsPrint,  "IsPrint",  ' ', '\n', 6 },
        { LVO_IsPunct,  "IsPunct",  '!', 'A',  7 },
        { LVO_IsSpace,  "IsSpace",  ' ', 'A',  8 },
        { LVO_IsUpper,  "IsUpper",  'G', 'g',  9 },
        { LVO_IsXDigit, "IsXDigit", 'f', 'g', 10 },
    };

    for (const auto &tc : cases) {
        r = sentinel_regs();
        r.a[0] = loc_addr;
        r.d[0] = tc.match_char;
        CHECK(f.call_locale(tc.lvo, r, false, tc.name) != 0,
              "%s('%c') expected non-zero", tc.name, (int)tc.match_char);

        r = sentinel_regs();
        r.a[0] = loc_addr;
        r.d[0] = tc.nonmatch_char;
        CHECK(f.call_locale(tc.lvo, r, false, tc.name) == 0,
              "%s('%c') expected 0", tc.name, (int)tc.nonmatch_char);

        int calls_now = g_is_calls[tc.call_idx];
        CHECK(calls_now == 2, "%s expected 2 native calls, got %d",
              tc.name, calls_now);

        for (ULONG bad : bad_locales) {
            r = sentinel_regs();
            r.a[0] = bad;
            r.d[0] = tc.match_char;
            CHECK(f.call_locale(tc.lvo, r, false, tc.name) == 0,
                  "%s with invalid locale 0x%08x must return 0", tc.name, bad);
        }
        CHECK(g_is_calls[tc.call_idx] == calls_now,
              "%s with invalid locale must not call native code", tc.name);
    }
}

static void test_context_cleanup()
{
    LocaleFixture f;
    ULONG free_initial = f.avail_mem();

    for (int i = 0; i < 4; i++) {
        RegSnapshot r = sentinel_regs();
        r.a[0] = 0;
        ULONG loc = f.call_locale(LVO_OpenLocale, r, false, "OpenLocale(cleanup)");
        CHECK(loc != 0, "OpenLocale %d failed", i);

        r = sentinel_regs();
        r.a[0] = loc;
        r.d[0] = DAY_1;
        CHECK(f.call_locale(LVO_GetLocaleStr, r, false, "GetLocaleStr(DAY_1)") != 0,
              "GetLocaleStr(DAY_1) failed");
        r = sentinel_regs();
        r.a[0] = loc;
        r.d[0] = YESSTR;
        CHECK(f.call_locale(LVO_GetLocaleStr, r, false, "GetLocaleStr(YESSTR)") != 0,
              "GetLocaleStr(YESSTR) failed");
    }
    CHECK(active_native_locales() == 4, "Expected 4 active native locales");
    CHECK(f.avail_mem() < free_initial, "Expected guest heap in use");

    Kaffeine_LocaleCleanup(&f.ctx);
    CHECK(active_native_locales() == 0,
          "Kaffeine_LocaleCleanup must close all native locales");
    CHECK(f.avail_mem() == free_initial,
          "Kaffeine_LocaleCleanup must free all guest locale and string blocks");
}

int main()
{
    test_table();
    test_catalog_fallback();
    test_open_locale_fields_and_strings();
    test_close_locale_and_freemem_protection();
    test_eight_locale_limit_and_heap_exhaustion();
    test_get_locale_str_caching_and_release();
    test_strconvert_and_strncmp_bounds();
    test_character_functions();
    test_context_cleanup();

    if (g_failures) {
        std::fprintf(stderr, "test_locale_thunks: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_locale_thunks: all tests passed\n");
    return 0;
}
