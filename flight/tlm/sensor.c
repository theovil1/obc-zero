/*
 * Sensor sampling and validation.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tlm/sensor.h"

#include <stdint.h>

#include "core/status.h"
#include "tlm/frame.h"

/*
 * Ranges. Raw counts, not physical units: converting to degrees on the vehicle
 * would put a scale factor in flight code that the ground would then have to
 * agree with, which is a second source of truth wearing a different hat.
 */
OBC_TLM_KEEP const obc_sensor_desc_t obc_sensor_desc[OBC_SENSOR_COUNT] = {
    { "temp", 200u, 3800u },
    { "rate", 100u, 900u },
};

OBC_TLM_KEEP const uint32_t obc_sensor_stuck_limit = OBC_SENSOR_STUCK_LIMIT;

volatile uint16_t obc_sensor_value[OBC_SENSOR_COUNT];
volatile uint32_t obc_sensor_flags;

volatile uint32_t obc_sensor_rejects_range[OBC_SENSOR_COUNT];
volatile uint32_t obc_sensor_rejects_stuck[OBC_SENSOR_COUNT];

/* Written by the harness through the debugger. .noinit so a reset does not
 * quietly hand the validator a zero it never asked for. */
volatile uint16_t obc_sensor_mock_raw[OBC_SENSOR_COUNT]
    __attribute__((section(".noinit")));

/* History for the stuck detector: the previous raw reading and how many
 * consecutive readings have equalled it. */
static uint16_t s_last_raw[OBC_SENSOR_COUNT];
static uint8_t s_run[OBC_SENSOR_COUNT];
static uint8_t s_seen[OBC_SENSOR_COUNT];

/* Three flag bits per sensor have to fit in the frame's one flag byte. Adding a
 * third sensor is caught here rather than by a truncated flag word that reads as
 * "nothing wrong". */
_Static_assert(OBC_SENSOR_COUNT * OBC_SENSOR_FLAG_BITS <= 8u,
               "the sensor flags no longer fit the frame's flag byte");

void obc_sensor_init(void)
{
    uint32_t i;

    obc_sensor_flags = 0u;
    for (i = 0u; i < OBC_SENSOR_COUNT; i++) {
        obc_sensor_value[i] = OBC_SENSOR_INVALID;
        s_last_raw[i] = 0u;
        s_run[i] = 0u;
        s_seen[i] = 0u;
        obc_sensor_flags |= OBC_SENSOR_FLAG_NODATA(i);
    }
}

obc_status_t obc_sensor_sample(void)
{
    obc_status_t worst = OBC_OK;
    uint32_t i;

    for (i = 0u; i < OBC_SENSOR_COUNT; i++) {
        uint16_t raw = obc_sensor_mock_raw[i];
        uint32_t bad = 0u;

        /*
         * Run length first, and it counts every reading including rejected
         * ones. A sensor stuck at an out-of-range value is stuck as well as out
         * of range, and reporting only the range would describe the symptom
         * that is easiest to see rather than the failure that occurred.
         */
        if (s_seen[i] != 0u && raw == s_last_raw[i]) {
            if (s_run[i] < 0xFFu) {
                s_run[i]++;
            }
        } else {
            s_run[i] = 1u;
        }
        s_last_raw[i] = raw;
        s_seen[i] = 1u;

        if (raw < obc_sensor_desc[i].min || raw > obc_sensor_desc[i].max) {
            bad |= OBC_SENSOR_FLAG_RANGE(i);
            obc_sensor_rejects_range[i]++;
        }
        if (s_run[i] >= OBC_SENSOR_STUCK_LIMIT) {
            bad |= OBC_SENSOR_FLAG_STUCK(i);
            obc_sensor_rejects_stuck[i]++;
        }

        /*
         * The two directions, together, because they are one assignment and
         * separating them is how the second gets forgotten:
         *
         *   a flagged reading is replaced by the substitute, never published;
         *   an unflagged reading is the value the sensor actually produced.
         *
         * The never-read bit is cleared here and only here, so a frame with no
         * flags set cannot be one where nothing was ever sampled.
         */
        obc_sensor_flags &= ~(OBC_SENSOR_FLAG_RANGE(i) | OBC_SENSOR_FLAG_STUCK(i)
                              | OBC_SENSOR_FLAG_NODATA(i));
        obc_sensor_flags |= bad;

        if (bad != 0u) {
            obc_sensor_value[i] = OBC_SENSOR_INVALID;
            worst = OBC_ERR_UNSTABLE;
        } else {
            obc_sensor_value[i] = raw;
        }
    }

    return worst;
}
