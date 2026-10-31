/* Copyright (C) 2026, The AROS Development Team. All rights reserved.
   Author: Michele Dipace (@kaffeine1) */
#ifndef KAFFEINE_PAINT_COMPILE_H
#define KAFFEINE_PAINT_COMPILE_H

#include <stddef.h>
#include <stdint.h>

#define KPAINT_VERSION 1u
#define KPAINT_RASTPORT_BYTES 100u
#define KPAINT_MAX_NODES 2048u
#define KPAINT_MAX_DEPS 512u
#define KPAINT_MAX_CELLS 16u
#define KPAINT_MAX_PATHS 16u
#define KPAINT_MAX_COMMANDS 32u
#define KPAINT_MAX_CLONES 2u
#define KPAINT_STACK_BYTES 1024u
#define KPAINT_MAX_STEPS 4096u
#define KPAINT_MAX_CALLS 8u

#ifdef __cplusplus
extern "C" {
#endif

enum KaffeinePaintStatus {
    KPAINT_OK, KPAINT_INPUT, KPAINT_WORKSPACE, KPAINT_BOUNDS,
    KPAINT_UNSUPPORTED, KPAINT_LIMIT, KPAINT_STACK, KPAINT_STORE,
    KPAINT_DATAFLOW, KPAINT_BAD_CLONE, KPAINT_RECT, KPAINT_CHANGED,
    KPAINT_SIGNAL
};
enum KaffeinePaintDependencyKind {
    KPAINT_CODE = 1, KPAINT_HEADER, KPAINT_POINTER, KPAINT_IMMUTABLE_DATA
};
enum KaffeinePaintCellRole {
    KPAINT_CELL_GENERAL = 0, KPAINT_CELL_BYTE = 1,
    KPAINT_CELL_OWNER_CAP = 2, KPAINT_CELL_IMMUTABLE = 4
};
enum KaffeinePaintCommandKind {
    KPAINT_CLONE = 1, KPAINT_APEN, KPAINT_DRAWMODE, KPAINT_MASK,
    KPAINT_RECTFILL, KPAINT_DEFER_SIGNAL
};

/* Read-only guest access on the driver. read must copy exactly size bytes
 * in guest (big-endian) order. ordinary approves external scalar data, not
 * executable/header provenance. No native Hook/RastPort/Task enters this API. */
struct KaffeinePaintSource {
    void *opaque;
    int (*read)(void *opaque, uint32_t address, void *out, uint32_t size);
    int (*ordinary)(void *opaque, uint32_t address, uint32_t size);
    uint32_t arena_size, hook, exec_base, graphics_base;
};
struct KaffeinePaintReport {
    enum KaffeinePaintStatus status;
    uint32_t pc, steps;
};
struct KaffeinePaintDependency {
    uint32_t address, expected;
    uint8_t width, kind;
    uint16_t reserved;
};
struct KaffeinePaintCell {
    uint32_t address, initial, used_mask;
    uint8_t width, role;
    uint8_t reserved[2];
};
/* Immutable integer expression DAG. Fields are exposed for serialization
 * and bounded snapshot collection, not as a supported hand-built plan ABI. */
struct KaffeinePaintExpr {
    uint32_t value;
    uint16_t a, b;
    uint8_t op, width, shift, reserved;
};
struct KaffeinePaintAction {
    uint16_t value[4];
    uint8_t kind, clone;
    uint16_t reserved;
};
struct KaffeinePaintPath {
    uint16_t guard, count;
    struct KaffeinePaintAction commands[KPAINT_MAX_COMMANDS];
};
struct KaffeinePaintPlan {
    uint32_t version, signal_mask;
    uint16_t nodes, dependencies, cells, paths;
    uint8_t rp_used[KPAINT_RASTPORT_BYTES];
    struct KaffeinePaintExpr expressions[KPAINT_MAX_NODES];
    struct KaffeinePaintDependency dependency[KPAINT_MAX_DEPS];
    struct KaffeinePaintCell cell[KPAINT_MAX_CELLS];
    struct KaffeinePaintPath path[KPAINT_MAX_PATHS];
};

/* All values must come from one coherent caller-owned snapshot. Dependency
 * entries correspond exactly to plan.dependency[], in big-endian numeric
 * order. Cells stay dynamic. used_mask identifies required bits: a low pen
 * byte may be copied alone; mutable multi-byte controls require an actual
 * coherent-publication capability. OWNER_CAP cells must contain only zero
 * or owner_task, and zero must select a no-Signal path. IMMUTABLE cells are
 * also equality-checked dependencies. UP Forbid alone does not establish that
 * an emulated multi-byte write was not already preempted halfway through.
 *
 * rp is a sanitized classic snapshot with guest tokens in pointer fields,
 * never native pointer bytes. Pointer/private RP bytes are copy-only;
 * rp_used marks the scalar bytes actually needed for evaluation. bounds are the actual native invocation's
 * signed WORD bounds; the original native RP itself stays outside this API.
 * owner_cookie is an opaque numeric generation, not a native address. */
struct KaffeinePaintInputs {
    uint32_t dependencies[KPAINT_MAX_DEPS], cells[KPAINT_MAX_CELLS];
    uint8_t rp[KPAINT_RASTPORT_BYTES];
    int16_t bounds[4];
    int32_t offset_x, offset_y;
    uint32_t layer_token, owner_task, allowed_signals;
    uint64_t owner_cookie;
};
/* Command slots: CLONE has no values and resets clone ID (0..1) by copying
 * the actual native RP and clearing Layer. APEN/DRAWMODE/MASK use value[0]
 * (UBYTE). RECTFILL uses signed WORD x0,y0,x1,y1 in value[0..3].
 * DEFER_SIGNAL uses target token/bits in value[0..1] and owner_cookie.
 * plan.signal_mask is the finite union to lease while the recipe is installed. */
struct KaffeinePaintCommand {
    uint32_t value[4];
    uint64_t owner_cookie;
    uint8_t kind, clone;
    uint8_t reserved[6];
};
struct KaffeinePaintOutput {
    uint32_t count;
    struct KaffeinePaintCommand commands[KPAINT_MAX_COMMANDS];
};

size_t Kaffeine_PaintCompileWorkspaceSize(void);
size_t Kaffeine_PaintEvaluateWorkspaceSize(void);
size_t Kaffeine_PaintWorkspaceAlignment(void);

/* Compile without executing guest code. Initial ABI: A0=Hook, A1=20-byte
 * BackFillMessage, A2=original classic RP token, A7=private abstract stack.
 * The result models only local stack effects and the four typed LVOs:
 * graphics SetAPen342/SetDrMd354/RectFill306 and Exec Signal324. Every path
 * must return finitely. Other stores/calls/unproven targets are refused.
 * No output plan is changed on failure. Workspaces need Kaffeine_PaintWorkspaceAlignment() alignment. */
enum KaffeinePaintStatus Kaffeine_PaintCompile(
    const struct KaffeinePaintSource *, void *workspace, size_t workspace_size,
    struct KaffeinePaintPlan *out, struct KaffeinePaintReport *report);

/* No allocation, guest arena access, OS calls or CPU execution. Checks all
 * immutable dependencies, selected-path inputs, clone and rectangle/signal
 * constraints BEFORE publishing any output. No output changes on failure.
 * The native executor must clone its ACTUAL RP, clear Layer, replay commands
 * in order, and defer signals to the captured live private guest owner.
 * Native lifetime leases, snapshot publication and owner-generation checks
 * remain caller responsibilities. This API makes no SMP claim. */
enum KaffeinePaintStatus Kaffeine_PaintEvaluate(
    const struct KaffeinePaintPlan *, const struct KaffeinePaintInputs *,
    void *workspace, size_t workspace_size, struct KaffeinePaintOutput *out);

#ifdef __cplusplus
}
#endif
#endif
