/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_SESSION_H
#define T380_SESSION_H
#ifdef __KERNEL__
#include <linux/types.h>
typedef u64 t380_ss_u64;
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t t380_ss_u64;
#endif

#include "../power066-regular/t380_regular_full.h"

#define T380_SS_BUDGET_MS 60000ULL
#define T380_SS_MAX_BUDGET_MS 1800000ULL
#define T380_SS_LEASE_MS 10000ULL
#define T380_SS_SAMPLE_MAX_AGE_MS 500ULL
#define T380_SS_ATTACHED_INITIAL_MS 36000000ULL
#define T380_SS_ATTACHED_RECHARGE_MS 5400000ULL
#define T380_SS_DETACH_PROOF_MS 30000ULL /* existing physical-source age bound */

/* Explicit household profile selection. Zero preserves every existing caller. */
enum t380_household_voltage_window {
	T380_HOUSEHOLD_WINDOW_3900 = 0,
	T380_HOUSEHOLD_WINDOW_ATTENDED_3800,
	T380_HOUSEHOLD_WINDOW_NORMAL_3600_V1,
	T380_HOUSEHOLD_WINDOW_OPERATOR900_3900_V1,
	T380_HOUSEHOLD_WINDOW_OPERATOR900_NORMAL_3600_V1,
	T380_HOUSEHOLD_WINDOW_REGULAR900_3900_V1,
	T380_HOUSEHOLD_WINDOW_REGULAR900_NORMAL_3600_V1,
	T380_HOUSEHOLD_WINDOW_COUNT,
};

/* One mapping is shared by admission and the phase model. */
static inline int t380_household_window_floor_mv(
		enum t380_household_voltage_window window)
{
	switch (window) {
	case T380_HOUSEHOLD_WINDOW_3900:
	case T380_HOUSEHOLD_WINDOW_OPERATOR900_3900_V1:
	case T380_HOUSEHOLD_WINDOW_REGULAR900_3900_V1:
		return 3900;
	case T380_HOUSEHOLD_WINDOW_ATTENDED_3800:
		return 3800;
	case T380_HOUSEHOLD_WINDOW_NORMAL_3600_V1:
	case T380_HOUSEHOLD_WINDOW_OPERATOR900_NORMAL_3600_V1:
	case T380_HOUSEHOLD_WINDOW_REGULAR900_NORMAL_3600_V1:
		return 3600;
	default:
		return -1;
	}
}

/* Input authority is distinct from the unchanged500mA battery fast limit.
 * New900 windows require an explicit operator contract; old zero/default is500. */
static inline int t380_household_window_input_ma(
        enum t380_household_voltage_window window)
{
    if (t380_household_window_floor_mv(window) < 0) return -1;
    return window == T380_HOUSEHOLD_WINDOW_OPERATOR900_3900_V1 ||
           window == T380_HOUSEHOLD_WINDOW_OPERATOR900_NORMAL_3600_V1 ||
           window == T380_HOUSEHOLD_WINDOW_REGULAR900_3900_V1 ||
           window == T380_HOUSEHOLD_WINDOW_REGULAR900_NORMAL_3600_V1 ? 900 : 500;
}

/* Immutable selected profile; old enrollment cannot select this window. */
static inline bool t380_household_window_regular(
 enum t380_household_voltage_window window)
{
 return window == T380_HOUSEHOLD_WINDOW_REGULAR900_3900_V1 ||
        window == T380_HOUSEHOLD_WINDOW_REGULAR900_NORMAL_3600_V1;
}
static inline int t380_household_window_cv_mv(
 enum t380_household_voltage_window window)
{
 return t380_household_window_regular(window) ? T380_RF_FLOAT_MV : 4190;
}
static inline int t380_household_window_initial_mv(
 enum t380_household_voltage_window window)
{
 return t380_household_window_regular(window) ? T380_RF_INITIAL_UPPER_MV : 4170;
}

/* Persistent GPIO-owner state, never allocated in the charger consumer. */
struct t380_ss_state {
	bool spent, pending, active, paused_for_plan;
	bool full_rest, rest_reentry_pending;
	bool attached_mode, episode_open, completed_full;
	/* Household removal is a verified-HIGH pause, never a fresh episode. */
	bool household_mode, household_detaching, household_detached;
	bool household_rebinding;
	bool household_detached_rest;
	t380_ss_u64 household_remaining_ms;
	t380_ss_u64 household_detach_started_ms;
	t380_ss_u64 budget_ms, session_deadline_ms, lease_deadline_ms, last_sample_ms;
	t380_ss_u64 paused_sample_ms;
	t380_ss_u64 full_sample_ms, rest_pending_sample_ms;
	t380_ss_u64 episode_start_ms, pending_recharge_deadline_ms;
	unsigned int episode_number;
	int latched_error;
};
struct t380_ss_io {
	void *context;
	/* Caller holds the exclusive GPIO-owner lock; no I2C in these hooks. */
	int (*high)(void *);
	int (*low)(void *);
	int (*schedule_high)(void *, t380_ss_u64, bool initial);
	int (*validate_source)(void *);
	int (*verify_low)(void *);
};
int t380_ss_reserve(struct t380_ss_state *, t380_ss_u64 now_ms,
		    t380_ss_u64 budget_ms);
int t380_ss_reserve_attached(struct t380_ss_state *, t380_ss_u64 now_ms);
int t380_ss_household_detach_start(struct t380_ss_state *,
		const struct t380_ss_io *, t380_ss_u64 now_ms);
int t380_ss_household_detach_finish(struct t380_ss_state *,
		const struct t380_ss_io *, t380_ss_u64 now_ms,
		t380_ss_u64 (*now)(void *));
int t380_ss_household_rebind(struct t380_ss_state *,
		const struct t380_ss_io *, t380_ss_u64 now_ms);
int t380_ss_begin(struct t380_ss_state *, const struct t380_ss_io *,
		  t380_ss_u64 now_ms, t380_ss_u64 sample_ms,
		  t380_ss_u64 (*now)(void *));
int t380_ss_renew(struct t380_ss_state *, const struct t380_ss_io *,
		  t380_ss_u64 now_ms, t380_ss_u64 sample_ms,
		  t380_ss_u64 (*now)(void *));
int t380_ss_check(struct t380_ss_state *, const struct t380_ss_io *,
		  t380_ss_u64 now_ms);
int t380_ss_pause_plan(struct t380_ss_state *, const struct t380_ss_io *,
		      t380_ss_u64 now_ms, t380_ss_u64 sample_ms);
int t380_ss_resume_plan(struct t380_ss_state *, const struct t380_ss_io *,
		       t380_ss_u64 now_ms, t380_ss_u64 sample_ms,
		       t380_ss_u64 (*now)(void *));
int t380_ss_enter_full_rest(struct t380_ss_state *, const struct t380_ss_io *,
			    t380_ss_u64 now_ms, t380_ss_u64 sample_ms,
			    t380_ss_u64 (*now)(void *));
int t380_ss_rest_check(struct t380_ss_state *, const struct t380_ss_io *,
		       t380_ss_u64 now_ms);
int t380_ss_recharge_pause(struct t380_ss_state *, const struct t380_ss_io *,
			   t380_ss_u64 now_ms, t380_ss_u64 sample_ms);
int t380_ss_recharge_resume(struct t380_ss_state *, const struct t380_ss_io *,
			    t380_ss_u64 now_ms, t380_ss_u64 sample_ms,
			    t380_ss_u64 (*now)(void *));
int t380_ss_off(struct t380_ss_state *, const struct t380_ss_io *, int reason);
#endif
