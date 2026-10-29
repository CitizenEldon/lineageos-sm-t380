/* SPDX-License-Identifier: GPL-2.0-only */
#include "t380_trial_policy.h"

bool t380_trial_decide(struct t380_cp_state *state, const struct t380_pp_cycle *c,
                       const struct t380_pp_result *sample, bool operator_checked,
                       bool fixed5v_declared, bool belkin_session_500ma,
                       enum t380_trial_voltage_window window,
                       struct t380_cp_output *decision)
{
    struct t380_cp_input input;
    const unsigned int battery_bits = T380_CP_VALID_TEMP | T380_CP_VALID_VOLTAGE |
        T380_CP_VALID_CURRENT | T380_CP_VALID_AVERAGE | T380_CP_VALID_SOC;
    int j, selected_ma = belkin_session_500ma ? 500 : 100;
    int floor_mv = window == T380_BELKIN_SESSION_WINDOW_3800 ? 3800 : 3900;

    if (!decision) return false;
    *decision = (struct t380_cp_output){ .reason = T380_CP_INVALID_ARGUMENT };
    if (!state || !c || !sample || !operator_checked ||
        (belkin_session_500ma && !fixed5v_declared) ||
        (window != T380_TRIAL_WINDOW_3900 &&
         window != T380_BELKIN_SESSION_WINDOW_3800) ||
        (window == T380_BELKIN_SESSION_WINDOW_3800 &&
         (!fixed5v_declared || !belkin_session_500ma)) ||
        !sample->acquisition_valid || sample->latched_error ||
        (fixed5v_declared ?
         !c->source.fixed5v_qualified ||
         c->muic.pattern != T380_MO_FIXED5V_TIMEOUT_PATTERN :
         !c->source.dcp_candidate || c->source.bus_suspended ||
         c->muic.pattern != T380_MO_DCP_PATTERN) ||
        c->source.system_suspended ||
        !c->muic.stable ||
        (sample->input.valid & battery_bits) != battery_bits ||
        sample->input.temp_decic < 200 || sample->input.temp_decic > 350 ||
        sample->input.voltage_mv < floor_mv || sample->input.voltage_mv > 4170 ||
        c->charger[0].chgon != c->charger[1].chgon)
        return false;
    for (j = 0; j < 2; ++j)
        if (!c->charger[j].valid || !c->charger[j].vbusok ||
            c->charger[j].vbusuvlo || c->charger[j].vbusovp ||
            c->charger[j].vbus_conflict || c->charger[j].otgfail ||
            c->charger[j].aicl_active)
            return false;
    input = sample->input;
    input.valid |= T380_CP_VALID_SOURCE | T380_CP_VALID_CHARGER;
    input.telemetry_validated = true;
    input.normal_charge_region_validated = true; /* Explicit bounded window. */
    input.source_validated = true; /* Explicit operator source declaration. */
    input.charger_healthy = true;  /* Limited observed fault bits, not exhaustive. */
    input.source = fixed5v_declared ?
        T380_CP_SOURCE_QUALIFIED_FIXED5V_TIMEOUT : T380_CP_SOURCE_DCP;
    input.source_voltage_mv = 5000;
    /* Only an explicitly selected Belkin session may request 500 mA. The
     * ten-second trial and DCP session still supply a 100 mA allowance. */
    input.source_allowance_ma = selected_ma;
    input.source_suspended = fixed5v_declared ? false : c->source.bus_suspended;
    t380_charge_policy_step(state, &input, decision);
    return decision->would_charge && decision->reason == T380_CP_WOULD_CHARGE &&
        decision->phase == T380_CP_FIRST &&
        decision->input_limit_ma == selected_ma &&
        decision->charge_limit_ma == selected_ma &&
        decision->float_voltage_mv == 4190 &&
        decision->topoff_ma == 350;
}
