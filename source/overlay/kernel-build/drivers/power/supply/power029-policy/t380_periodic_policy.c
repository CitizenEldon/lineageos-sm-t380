/* SPDX-License-Identifier: GPL-2.0-only */
#include "t380_periodic_policy.h"
#include "t380_power_acquisition_policy.h"
#include "t380_source_lifecycle_policy.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

void t380_pp_init(struct t380_pp_state *s)
{
    const struct t380_pp_state empty = { 0 };
    *s = empty;
    t380_charge_policy_init(&s->policy);
}
/* Physical integrity is independent of source charging qualification. Always
 * inspect the complete acquisition before reporting a bus-only refusal. */
static int t380_pp_acquisition_error(const struct t380_pp_cycle *c,
                                    uint64_t evaluations, uint64_t last_ms)
{
    uint64_t oldest;
    int i, error = 0;
    oldest = c->battery.started_ms;
    for (i = 0; i < 2; ++i)
        if (c->charger_started[i] < oldest) oldest = c->charger_started[i];
    if (c->muic_started_ms < oldest) oldest = c->muic_started_ms;
    if (c->off_before_error || c->off_after_error)
        error = c->off_before_error ? c->off_before_error : c->off_after_error;
    else if (c->charger_error[0] || c->charger_error[1] || c->muic_error ||
             c->battery_error || c->readback_error)
        error = c->charger_error[0] ? c->charger_error[0] :
                c->charger_error[1] ? c->charger_error[1] :
                c->muic_error ? c->muic_error : c->battery_error ?
                c->battery_error : c->readback_error;
    else if (c->ended_ms < c->started_ms || oldest < c->started_ms ||
             c->charger_ended[0] < c->charger_started[0] ||
             c->muic_started_ms < c->charger_ended[0] ||
             c->muic_ended_ms < c->muic_started_ms ||
             c->battery.started_ms < c->muic_ended_ms ||
             c->charger_started[1] < c->battery.ended_ms ||
             c->charger_ended[1] < c->charger_started[1] ||
             c->source.now_ms < c->charger_ended[1] ||
             c->ended_ms < c->source.now_ms ||
             (evaluations && c->ended_ms <= last_ms))
        error = -ERANGE;
    else if (c->ended_ms - c->started_ms > T380_PP_ACQUISITION_LIMIT_MS)
        error = -ETIME;
    else if (c->epoch_started != c->source.generation ||
             c->battery.epoch_before != c->source.generation ||
             c->battery.epoch_after != c->source.generation)
        error = -ESTALE;
    else if (!c->battery.tuple_valid || !c->muic.valid || !c->muic.stable)
        error = -ENODATA;
    for (i = 0; !error && i < 2; ++i)
        if (!c->charger[i].valid || (!c->trial_active && c->charger[i].chgon) ||
            c->charger[i].otgfail ||
            c->charger[i].vbusovp || c->charger[i].vbus_conflict ||
            (c->charger[i].vbusuvlo &&
             !t380_co_expected_absent_uvlo(&c->charger[i], c->trial_active)))
            error = -EIO;
    return error;
}
void t380_pp_evaluate(struct t380_pp_state *s, const struct t380_pp_cycle *c,
                       struct t380_pp_result *out)
{
    const struct t380_pp_result empty = { 0 };
    uint64_t oldest;
    int i, error = 0;
    *out = empty;
    t380_pa_policy_inputs(&c->battery, &out->input);
    t380_sl_policy_inputs(&c->source, &out->input);
    out->integrity_error = t380_pp_acquisition_error(c, s->evaluations, s->last_ms);
    error = out->integrity_error;
    oldest = c->battery.started_ms;
    for (i = 0; i < 2; ++i)
        if (c->charger_started[i] < oldest) oldest = c->charger_started[i];
    if (c->muic_started_ms < oldest) oldest = c->muic_started_ms;
    out->input.now_ms = c->ended_ms;
    out->input.sample_ms = oldest;
    /* Observed CHGON/VBUS/OTG is not complete health. No validity promotion. */
    out->input.charger_healthy = false;
    if (!error && (c->source.system_suspended ||
        (c->source.bus_suspended && !c->source.fixed5v_qualified)))
        error = -EAGAIN;
    if (error && error != -EAGAIN && error != -ESTALE && !s->latched_error)
        s->latched_error = error;
    if (error)
        out->input.valid &= ~(unsigned int)(T380_CP_VALID_TEMP | T380_CP_VALID_VOLTAGE |
            T380_CP_VALID_CURRENT | T380_CP_VALID_AVERAGE | T380_CP_VALID_SOC);
    t380_charge_policy_step(&s->policy, &out->input, &out->decision);
    if (out->decision.would_charge || out->decision.input_limit_ma ||
        out->decision.charge_limit_ma || out->decision.float_voltage_mv ||
        out->decision.topoff_ma) {
        /* Invariant failure is latched; the integration never exports a plan. */
        s->latched_error = -EPERM;
        out->decision = (struct t380_cp_output){ .reason = T380_CP_INVALID_ARGUMENT };
    }
    ++s->evaluations;
    s->last_ms = c->ended_ms;
    out->cycle_error = error;
    out->latched_error = s->latched_error;
    out->acquisition_valid = !error;
    out->permission = false;
}
