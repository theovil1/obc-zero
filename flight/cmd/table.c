/*
 * The command table.
 *
 * Compile-time constant, in flash, and audited both on the ground and at init.
 * The split is ADR 0012 decision 2: everything here that a vehicle could not
 * repair is a build property and is checked against the binary by
 * `make cmd-table-check`; whether a *particular* argument is in range is decided
 * per frame, in flight, and is a refusal.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include "cmd/frame.h"
#include "cmd/queue.h"
#include "core/mode.h"
#include "core/status.h"

/* What the commands do. Deliberately small: M7 is about the ingest path, and
 * giving these real effects would make the fuzz campaign's "nothing moved"
 * assertion depend on what a handler happens to touch. */
volatile uint32_t obc_cmd_noop_arg;
volatile uint32_t obc_cmd_ping_count;
volatile uint32_t obc_cmd_arm_target;
volatile uint32_t obc_cmd_safe_requested;

static void cmd_ping(uint32_t arg)
{
    (void)arg;
    if (obc_cmd_ping_count < OBC_CMD_COUNTER_MAX) {
        obc_cmd_ping_count++;
    }
}

static void cmd_set(uint32_t arg)
{
    obc_cmd_noop_arg = arg;
}

/*
 * Arms the opcode named by the argument.
 *
 * The arming itself is not critical — it does nothing on its own, and requiring
 * an arm to arm an arm is a regress with no end. What it does is make a critical
 * command need two accepted frames in sequence, so a single corrupted opcode
 * cannot reach one.
 */
static void cmd_arm(uint32_t arg)
{
    obc_cmd_arm_target = arg;
}

/*
 * The one critical command. Requests safe mode, which is the most consequential
 * thing a ground station can ask of this system and the natural candidate for
 * arm-then-execute.
 *
 * It records the request rather than entering safe mode directly: entering is
 * the executive's to do, on its own terms, and a handler that changed mode from
 * inside a dispatch would be a command path reaching into the recovery path —
 * which ADR 0011 spent a record forbidding.
 */
static void cmd_safe(uint32_t arg)
{
    (void)arg;
    obc_cmd_safe_requested = 1u;
}

#define OP_PING 0x10u
#define OP_SET 0x11u
#define OP_ARM 0x12u
#define OP_SAFE 0x13u

const obc_cmd_t obc_cmd_table[] = {
    { "ping", OP_PING, 0u, { 0, 0 }, 0u, 0u, cmd_ping },
    { "set", OP_SET, 0u, { 0, 0 }, 100u, 900u, cmd_set },
    { "arm", OP_ARM, 0u, { 0, 0 }, OP_PING, OP_SAFE, cmd_arm },
    { "safe", OP_SAFE, 1u, { 0, 0 }, 0u, 0u, cmd_safe },
};

const uint32_t obc_cmd_count =
    (uint32_t)(sizeof(obc_cmd_table) / sizeof(obc_cmd_table[0]));

/* --- Compile-time checks ------------------------------------------------- */

/*
 * Expressible in C, so it costs nothing and fails at the desk. ADR 0012 keeps
 * the *other* table properties — every opcode has a handler, no duplicates — on
 * the ground, because a function pointer in a table row is not an integer
 * constant expression and a _Static_assert that merely counted rows would be
 * decorative.
 */
_Static_assert(sizeof(obc_cmd_table) / sizeof(obc_cmd_table[0]) > 0u,
               "an empty command table would accept nothing and reject nothing");

/* One argument, one word, and the frame has room for exactly that. */
_Static_assert(OBC_CMD_W_ARG == 4u,
               "the frame's argument field no longer holds a full word");

/* The arm command's range has to be able to name every opcode in the table, or
 * some critical command is unreachable by construction and the arming mechanism
 * is decorative for it. */
_Static_assert(OP_PING <= OP_SAFE,
               "the arm command cannot name every opcode in the table");
