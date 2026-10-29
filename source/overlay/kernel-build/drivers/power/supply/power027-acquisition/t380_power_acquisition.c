/* SPDX-License-Identifier: GPL-2.0-only */
#include "t380_power_acquisition.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

struct t380_pa_profile t380_pa_conservative_profile(void)
{
    const struct t380_pa_profile profile = { 4, 4190, 4090, 89, 4120, true, false };
    return profile;
}
struct t380_pa_profile t380_pa_stock_default_profile(void)
{
 const struct t380_pa_profile profile = { 0, T380_RF_FLOAT_MV, T380_RF_FULL_MIN_MV,
  T380_RF_FULL_SCALED_SOC, T380_RF_RECHARGE_MV, false, false };
 return profile;
}
static void t380_pa_history_reset(struct t380_pa_history *h)
{
    const struct t380_pa_history empty = {0};
    *h = empty;
}
static void t380_pa_history_update(struct t380_pa_history *h, struct t380_pa_tuple *o)
{
    const struct t380_pa_field_sample *current_sample = &o->field[T380_PA_CURRENT];
    t380_pa_u64 interval;
    long long weighted;
    int ma = current_sample->value / 1000;

    if (!o->tuple_valid) { t380_pa_history_reset(h); return; }
    if (h->samples && (h->epoch != o->epoch_after || current_sample->started_ms <= h->last_ms))
        t380_pa_history_reset(h);
    if (h->samples) {
        interval = current_sample->started_ms - h->last_ms;
        if (interval < T380_PA_HISTORY_MIN_MS || interval > T380_PA_HISTORY_MAX_MS)
            t380_pa_history_reset(h);
    }
    if (!h->samples) {
        /* Seed from the first actual measurement, never an invented zero.
         * This initial state is explicit and not average-validation evidence. */
        h->epoch = o->epoch_after;
        h->previous_ma = h->average_ma = ma;
        h->samples = 1;
    } else {
        /* Active Samsung battery_v2_legacy formula, signed toward-zero division.
         * All operands derive from the bounded 11-bit gauge magnitude. */
        weighted = 2LL * h->average_ma + h->previous_ma + ma;
        h->average_ma = (int)(weighted / 4);
        h->previous_ma = ma;
        h->samples = 2;
        o->history_ready = true;
    }
    h->last_ms = current_sample->started_ms;
    o->average_ma = h->average_ma;
}
int t380_pa_acquire(const struct t380_pa_reader *r, struct t380_pa_history *h,
                     struct t380_pa_tuple *o)
{
    const struct t380_pa_tuple empty = {0};
    static const unsigned char addresses[T380_PA_FIELDS] = { 0, 0x07, 0x08, 0x05 };
    struct t380_pa_field_sample *f;
    t380_pa_u64 previous;
    bool failed = false;
    int i, ret;

    if (!o || !h) return -EINVAL;
    *o = empty;
    o->guard_before_error = o->guard_after_error = -ENODATA;
    for (i = 0; i < T380_PA_FIELDS; ++i) {
        o->field[i].raw = -1;
        o->field[i].error = -ENODATA;
    }
    if (!r || !r->guard || !r->word || !r->temperature || !r->now ||
        !r->epoch || !r->revoke_epoch) { t380_pa_history_reset(h); return -EINVAL; }
    o->guard_before_start_ms = r->now(r->context);
    o->guard_before_error = r->guard(r->context);
    o->guard_before_end_ms = r->now(r->context);
    if (o->guard_before_error) {
        t380_pa_history_reset(h);
        if (o->guard_before_error == -ECANCELED) {
            o->guard_cancelled = true;
            return -ECANCELED;
        }
        r->revoke_epoch(r->context); return -EPERM;
    }
    o->epoch_before = r->epoch(r->context);
    o->started_ms = r->now(r->context);
    previous = o->started_ms;
    for (i = 0; i < T380_PA_FIELDS; ++i) {
        f = &o->field[i];
        f->raw = -1;
        f->started_ms = r->now(r->context);
        if (!i) {
            ret = r->temperature(r->context, &f->raw, &f->value);
        } else {
            ret = r->word(r->context, addresses[i]);
            f->raw = ret;
            if (ret >= 0 && ret <= 65535) {
                if (i == T380_PA_SOC) {
                    f->value = ret >> 8;
                    ret = f->value <= 100 || r->stock_soc ? 0 : -ERANGE;
                    if (!ret && f->value > 100) f->value = 100;
                    /* raw retains exact stock unsigned8.8 over100 encoding. */
                } else {
                    f->value = (ret & 0x7ff) * 1000000 / 256;
                    if (i == T380_PA_CURRENT && (ret & 0x8000)) f->value = -f->value;
                    ret = 0;
                }
            } else if (ret > 65535) ret = -ERANGE;
        }
        f->ended_ms = r->now(r->context);
        if (!ret && (f->started_ms < previous || f->ended_ms < f->started_ms))
            ret = -ERANGE;
        f->error = ret;
        if (ret) failed = true;
        else o->valid |= 1u << i;
        previous = f->ended_ms;
    }
    o->guard_after_start_ms = r->now(r->context);
    o->guard_after_error = r->guard(r->context);
    o->guard_after_end_ms = r->now(r->context);
    o->ended_ms = r->now(r->context);
    if (o->guard_after_error && o->guard_after_error != -ECANCELED)
        failed = true;
    if (o->ended_ms < previous ||
        o->ended_ms < o->started_ms || o->ended_ms - o->started_ms > T380_PA_HISTORY_MAX_MS)
        failed = true;
    if (h->samples && o->field[T380_PA_CURRENT].started_ms <= h->last_ms) {
        o->field[T380_PA_CURRENT].error = -ERANGE;
        o->valid &= ~(1u << T380_PA_CURRENT);
        failed = true;
    }
    if (!failed && o->guard_after_error == -ECANCELED) {
        o->guard_cancelled = true;
        t380_pa_history_reset(h);
        return -ECANCELED;
    }
    if (failed) r->revoke_epoch(r->context);
    o->epoch_after = r->epoch(r->context);
    o->epoch_coherent = o->epoch_before && o->epoch_before == o->epoch_after;
    o->tuple_valid = !failed && o->epoch_coherent && o->valid == 15u;
    o->conservative_voltage_blocked = (o->valid & (1u << T380_PA_VOLTAGE)) &&
        o->field[T380_PA_VOLTAGE].value / 1000 > 4190;
    t380_pa_history_update(h, o);
    /* A failed tuple is still inspectable; validity/provenance remain explicit. */
    return 0;
}
