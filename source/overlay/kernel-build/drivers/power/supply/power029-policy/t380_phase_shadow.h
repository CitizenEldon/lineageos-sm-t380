/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_PHASE_SHADOW_H
#define T380_PHASE_SHADOW_H
#include "t380_periodic_policy.h"
#include "../power032-session/t380_session.h"

/* Observation only. None of these fields is permission to program or enable. */
enum t380_ps_status {
	T380_PS_WAIT_SOURCE,
	T380_PS_BOUND,
	T380_PS_TERMINAL,
};
struct t380_ps_state {
	struct t380_cp_state policy;
	enum t380_ps_status status;
	uint64_t generation;
	struct t380_cp_output decision;
	bool recharge_candidate;
	enum t380_cp_phase applied_phase;
	bool applied_valid;
	bool household_rebind_pending;
	enum t380_household_voltage_window household_window;
	int refusal;
};
struct t380_ps_output {
	enum t380_ps_status status;
	uint64_t generation;
	enum t380_cp_reason reason;
	enum t380_cp_phase phase;
	int input_ma, fast_ma, float_mv, topoff_ma;
	bool candidate_plan, recharge_candidate;
	bool authority; /* Always false, including a hypothetical recharge. */
};
void t380_ps_init(struct t380_ps_state *);
bool t380_ps_set_household_window(struct t380_ps_state *,
		enum t380_household_voltage_window);
void t380_ps_observe(struct t380_ps_state *, const struct t380_pp_cycle *,
		     const struct t380_pp_result *, struct t380_ps_output *);
bool t380_ps_ack_applied(struct t380_ps_state *, enum t380_cp_phase,
                         uint64_t now_ms);
void t380_ps_household_rebind(struct t380_ps_state *);
/* Stock candidate preflight: pure copy of restrictive guards/accounting,
 * never debits real state or grants completion/LOW. */
bool t380_ps_regular_full_candidate(const struct t380_ps_state *,
 const struct t380_pp_cycle *, const struct t380_pp_result *);
#endif
