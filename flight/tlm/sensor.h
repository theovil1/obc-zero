/*
 * Sensors, and the three ways one lies.
 *
 * ADR 0008 decision 4 is the whole of this file. "A sensor returning garbage"
 * is not one failure, it is three, and they are not caught by the same means:
 *
 *   out of range                   caught here, by a bound
 *   stuck on a plausible value     caught here, by a run length
 *   oscillating between plausible  NOT CAUGHT, and will not be at M6
 *
 * The third needs a model of how fast the measured quantity can physically
 * change. This project does not have one and cannot invent one from an emulator;
 * a rate limit written without it would flag real transients and miss real
 * faults, at a threshold chosen by whoever typed it. So it is an open gap, named
 * as one, rather than a case quietly believed to be covered.
 *
 * The flag therefore means "out of range or stuck", and a report that says "the
 * sensor lied and was caught" must say which.
 *
 * Copyright 2026 Théo Vilain
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef OBC_TLM_SENSOR_H
#define OBC_TLM_SENSOR_H

#include <stdint.h>

#include "core/status.h"

#define OBC_SENSOR_COUNT 2u
#define OBC_SENSOR_TEMP 0u
#define OBC_SENSOR_RATE 1u

/*
 * What appears in the frame when a reading is not trustworthy.
 *
 * A distinguished value rather than the last good one. Substituting the previous
 * reading would put a plausible number in the frame with a flag beside it, and
 * every consumer that forgets to look at the flag would carry on working — which
 * is the definition of propagating the fault. This value is unmistakable.
 */
#define OBC_SENSOR_INVALID 0xFFFFu

/*
 * Flag bits, three per sensor, packed into the frame's one flag byte.
 *
 * The third bit exists because of a question the ADR forces: what does the frame
 * contain before the sensor has ever been read? Reusing the out-of-range bit
 * would be a lie about which failure happened, and leaving the flags clear would
 * break the invariant that an unflagged reading is a real one — the direction
 * that stops the flag becoming decorative.
 */
#define OBC_SENSOR_FLAG_BITS 3u
#define OBC_SENSOR_FLAG_RANGE(i) ((uint32_t)1u << ((i) * OBC_SENSOR_FLAG_BITS + 0u))
#define OBC_SENSOR_FLAG_STUCK(i) ((uint32_t)1u << ((i) * OBC_SENSOR_FLAG_BITS + 1u))
#define OBC_SENSOR_FLAG_NODATA(i) ((uint32_t)1u << ((i) * OBC_SENSOR_FLAG_BITS + 2u))

/*
 * How many identical consecutive readings mean "stuck".
 *
 * **Arbitrary, and declared so**, on the same footing as OBC_SHORT_BOOT_LIMIT in
 * ADR 0007. A genuinely constant quantity would trip this; the honest defence is
 * that nothing here measures one, not that the number was derived. It is a
 * figure to calibrate against a real sensor.
 *
 * An injector is a program and this threshold is part of it: a campaign that
 * only ever holds a value for fewer readings than this has not tested the stuck
 * detector, it has tested that the detector stays quiet.
 */
#define OBC_SENSOR_STUCK_LIMIT 8u

/*
 * The plausible range, per sensor, held beside the offset the reading is written
 * to. One table: the bound a value is checked against and the place it lands
 * cannot drift apart if there is nowhere for them to drift to.
 */
typedef struct {
    const char *name;
    uint16_t min; /* inclusive */
    uint16_t max; /* inclusive */
} obc_sensor_desc_t;

extern const obc_sensor_desc_t obc_sensor_desc[OBC_SENSOR_COUNT];

/*
 * The stuck threshold as a symbol, for the same reason the sync word is one: the
 * host has to assert "not flagged before N samples, flagged at N", and an N
 * restated in the checker would keep passing after this one changed. A macro is
 * not reliably in the debug info; a symbol is.
 */
extern const uint32_t obc_sensor_stuck_limit;

/* Last validated reading, and the flags that say whether it is one. */
extern volatile uint16_t obc_sensor_value[OBC_SENSOR_COUNT];
extern volatile uint32_t obc_sensor_flags;

/* Rejections, per sensor and per kind, so a campaign can report which. */
extern volatile uint32_t obc_sensor_rejects_range[OBC_SENSOR_COUNT];
extern volatile uint32_t obc_sensor_rejects_stuck[OBC_SENSOR_COUNT];

/*
 * The mock backend.
 *
 * `sifive_e` has no sensors, so there is no real backend to prefer over this
 * one; the harness writes raw counts here through the debugger and the flight
 * code reads them exactly as it would read a device register. What is being
 * tested is the validation path, and that path does not know where its bytes
 * came from.
 *
 * Poisoned rather than zeroed at reset, so that a validator which happens to
 * accept zero is not accidentally correct on the first frame.
 */
extern volatile uint16_t obc_sensor_mock_raw[OBC_SENSOR_COUNT];

/* Clears history and flags to the never-read state. */
void obc_sensor_init(void);

/*
 * Reads every sensor once and validates it. Bounded by OBC_SENSOR_COUNT.
 *
 * Returns OBC_ERR_UNSTABLE if any reading was rejected, so the caller can act on
 * it rather than having to inspect the flag word to find out. The frame carries
 * the detail; the status carries the fact.
 */
OBC_MUST_CHECK obc_status_t obc_sensor_sample(void);

#endif /* OBC_TLM_SENSOR_H */
