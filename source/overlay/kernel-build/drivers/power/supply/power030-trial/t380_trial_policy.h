/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_TRIAL_POLICY_H
#define T380_TRIAL_POLICY_H
#include "../power029-policy/t380_periodic_policy.h"

/* The selected window is explicit; only the new attended Belkin session may
 * use the nearby-voltage profile. A DCP pattern alone grants no current. */
enum t380_trial_voltage_window {
	T380_TRIAL_WINDOW_3900,
	T380_BELKIN_SESSION_WINDOW_3800,
};
bool t380_trial_decide(struct t380_cp_state *, const struct t380_pp_cycle *,
                       const struct t380_pp_result *, bool operator_checked,
                       bool fixed5v_declared, bool belkin_session_500ma,
                       enum t380_trial_voltage_window,
                       struct t380_cp_output *);
#endif
