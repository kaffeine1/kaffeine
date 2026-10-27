/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
    Author: Fabian Schmieder (@metaneutrons)
    kaffeine.library — Moira CPU subclass
*/
#include <aros/debug.h>
#include <dos/dos.h>
#include "kaffeine_intern.h"
#include "kaffeine_offsets.h"
#include "kaffeine_cpu.h"
#include "kaffeine_dispatch.h"
#include "kaffeine_callback.h"
#include "kaffeine_sched.h"
#include "Moira/Moira.h"

#include <new>

using namespace moira;

#define RTE_ADDR      0x000100u   /* address of our RTE instruction */
#define SV_TRAMP_ADDR 0x000108u   /* supervisor trampoline: skip frame + JMP (A5) */

/* The return address of the program. Any read of it stops the CPU, so no
   data may live there: it follows the callback return tokens in the user
   interrupt vectors, which no exception uses, below the heap. */
#define SENTINEL_ADDR 0x000160u
static_assert(SENTINEL_ADDR >= KAFFEINE_CALLBACK_RETURN_BASE + KAFFEINE_CALLBACK_RETURN_SIZE &&
              SENTINEL_ADDR + 2 <= KAFFEINE_VECTORS_SIZE,
              "the sentinel must stay in the free vector space, below the heap");
/* The root returns to the sentinel, a child to its own end address, whose
   prefetch must not read the sentinel. */
static_assert(KAFFEINE_TASK_EXIT_ADDR >= SENTINEL_ADDR + 2 &&
              KAFFEINE_TASK_EXIT_ADDR + 4 <= KAFFEINE_VECTORS_SIZE,
              "a child's end address must not be the root's sentinel");

namespace
{

extern const struct KaffeineCPUOps moira_cpu_ops;

class AROSMoira : public Moira
{
public:
    struct KaffeineContext *ctx;
    struct KaffeineCPU cpu_handle;
    /* Set by willExecute when a LINEA trap was a library call */
    bool lineaDispatched = false;
    bool faulted = false;
    u32 instructionPC = 0;

    explicit AROSMoira(struct KaffeineContext *c)
        : ctx(c), cpu_handle{&moira_cpu_ops, this}
    {
        /* Same C68020 core and 040 instructions, without installing Moira's
           throwing FPU and MMU handlers: FPU, PFLUSH and PTEST opcodes stay
           Line-F. AROS modules cannot rely on C++ unwinding. */
        setModel(Model::M68EC040);
    }

    void stopFault(const char *kind, u32 pc, const char *detail = nullptr) noexcept
    {
        faulted = true;
        ctx->running = FALSE;
        ctx->exit_code = RETURN_FAIL;
        setD(0, RETURN_FAIL);
        if (ctx->callback_depth) ctx->callback_failed = TRUE;
        bug("[kaffeine] CPU fault: %s at PC=0x%08lx%s%s\n", kind,
            (unsigned long)pc, detail ? ": " : "", detail ? detail : "");
    }

    /* ── Custom chip register emulation ──────────────────────────── */

    /* Advance beam position by 'ticks' color clocks (PAL: 313 lines × 227 clocks) */
    void beamAdvance(int ticks)
    {
        ctx->beam_h += ticks;
        while (ctx->beam_h >= PAL_CLOCKS_PER_LINE)
        {
            ctx->beam_h -= PAL_CLOCKS_PER_LINE;
            ctx->beam_v++;
            if (ctx->beam_v >= PAL_LINES_PER_FRAME)
            {
                ctx->beam_v = 0;
                ctx->beam_lof ^= 1;
            }
        }
    }

    /* Read custom chip register (word-aligned addr relative to DFF000) */
    u16 customRead16(u16 reg) const
    {
        const_cast<AROSMoira*>(this)->beamAdvance(2);
        switch (reg)
        {
            case 0x004: /* VPOSR */
                return ((u16)ctx->beam_lof << 15) |
                       ((ctx->beam_v >> 8) & 1);  /* PAL Agnus ID = 0x00 */
            case 0x006: /* VHPOSR */
                return ((u16)(ctx->beam_v & 0xFF) << 8) | ctx->beam_h;
            case 0x010: /* ADKCONR */
                return 0;
            case 0x016: /* POTGOR/POTINP */
                return POTGOR_DEFAULT; /* no buttons pressed */
            case 0x01C: /* INTREQR */
                return 0;
            case 0x01E: /* INTENA */
                return 0;
            case 0x002: /* DMACONR */
                return 0;
            default:
                return 0;
        }
    }

    void customWrite16(u16 reg, u16 val) const
    {
        switch (reg)
        {
            case 0x02A: /* VPOSW */
                const_cast<AROSMoira*>(this)->ctx->beam_lof = (val >> 15) & 1;
                const_cast<AROSMoira*>(this)->ctx->beam_v =
                    (ctx->beam_v & 0xFF) | ((val & 1) << 8);
                break;
            case 0x02C: /* VHPOSW */
                const_cast<AROSMoira*>(this)->ctx->beam_v =
                    (ctx->beam_v & 0x100) | (val >> 8);
                const_cast<AROSMoira*>(this)->ctx->beam_h = val & 0xFF;
                break;
            default:
                break; /* ignore writes to unimplemented registers */
        }
    }

    /*
     * Supervisor() redirect: push an exception frame for the caller's
     * return, then arrange for Moira's LINEA exception to land at the
     * user function via a trampoline that preserves supervisor mode.
     *
     * After willExecute returns, Moira will:
     *   1. Capture old SR (already done before willExecute)
     *   2. Enter supervisor mode
     *   3. Push exception frame (old_SR, PC-2) onto SSP
     *   4. Jump to LINEA vector
     *
     * We redirect the LINEA vector to SV_TRAMP_ADDR which skips
     * Moira's frame and JMPs to (A5) = userFunc, staying in
     * supervisor mode. The user function's RTE pops our manually
     * pushed frame, returning to the original caller.
     */
    void dispatchSupervisor(u32 ret_addr)
    {
        ULONG userFunc = ctx->sv_redirect;
        ctx->sv_redirect = 0;

        /* Push exception frame for the user function's RTE:
           68020 format: vector_offset(word) + PC(long) + SR(word) = 8 bytes
           68000 format: PC(long) + SR(word) = 6 bytes */
        u32 sp = getA(7);
        sp -= 2; m68k_write16(ctx, sp, 0x0000);  /* vector offset (68020) */
        sp -= 4; m68k_write32(ctx, sp, ret_addr); /* return PC */
        sp -= 2; m68k_write16(ctx, sp, 0x0000);  /* SR: user mode */
        setA(7, sp);

        /* Redirect LINEA vector to supervisor trampoline */
        m68k_write32(ctx, M68K_VEC_LINEA, SV_TRAMP_ADDR);

        /* Set PC so Moira's exception frame points to userFunc.
           68020 pushes reg.pc0, 68000 pushes reg.pc-2. */
        setPC0(userFunc);
        setPC(userFunc + 2);

        bug("[kaffeine] Supervisor: dispatching to 0x%lx, ret=0x%lx\n",
            (unsigned long)userFunc, (unsigned long)ret_addr);
    }

    bool guestAccess(u32 addr, u32 size) const
    {
        if (ctx->callback_failed) return false;
        if (addr >= ctx->mem_size || ctx->mem_size - addr < size)
        {
            if (ctx->callback_depth)
                const_cast<AROSMoira *>(this)->stopFault("callback memory access", instructionPC);
            return false;
        }
        return true;
    }

    u8 read8(u32 addr) const override
    {
        if (ctx->callback_failed) return 0;
        if ((addr & CUSTOM_MASK) == CUSTOM_BASE)
        {
            u16 w = customRead16(addr & 0xFFE);
            return (addr & 1) ? (u8)w : (u8)(w >> 8);
        }
        if (!guestAccess(addr, 1)) return 0;
        return ctx->mem[addr];
    }

    u16 read16(u32 addr) const override
    {
        if (ctx->callback_failed) return 0;
        if (Kaffeine_IsCallbackReturn(addr)) return M68K_OP_NOP;
        if (addr >= SENTINEL_ADDR && addr < SENTINEL_ADDR + 2)
        {
            const_cast<AROSMoira*>(this)->ctx->running = FALSE;
            if (ctx->callback_depth)
            {
                ctx->callback_failed = TRUE;
                ctx->exit_code = (LONG)getD(0);
            }
            return M68K_OP_STOP;
        }
        if ((addr & CUSTOM_MASK) == CUSTOM_BASE)
            return customRead16(addr & 0xFFE);
        return guestAccess(addr, 2) ? m68k_read16(ctx, addr) : 0;
    }

    void write8(u32 addr, u8 val) const override
    {
        if (ctx->callback_failed) return;
        if ((addr & CUSTOM_MASK) == CUSTOM_BASE) return;
        if (!guestAccess(addr, 1)) return;
        ctx->mem[addr] = val;
    }

    void write16(u32 addr, u16 val) const override
    {
        if (ctx->callback_failed) return;
        if ((addr & CUSTOM_MASK) == CUSTOM_BASE)
        {
            customWrite16(addr & 0xFFE, val);
            return;
        }
        if (guestAccess(addr, 2)) m68k_write16(ctx, addr, val);
    }

    u16 read16OnReset(u32 addr) const override
    {
        return m68k_read16(ctx, addr);
    }

    u16 read16Dasm(u32 addr) const override
    {
        return m68k_read16(ctx, addr);
    }

    /*
     * willExecute is called BEFORE Moira processes the LINEA exception.
     * After we return, Moira will:
     *   1. Push SR and (reg.pc - 2) onto the m68k stack
     *   2. Jump to the LINEA exception vector (vector 10, address 0x28)
     *
     * The m68k code got here via JSR -offset(A6), which already pushed
     * the return address onto the stack. So the stack currently has:
     *   [SP] = return address from JSR
     *
     * Strategy:
     *   - Pop the JSR return address from the stack
     *   - Run the thunk, set D0
     *   - Set reg.pc0 = return_addr so Moira pushes return_addr
     *     in the 68020 exception frame (68000 uses reg.pc-2 instead,
     *     so we also set reg.pc = return_addr + 2 for compatibility)
     *   - LINEA vector points to an RTE instruction at RTE_ADDR
     *   - RTE pops the exception frame -> resumes at return_addr
     */
    void willExecute(M68kException exc, u16 vector) override
    {
        if (ctx->callback_failed) return;
        if (exc == M68kException::LINEF)
        {
            stopFault((getIRD() & 0xFE00) == 0xF200 ?
                      "unsupported FPU instruction (Line-F vector 11)" :
                      "unsupported Line-F instruction (vector 11)", getPC0());
            return;
        }
        if (exc != M68kException::LINEA)
        {
            if (ctx->callback_depth) stopFault("callback CPU exception", getPC0());
            return;
        }

        lineaDispatched = false;

        /* Restore LINEA vector to normal RTE return (may have been
           redirected by a previous Supervisor() call) */
        m68k_write32(ctx, M68K_VEC_LINEA, RTE_ADDR);

        /* Dispatch can recursively execute this CPU. Public D/A/SR results
           belong to the thunk; prefetch, banked stacks and exception state
           still belong to this suspended LINEA instruction. */
        const auto savedReg = reg;
        const auto savedQueue = queue;
        const auto savedFlags = flags;
        const auto savedException = exception;
        const auto savedFC = fcl;
        const auto savedFCSource = fcSource;
        const auto savedCP = cp;
        const auto savedReadBuffer = readBuffer;
        const auto savedWriteBuffer = writeBuffer;
        const auto savedInstructionPC = instructionPC;
        struct KaffeineDispatchResult result;
        if (Kaffeine_DispatchLibraryCall(ctx, &cpu_handle, getPC0(), &result))
        {
            if (!ctx->running)
            {
                if (ctx->callback_depth || ctx->callback_failed)
                    Kaffeine_StopGuest(ctx, &cpu_handle, (LONG)getD(0));
                return;
            }
            reg.usp = savedReg.usp;
            reg.isp = savedReg.isp;
            reg.msp = savedReg.msp;
            reg.pc = savedReg.pc;
            reg.pc0 = savedReg.pc0;
            queue = savedQueue;
            flags = savedFlags;
            exception = savedException;
            fcl = savedFC;
            fcSource = savedFCSource;
            cp = savedCP;
            readBuffer = savedReadBuffer;
            writeBuffer = savedWriteBuffer;
            instructionPC = savedInstructionPC;
            /* A nested redirect may have left the shared vector pointing
               at its own trampoline. This suspended call owns it again. */
            m68k_write32(ctx, M68K_VEC_LINEA, RTE_ADDR);
            if (result.supervisor)
            {
                dispatchSupervisor(result.return_pc);
                return;
            }

            /* Moira owns the exception frame and the RTE return sequence.
               68020 pushes PC0; 68000 pushes PC - 2. */
            setPC0(result.return_pc);
            setPC(result.return_pc + 2);
            lineaDispatched = true;
            return;
        }

        if (ctx->callback_depth)
        {
            stopFault("unhandled callback Line-A", getPC0());
            return;
        }

        /* Not in any library — unknown A-line. Let Moira handle it normally.
         * Point LINEA vector to RTE so it just returns. */
    }

    /*
     * Moira took the SR for the exception frame before willExecute ran the
     * thunk, and the RTE at RTE_ADDR restores that frame. A library call
     * returns the condition codes the library left, so copy the CCR of the
     * live SR, which the thunk may have set, into the frame. A thunk that
     * does not touch the CCR leaves it as the caller had it.
     */
    void didExecute(M68kException exc, u16 vector) override
    {
        if (ctx->callback_failed) return;
        if (exc != M68kException::LINEA || !lineaDispatched) return;
        lineaDispatched = false;

        u32 sp = getA(7);
        u16 framed = m68k_read16(ctx, sp);
        m68k_write16(ctx, sp, (u16)((framed & 0xFF00) | (getSR() & 0x00FF)));
    }

    void cpuDidHalt() override
    {
        /* Moira also consumes some double faults itself and calls halt(). */
        stopFault("CPU halt (double fault)", instructionPC);
    }
};

ULONG moira_get_d(void *state, int reg)
{
    return static_cast<AROSMoira *>(state)->getD(reg);
}

ULONG moira_get_a(void *state, int reg)
{
    return static_cast<AROSMoira *>(state)->getA(reg);
}

void moira_set_d(void *state, int reg, ULONG value)
{
    static_cast<AROSMoira *>(state)->setD(reg, (u32)value);
}

void moira_set_a(void *state, int reg, ULONG value)
{
    static_cast<AROSMoira *>(state)->setA(reg, (u32)value);
}

UWORD moira_get_sr(void *state)
{
    return static_cast<AROSMoira *>(state)->getSR();
}

void moira_set_sr(void *state, UWORD value)
{
    static_cast<AROSMoira *>(state)->setSR((u16)value);
}

ULONG moira_get_pc(void *state)
{
    return static_cast<AROSMoira *>(state)->getPC();
}

void moira_set_pc(void *state, ULONG value)
{
    auto *cpu = static_cast<AROSMoira *>(state);
    cpu->setPC((u32)value);
    cpu->setPC0((u32)value);
    cpu->instructionPC = (u32)value;
    cpu->setIRD(cpu->read16((u32)value));
    cpu->setIRC(cpu->read16((u32)value + 2));
}

void moira_execute(void *state) noexcept
{
    auto *cpu = static_cast<AROSMoira *>(state);
    const u32 pc = cpu->getPC0();
    const u16 opcode = cpu->getIRD();
    cpu->instructionPC = pc;
    if (Kaffeine_IsCallbackReturn(pc) ||
        (cpu->ctx->callback_depth && ((pc & 1) || pc >= cpu->ctx->mem_size ||
          cpu->ctx->mem_size - pc < 2)))
    {
        cpu->stopFault("invalid callback PC", pc);
        Kaffeine_StopGuest(cpu->ctx, &cpu->cpu_handle, RETURN_FAIL);
        return;
    }

    /* Best-effort safety net on hosts with a working unwinder. AROS module
       frames are not registered today, so normal fault paths must not throw.
       Catch here before any unwind could enter the C Kaffeine_RunCPU(). */
    try
    {
        cpu->execute();
    }
    catch (const AddressError &)
    {
        cpu->stopFault("address error", pc);
    }
    catch (const BusError &)
    {
        cpu->stopFault("bus error", pc);
    }
    catch (const DoubleFault &)
    {
        cpu->stopFault("double fault", pc);
    }
    catch (const DoubleFault *)
    {
        /* processException() can throw its dynamic_cast result, a pointer. */
        cpu->stopFault("double fault", pc);
    }
    catch (const std::exception &exc)
    {
        /* Moira rethrows by value as std::exception, losing runtime_error's
           message. Coprocessor ID 1 identifies the FPU opcode at entry. */
        cpu->stopFault((opcode & 0xFE00) == 0xF200 ?
                       "unsupported FPU instruction" : "backend exception", pc, exc.what());
    }
    catch (...)
    {
        cpu->stopFault("unknown backend exception", pc);
    }
    /* The core can finish the current instruction after a nonthrowing
       memory fault or a stopped nested dispatch. Keep its process result. */
    if (cpu->ctx->callback_failed || cpu->faulted)
        cpu->setD(0, (u32)cpu->ctx->exit_code);
}

/* Moira fetches every instruction from guest memory: no flush_code. */
const struct KaffeineCPUOps moira_cpu_ops =
{
    moira_get_d, moira_get_a, moira_set_d, moira_set_a,
    moira_get_sr, moira_set_sr, moira_get_pc, moira_set_pc, moira_execute,
    nullptr
};

/* A complete Moira for each AddTask() child (kaffeine_sched.c), so that no
   register, FPU or exception state is shared. Reset leaves it in
   supervisor mode; the scheduler sets SR, A7 and PC. */
struct KaffeineCPU *moira_task_create(struct KaffeineContext *ctx) noexcept
{
    AROSMoira *cpu = nullptr;
    try
    {
        /* nothrow covers allocation of this object, but its constructor
           can also allocate backend state and throw. Keep both inside
           the C++ boundary before returning to the C scheduler. */
        cpu = new (std::nothrow) AROSMoira(ctx);
        if (!cpu)
            return nullptr;
        cpu->reset();
    }
    catch (...)
    {
        delete cpu;
        return nullptr;
    }
    return &cpu->cpu_handle;
}

void moira_task_destroy(struct KaffeineContext *, struct KaffeineCPU *handle) noexcept
{
    delete static_cast<AROSMoira *>(handle->backend_state);
}

const struct KaffeineCPUFactory moira_cpu_factory = { moira_task_create, moira_task_destroy };

} /* anonymous namespace */

extern "C" {

LONG Kaffeine_Execute(struct KaffeineContext *ctx)
try
{
    AROSMoira cpu(ctx);

    m68k_write32(ctx, 0, ctx->stack_top);
    m68k_write32(ctx, 4, ctx->entry_point);
    cpu.reset();

    /* Restore AbsExecBase at address 4 */
    m68k_write32(ctx, 4, ctx->m68k_sysbase);

    /* Write RTE at RTE_ADDR for LINEA exception return */
    m68k_write16(ctx, RTE_ADDR, M68K_OP_RTE);

    /* Supervisor trampoline at SV_TRAMP_ADDR:
       ADDQ.L #8,SP   (508F)  — skip Moira's 68020 exception frame
       JMP    (A5)     (4ED5)  — jump to user function in supervisor mode */
    m68k_write16(ctx, SV_TRAMP_ADDR,     M68K_OP_ADDQ8_SP);
    m68k_write16(ctx, SV_TRAMP_ADDR + 2, M68K_OP_JMP_A5);

    /* Set LINEA exception vector to RTE_ADDR */
    m68k_write32(ctx, M68K_VEC_LINEA, RTE_ADDR);

    /* Push sentinel return address */
    u32 sp = cpu.getA(7);
    sp -= 4;
    m68k_write32(ctx, sp, SENTINEL_ADDR);
    cpu.setA(7, sp);

    /* AROS_PROCH entry: A0=argptr, D0=argsize, A6=SysBase */
    cpu.setA(0, ctx->m68k_argptr);
    cpu.setD(0, ctx->m68k_argsize);
    cpu.setA(6, ctx->m68k_sysbase);

    /* AddTask() children get CPUs of their own while this one runs; the
       loop destroys them before it returns. */
    ctx->cpu_factory = &moira_cpu_factory;
    LONG result = Kaffeine_RunCPU(ctx, &cpu.cpu_handle);
    ctx->cpu_factory = nullptr;
    /* RunCPU returns guest D0 even after a halt. A fault takes precedence. */
    ctx->exit_code = cpu.faulted ? RETURN_FAIL : result;
    return ctx->exit_code;
}
catch (const std::exception &exc)
{
    /* Construction, model selection and reset precede the C run loop. */
    bug("[kaffeine] CPU initialization exception at entry PC=0x%08lx: %s\n",
        (unsigned long)ctx->entry_point, exc.what());
    ctx->running = FALSE;
    return ctx->exit_code = RETURN_FAIL;
}
catch (...)
{
    bug("[kaffeine] Unknown CPU initialization exception at entry PC=0x%08lx\n",
        (unsigned long)ctx->entry_point);
    ctx->running = FALSE;
    return ctx->exit_code = RETURN_FAIL;
}

} /* extern "C" */
