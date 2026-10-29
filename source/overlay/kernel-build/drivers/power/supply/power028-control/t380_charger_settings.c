/* SPDX-License-Identifier: GPL-2.0-only */
#include "t380_charger_settings.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

/* Logical mapping, NOT a hardware programming sequence. CNTL mask excludes
 * operation-mode bits0..2. No CHGCNTL1, OTG, boost, GPIO or enable setting. */
static const int addresses[T380_CS_COUNT] = { 0x0c, 0x0d, 0x0f, 0x10, 0x11, 0x12, 0x13 };
static const int masks[T380_CS_COUNT] = { 0x10, 0x3f, 0x3f, 0x1f, 0xf8, 0xe7, 0x0c };

int t380_cs_encode_profile(const struct t380_cs_limits *request,
                          enum t380_cs_profile profile, struct t380_cs_plan *plan)
{
    struct t380_cs_plan encoded = { 0 };
    struct t380_cs_limits r;
    int i;

    if (!plan)
        return -EINVAL;
    if (!request || (profile != T380_CS_BOARD_INIT &&
                     profile != T380_CS_MANAGED_ATTACHED && profile != T380_CS_STOCK_ATTACHED)) {
        *plan = encoded;
        return -EINVAL;
    }
    r = *request; /* Permit request == &plan->requested without losing input. */
    *plan = encoded;
    if (r.input_ma < 100 || r.input_ma > 2100 ||
        r.charge_ma < 100 || r.charge_ma > 2100 ||
        r.float_mv < 4120 || r.float_mv > 4300 ||
        r.topoff_ma < 100 || r.topoff_ma > 350)
        return -ERANGE;
    /* Termination threshold is not an upper-current limit: no rounding. */
    if ((r.topoff_ma - 100) % 25)
        return -EINVAL;
    encoded.profile = profile;
    encoded.requested = r;
    for (i = 0; i < T380_CS_COUNT; ++i) {
        encoded.fields[i].address = addresses[i];
        encoded.fields[i].mask = masks[i];
    }
    /* Active stock ONLINE setter: AUTOSET0 attached, board value1 unplug.
     * This selects a masked field only, not hardware order or permission. */
    encoded.fields[T380_CS_CNTL].value =
        profile == T380_CS_BOARD_INIT ? 0x10 : 0x00;
    if (profile == T380_CS_STOCK_ATTACHED) {
        /* Samsung charging operation mode5; independent GPIO remains HIGH
         * through the whole programming/readback transaction. */
        encoded.fields[T380_CS_CNTL].mask = 0x17;
        encoded.fields[T380_CS_CNTL].value = 0x05;
    }
    encoded.fields[T380_CS_VBUSCNTL].value = (r.input_ma - 100) / 50;
    encoded.fields[T380_CS_CHGCNTL2].value = (r.charge_ma - 100) / 50;
    /* Pinned active source is linear4120..4430mV; conservative cap4300. */
    encoded.fields[T380_CS_CHGCNTL3].value = (r.float_mv - 4120) / 10;
    encoded.fields[T380_CS_CHGCNTL4].value = 0x80 | (((r.topoff_ma - 100) / 25) << 3);
    encoded.fields[T380_CS_CHGCNTL5].value = 0xe0; /* AICLEN1,45min,4300mV. */
    encoded.fields[T380_CS_CHGCNTL6].value = 0x08; /* Stock code2:1500kHz. */
    encoded.effective.input_ma = 100 + encoded.fields[T380_CS_VBUSCNTL].value * 50;
    encoded.effective.charge_ma = 100 + encoded.fields[T380_CS_CHGCNTL2].value * 50;
    encoded.effective.float_mv = 4120 + encoded.fields[T380_CS_CHGCNTL3].value * 10;
    encoded.effective.topoff_ma = 100 + ((encoded.fields[T380_CS_CHGCNTL4].value & 0x78) >> 3) * 25;
    encoded.valid = 1;
    *plan = encoded;
    return 0;
}

int t380_cs_encode(const struct t380_cs_limits *request, struct t380_cs_plan *plan)
{
    return t380_cs_encode_profile(request, T380_CS_BOARD_INIT, plan);
}

static int check_plan(const struct t380_cs_plan *plan)
{
    struct t380_cs_plan expected;
    int i;

    if (!plan || plan->valid != 1 ||
        t380_cs_encode_profile(&plan->requested, plan->profile, &expected))
        return -EINVAL;
    if (plan->effective.input_ma != expected.effective.input_ma ||
        plan->effective.charge_ma != expected.effective.charge_ma ||
        plan->effective.float_mv != expected.effective.float_mv ||
        plan->effective.topoff_ma != expected.effective.topoff_ma)
        return -EINVAL;
    for (i = 0; i < T380_CS_COUNT; ++i)
        if (plan->fields[i].address != expected.fields[i].address ||
            plan->fields[i].mask != expected.fields[i].mask ||
            plan->fields[i].value != expected.fields[i].value)
            return -EINVAL;
    return 0;
}

static int check_bytes(const int raw[T380_CS_COUNT])
{
    int i;

    if (!raw)
        return -EINVAL;
    for (i = 0; i < T380_CS_COUNT; ++i) {
        if (raw[i] < 0)
            return raw[i];
        if (raw[i] > 255)
            return -EINVAL;
    }
    return 0;
}

int t380_cs_merge(const struct t380_cs_plan *plan, const int old[T380_CS_COUNT],
                  int merged[T380_CS_COUNT])
{
    int result[T380_CS_COUNT], i, ret;

    if (!merged)
        return -EINVAL;
    ret = check_plan(plan);
    if (!ret)
        ret = check_bytes(old);
    if (ret) {
        /* -1 is not a valid register value; stale output cannot look usable. */
        for (i = 0; i < T380_CS_COUNT; ++i)
            merged[i] = -1;
        return ret;
    }
    for (i = 0; i < T380_CS_COUNT; ++i)
        result[i] = (old[i] & ~plan->fields[i].mask) | plan->fields[i].value;
    for (i = 0; i < T380_CS_COUNT; ++i)
        merged[i] = result[i]; /* In-place old/merged alias is supported. */
    return 0;
}

int t380_cs_decode(const int raw[T380_CS_COUNT], struct t380_cs_decoded *decoded)
{
    static const int timers[4] = { 10, 20, 30, 45 };
    static const int frequencies[4] = { 3000, 2400, 1500, 1800 };
    struct t380_cs_decoded d = { 0 };
    int ret;

    if (!decoded)
        return -EINVAL;
    *decoded = d;
    ret = check_bytes(raw);
    if (ret)
        return ret;
    d.input_code = raw[T380_CS_VBUSCNTL] & 0x3f;
    d.fast_code = raw[T380_CS_CHGCNTL2] & 0x3f;
    d.stock_arithmetic.input_ma = 100 + d.input_code * 50;
    d.stock_arithmetic.charge_ma = 100 + d.fast_code * 50;
    d.fast_in_stock_setter_range = d.fast_code <= 48;
    d.stock_arithmetic.float_mv = 4120 + (raw[T380_CS_CHGCNTL3] & 0x1f) * 10;
    d.stock_arithmetic.topoff_ma = 100 + ((raw[T380_CS_CHGCNTL4] & 0x78) >> 3) * 25;
    d.autoset = !!(raw[T380_CS_CNTL] & 0x10);
    d.autostop = !!(raw[T380_CS_CHGCNTL4] & 0x80);
    d.aiclen = !!(raw[T380_CS_CHGCNTL5] & 0x80);
    d.aicl_code = raw[T380_CS_CHGCNTL5] & 0x07;
    d.aicl_supported = d.aicl_code <= 6;
    d.aicl_mv = d.aicl_supported ? 4300 + d.aicl_code * 100 : -1;
    d.topoff_minutes = timers[(raw[T380_CS_CHGCNTL5] & 0x60) >> 5];
    d.frequency_khz = frequencies[(raw[T380_CS_CHGCNTL6] & 0x0c) >> 2];
    d.valid = 1;
    *decoded = d;
    return 0;
}

int t380_cs_verify(const struct t380_cs_plan *plan, const int raw[T380_CS_COUNT],
                   struct t380_cs_verification *verification)
{
    struct t380_cs_verification v = { 0 };
    int i, ret;

    if (!verification)
        return -EINVAL;
    *verification = v;
    ret = check_plan(plan);
    if (ret)
        return ret;
    ret = t380_cs_decode(raw, &v.observed);
    if (ret)
        return ret;
    for (i = 0; i < T380_CS_COUNT; ++i)
        if ((raw[i] & plan->fields[i].mask) != plan->fields[i].value)
            v.mismatch |= 1u << i;
    v.valid = !v.mismatch;
    *verification = v;
    return v.valid ? 0 : -EIO;
}
