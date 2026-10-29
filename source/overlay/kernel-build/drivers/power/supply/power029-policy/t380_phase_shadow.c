/* SPDX-License-Identifier: GPL-2.0-only */
#include "t380_phase_shadow.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

static bool ps_window_valid(enum t380_household_voltage_window window)
{
	return t380_household_window_floor_mv(window) >= 0;
}

static bool ps_qualified(const struct t380_ps_state *s,
			 const struct t380_pp_cycle *c,
			 const struct t380_pp_result *sample)
{
	const unsigned int battery = T380_CP_VALID_TEMP |
		T380_CP_VALID_VOLTAGE | T380_CP_VALID_CURRENT |
		T380_CP_VALID_AVERAGE | T380_CP_VALID_SOC |
		T380_CP_VALID_AGE;
	int i;

	if (!ps_window_valid(s->household_window) ||
	    !c->source.fixed5v_qualified ||
	    (t380_household_window_input_ma(s->household_window) == 900 &&
	     c->source.fixed5v_input_allowance_ma != 900) ||
	    c->muic.pattern != T380_MO_FIXED5V_TIMEOUT_PATTERN ||
	    !c->muic.stable || c->source.system_suspended ||
	    c->source.now_ms < c->source.oldest_ms ||
	    c->source.now_ms - c->source.oldest_ms > 2000 ||
	    !sample->acquisition_valid || sample->latched_error ||
	    (sample->input.valid & battery) != battery ||
	    (t380_household_window_regular(s->household_window) ?
         (!sample->input.regular_model_matched || !sample->input.regular_profile ||
          sample->input.age_step != 0) : sample->input.age_step != 4) ||
	    sample->input.temp_decic < 200 || sample->input.temp_decic > 350 ||
	    sample->input.voltage_mv <
		t380_household_window_floor_mv(s->household_window) ||
	    sample->input.voltage_mv >
        t380_household_window_cv_mv(s->household_window))
		return false;
	for (i = 0; i < 2; ++i)
		if (!c->charger[i].valid || !c->charger[i].vbusok ||
		    c->charger[i].vbusuvlo || c->charger[i].vbusovp ||
		    c->charger[i].vbus_conflict || c->charger[i].otgfail ||
		    c->charger[i].aicl_active)
			return false;
	return true;
}

bool t380_ps_regular_full_candidate(const struct t380_ps_state *s,
 const struct t380_pp_cycle *c, const struct t380_pp_result *sample)
{
 struct t380_cp_state probe;
 struct t380_cp_input input;
 struct t380_cp_output output;
 if (!s || !c || !sample || !t380_household_window_regular(s->household_window) ||
     s->status != T380_PS_BOUND || !ps_qualified(s, c, sample) ||
     s->generation != c->source.generation || !c->trial_active ||
     !s->applied_valid || s->applied_phase != s->policy.phase ||
     (s->policy.phase != T380_CP_FIRST && s->policy.phase != T380_CP_SECOND) ||
     !c->charger[0].chgon || !c->charger[1].chgon ||
     c->charger[0].done || c->charger[1].done ||
     sample->input.voltage_mv < T380_RF_FULL_MIN_MV || sample->input.current_ma <= 0 ||
     sample->input.current_ma >= 350 || sample->input.average_current_ma <= 0 ||
     sample->input.average_current_ma >= (s->policy.phase == T380_CP_FIRST ? 350 : 250))
  return false;
 input = sample->input;
 input.valid |= T380_CP_VALID_SOURCE | T380_CP_VALID_CHARGER;
 input.telemetry_validated = input.normal_charge_region_validated = true;
 input.source_validated = input.charger_healthy = true;
 input.source = T380_CP_SOURCE_OPERATOR900_FIXED5V_TIMEOUT;
 input.source_voltage_mv = 5000; input.source_allowance_ma = 900;
 input.source_suspended = false;
 probe = s->policy;
 t380_charge_policy_step_without_completion(&probe, &input, &output);
 return output.reason == T380_CP_WOULD_CHARGE && !probe.fault_latched;
}

void t380_ps_init(struct t380_ps_state *s)
{
	if (!s) return;
	*s = (struct t380_ps_state){ 0 };
	t380_charge_policy_init(&s->policy);
	s->household_window = T380_HOUSEHOLD_WINDOW_3900;
}

bool t380_ps_set_household_window(struct t380_ps_state *s,
		enum t380_household_voltage_window window)
{
	if (!s || !ps_window_valid(window) ||
	    s->status != T380_PS_WAIT_SOURCE || s->policy.initialized ||
	    s->household_rebind_pending)
		return false;
	s->household_window = window;
	return true;
}

void t380_ps_household_rebind(struct t380_ps_state *s)
{
	if (!s) return;
	s->status = T380_PS_WAIT_SOURCE;
	s->generation = 0;
	s->applied_valid = false; /* Old attachment's plan is never proof of new plan. */
	s->household_rebind_pending = true;
	s->recharge_candidate = false;
}

void t380_ps_observe(struct t380_ps_state *s, const struct t380_pp_cycle *c,
		     const struct t380_pp_result *sample, struct t380_ps_output *out)
{
	struct t380_cp_input input;
	enum t380_cp_phase previous;
	bool qualified, active_applied, hardware_mismatch, hardware_done;

	if (!out) return;
	*out = (struct t380_ps_output){ .status = T380_PS_WAIT_SOURCE,
		.reason = T380_CP_UNVALIDATED_SOURCE };
	if (!s || !c || !sample) return;
	if (s->status == T380_PS_TERMINAL) goto publish;
	active_applied = c->trial_active && s->applied_valid &&
		(s->applied_phase == T380_CP_FIRST ||
		 s->applied_phase == T380_CP_SECOND) &&
		(s->policy.phase == T380_CP_FIRST ||
		 s->policy.phase == T380_CP_SECOND);
	qualified = ps_qualified(s, c, sample) &&
		(active_applied ||
		 c->charger[0].chgon == c->charger[1].chgon);
	if (s->status == T380_PS_WAIT_SOURCE) {
		/* Boot PC/unknown/absent is not the Belkin episode. */
		if (!qualified) goto publish;
		s->generation = c->source.generation;
		s->status = T380_PS_BOUND;
	}
	if (!qualified || c->source.generation != s->generation) {
		s->status = T380_PS_TERMINAL;
		s->refusal = sample->cycle_error ? sample->cycle_error : -1;
		goto publish;
	}
	/* Only a live LOW session with an acknowledged charging plan can be
	 * autonomously terminated. Pre-arm and genuine FULL/rest observations
	 * retain their existing behavior. A flag is never a FULL witness. */
	hardware_mismatch = active_applied &&
		(c->charger[0].chgon != c->charger[1].chgon ||
		 c->charger[0].done != c->charger[1].done ||
		 c->charger[0].done || !c->charger[0].chgon);
	hardware_done = hardware_mismatch &&
		!c->charger[0].chgon && !c->charger[1].chgon &&
		c->charger[0].done && c->charger[1].done &&
		c->charger[0].topoff == c->charger[1].topoff;
	if (s->household_rebind_pending &&
	    s->policy.phase == T380_CP_SECOND) {
		/* The first qualified post-replug sample can request a fresh 250 mA
		 * plan while HIGH. It may not count SECOND completion before that plan
		 * is verified and acknowledged on this attachment. */
		s->decision = (struct t380_cp_output) {
			.would_charge = true, .reason = T380_CP_WOULD_CHARGE,
			.phase = T380_CP_SECOND, .input_limit_ma =
			t380_household_window_input_ma(s->household_window),
			.charge_limit_ma = 500, .float_voltage_mv =
            t380_household_window_cv_mv(s->household_window),
			.topoff_ma = 250,
			.remaining_budget_ma_ms = s->policy.remaining_budget_ma_ms };
		goto publish;
	}
	/* Never count a second-stage completion from a sample collected while
	 * the first-stage plan is still the actual applied plan. */
	if (!hardware_mismatch && s->policy.phase == T380_CP_SECOND &&
	    (!s->applied_valid || s->applied_phase != T380_CP_SECOND))
		goto publish;
	input = sample->input;
    input.regular_completion_allowed = active_applied;
	input.valid |= T380_CP_VALID_SOURCE | T380_CP_VALID_CHARGER;
	/* This is a prospective model premise, never production admission. */
	input.telemetry_validated = true;
	input.normal_charge_region_validated = true;
	input.source_validated = true;
	input.charger_healthy = true;
	input.source = t380_household_window_input_ma(s->household_window) == 900 ?
		T380_CP_SOURCE_OPERATOR900_FIXED5V_TIMEOUT :
		T380_CP_SOURCE_QUALIFIED_FIXED5V_TIMEOUT;
	input.source_voltage_mv = 5000;
	input.source_allowance_ma = t380_household_window_input_ma(s->household_window);
	input.source_suspended = false; /* Qualified hostless fixed 5 V only. */
	previous = s->policy.phase;
	if (hardware_mismatch)
		t380_charge_policy_step_without_completion(
			&s->policy, &input, &s->decision);
	else
		t380_charge_policy_step(&s->policy, &input, &s->decision);
	/* Account through this fresh sample first. Existing thermal, timestamp,
	 * fault and budget refusals take precedence over the hardware diagnosis.
	 * No completion, recharge, new episode or budget is earned by this stop. */
	if (hardware_mismatch &&
	    s->decision.reason == T380_CP_WOULD_CHARGE) {
		enum t380_cp_reason reason = hardware_done ?
			T380_CP_HARDWARE_DONE_INCOMPLETE :
			T380_CP_HARDWARE_STATE_MISMATCH;
		s->policy.previous_would_charge = false;
		s->policy.previous_effective_ma = 0;
		s->policy.episode_open = false;
		s->policy.fault_latched = true;
		s->policy.fault_reason = reason;
		s->recharge_candidate = false;
		s->status = T380_PS_TERMINAL;
		s->refusal = hardware_done ? -ENOMSG : -EPROTO;
		s->decision = (struct t380_cp_output) {
			.reason = reason, .phase = s->policy.phase,
			.thermal = s->policy.thermal,
			.swelling_blocked = s->policy.swelling_blocked,
			.low_temp_derated = s->policy.low_temp_derated,
			.remaining_budget_ma_ms =
				s->policy.remaining_budget_ma_ms };
		goto publish;
	}
	s->recharge_candidate = previous == T380_CP_FULL &&
		s->decision.phase == T380_CP_FIRST &&
		s->decision.reason == T380_CP_WOULD_CHARGE;
	if (s->decision.reason != T380_CP_WOULD_CHARGE &&
	    s->decision.reason != T380_CP_FULL_STOP) {
		s->status = T380_PS_TERMINAL;
		s->refusal = -1;
	}
publish:
	out->status = s->status;
	out->generation = s->generation;
	out->reason = s->status == T380_PS_WAIT_SOURCE ?
		T380_CP_UNVALIDATED_SOURCE :
		s->status == T380_PS_TERMINAL &&
		s->decision.reason == T380_CP_WOULD_CHARGE ?
		T380_CP_UNVALIDATED_SOURCE : s->decision.reason;
	out->phase = s->decision.phase;
	out->recharge_candidate = s->status == T380_PS_BOUND &&
		s->recharge_candidate;
	if (s->status == T380_PS_BOUND && s->decision.would_charge &&
	    s->decision.reason == T380_CP_WOULD_CHARGE &&
	    s->decision.input_limit_ma ==
		t380_household_window_input_ma(s->household_window) &&
	    s->decision.charge_limit_ma == 500 &&
	    s->decision.float_voltage_mv ==
    t380_household_window_cv_mv(s->household_window) &&
	    (s->decision.phase == T380_CP_FIRST ?
	     s->decision.topoff_ma == 350 :
	     s->decision.phase == T380_CP_SECOND &&
	     s->decision.topoff_ma == 250)) {
		out->candidate_plan = true;
		out->input_ma = s->decision.input_limit_ma;
		out->fast_ma = s->decision.charge_limit_ma;
		out->float_mv = s->decision.float_voltage_mv;
		out->topoff_ma = s->decision.topoff_ma;
	}
	/* No path writes authority=true or invokes the GPIO/settings owner. */
}

bool t380_ps_ack_applied(struct t380_ps_state *s, enum t380_cp_phase phase,
			 uint64_t now_ms)
{
	if (!s || s->status != T380_PS_BOUND ||
	    s->decision.reason != T380_CP_WOULD_CHARGE ||
	    s->decision.phase != phase || phase == T380_CP_FULL)
		return false;
	if (s->household_rebind_pending && phase == T380_CP_SECOND &&
	    (s->decision.input_limit_ma !=
	     t380_household_window_input_ma(s->household_window) ||
	     s->decision.charge_limit_ma != 500 ||
	     s->decision.float_voltage_mv !=
         t380_household_window_cv_mv(s->household_window) ||
	     s->decision.topoff_ma != 250 ||
	     !t380_charge_policy_household_ack_second(&s->policy, now_ms)))
		return false;
	s->applied_phase = phase;
	s->applied_valid = true;
	s->household_rebind_pending = false;
	return true;
}
