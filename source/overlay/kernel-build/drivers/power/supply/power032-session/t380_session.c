/* SPDX-License-Identifier: GPL-2.0-only */
#include "t380_session.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

static t380_ss_u64 ss_deadline(t380_ss_u64 now, t380_ss_u64 end)
{
	return end - now < T380_SS_LEASE_MS ? end : now + T380_SS_LEASE_MS;
}
static int ss_fail(struct t380_ss_state *s, const struct t380_ss_io *io, int reason)
{
	int ret = t380_ss_off(s, io, reason);
	return ret ? ret : reason;
}
int t380_ss_off(struct t380_ss_state *s, const struct t380_ss_io *io, int reason)
{
	int ret;
	if (!s || !io || !io->high) return -EINVAL;
	/* Revoke admission before any potentially sleeping GPIO operation. */
	s->pending = false;
	s->active = false;
	s->paused_for_plan = false;
	s->paused_sample_ms = 0;
	s->full_rest = false;
	s->rest_reentry_pending = false;
	s->rest_pending_sample_ms = 0;
	s->pending_recharge_deadline_ms = 0;
	s->episode_open = false;
	s->household_detaching = false;
	s->household_detached = false;
	s->household_rebinding = false;
	s->household_detached_rest = false;
	s->household_remaining_ms = 0;
	s->household_detach_started_ms = 0;
	if (reason && !s->latched_error) s->latched_error = reason;
	ret = io->high(io->context);
	if (ret && !s->latched_error) s->latched_error = ret;
	return ret;
}
int t380_ss_reserve(struct t380_ss_state *s, t380_ss_u64 now_ms,
		    t380_ss_u64 budget_ms)
{
	if (!s || s->spent || s->pending || s->active) return -EPERM;
	/* Only the two explicitly named development durations are representable. */
	if (budget_ms != T380_SS_BUDGET_MS &&
	    budget_ms != T380_SS_MAX_BUDGET_MS) return -EINVAL;
	if (now_ms > (t380_ss_u64)~(t380_ss_u64)0 - budget_ms)
		return -ERANGE;
	s->spent = true; /* Failed begin and consumer reprobe cannot rearm. */
	s->budget_ms = budget_ms;
	s->session_deadline_ms = now_ms + budget_ms;
	s->pending = true;
	return 0;
}
int t380_ss_reserve_attached(struct t380_ss_state *s, t380_ss_u64 now_ms)
{
	if (!s || s->spent || s->pending || s->active) return -EPERM;
	if (now_ms > (t380_ss_u64)~(t380_ss_u64)0 -
	    T380_SS_ATTACHED_INITIAL_MS) return -ERANGE;
	s->spent = true;
	s->pending = true;
	s->attached_mode = true;
	s->episode_open = true;
	s->episode_number = 1;
	s->episode_start_ms = now_ms;
	s->budget_ms = T380_SS_ATTACHED_INITIAL_MS;
	s->session_deadline_ms = now_ms + T380_SS_ATTACHED_INITIAL_MS;
	return 0;
}
/* The first physical absence may precede the second sensor by one cycle.
 * Stop LOW immediately, but never pause an incomplete budget until both
 * sensors have established fresh absence. A real error takes ss_off instead. */
int t380_ss_household_detach_start(struct t380_ss_state *s,
		const struct t380_ss_io *io, t380_ss_u64 now_ms)
{
	int ret;

	if (!s || !io || !io->high || !s->household_mode ||
	    s->latched_error || s->household_detached)
		return -EPERM;
	if (s->household_detaching) {
		if (now_ms < s->household_detach_started_ms ||
		    now_ms - s->household_detach_started_ms >=
			T380_SS_DETACH_PROOF_MS)
			return ss_fail(s, io, -ETIME);
		return io->high(io->context);
	}
	if (!s->active && !s->full_rest) return -EPERM;
	if (s->episode_open &&
	    (now_ms >= s->session_deadline_ms ||
	     (s->active && now_ms >= s->lease_deadline_ms)))
		return ss_fail(s, io, -ETIME);
	/* Keep the old lease/end until completed absence. */
	ret = io->high(io->context);
	if (ret) return ss_fail(s, io, ret);
	if (s->full_rest) {
		if (now_ms > (t380_ss_u64)~(t380_ss_u64)0 -
		    T380_SS_DETACH_PROOF_MS)
			return ss_fail(s, io, -ERANGE);
		ret = io->schedule_high ? io->schedule_high(io->context,
			now_ms + T380_SS_DETACH_PROOF_MS, false) : -EINVAL;
		if (ret) return ss_fail(s, io, ret);
	}
	s->active = false;
	s->household_detaching = true;
	s->household_detach_started_ms = now_ms;
	s->paused_for_plan = false;
	s->rest_reentry_pending = false;
	return 0;
}
int t380_ss_household_detach_finish(struct t380_ss_state *s,
		const struct t380_ss_io *io, t380_ss_u64 now_ms,
		t380_ss_u64 (*now)(void *))
{
	t380_ss_u64 refreshed;
	int ret;

	if (!s || !io || !io->high || !now || !s->household_mode ||
	    !s->household_detaching || s->latched_error) return -EPERM;
	if (now_ms < s->household_detach_started_ms ||
	    now_ms - s->household_detach_started_ms >=
		T380_SS_DETACH_PROOF_MS)
		return ss_fail(s, io, -ETIME);
	if (s->episode_open &&
	    (now_ms >= s->session_deadline_ms ||
	     now_ms >= s->lease_deadline_ms))
		return ss_fail(s, io, -ETIME);
	ret = io->high(io->context);
	if (ret) return ss_fail(s, io, ret);
	refreshed = now(io->context);
	if (refreshed < now_ms ||
	    refreshed < s->household_detach_started_ms ||
	    refreshed - s->household_detach_started_ms >=
		T380_SS_DETACH_PROOF_MS ||
	    (s->episode_open &&
	     (refreshed >= s->session_deadline_ms ||
	      refreshed >= s->lease_deadline_ms)))
		return ss_fail(s, io, -ETIME);
	s->household_remaining_ms = s->episode_open ?
		s->session_deadline_ms - refreshed : 0;
	s->household_detached_rest = s->full_rest;
	s->full_rest = false;
	s->pending = false;
	s->household_detaching = false;
	s->household_detach_started_ms = 0;
	s->household_detached = true;
	return 0;
}
int t380_ss_household_rebind(struct t380_ss_state *s,
		const struct t380_ss_io *io, t380_ss_u64 now_ms)
{
	t380_ss_u64 cap, remaining;
	int ret;

	if (!s || !io || !io->high || !s->household_mode ||
	    !s->household_detached || s->latched_error) return -EPERM;
	if (s->episode_open) {
		cap = s->episode_number == 1 ? T380_SS_ATTACHED_INITIAL_MS :
			T380_SS_ATTACHED_RECHARGE_MS;
		if (!s->episode_number || !s->household_remaining_ms ||
		    s->household_remaining_ms > cap ||
		    s->session_deadline_ms <= s->episode_start_ms ||
		    s->session_deadline_ms - s->episode_start_ms > cap ||
		    s->household_remaining_ms >
			 s->session_deadline_ms - s->episode_start_ms ||
		    now_ms < s->session_deadline_ms - s->household_remaining_ms)
			return ss_fail(s, io, -ERANGE);
		if (now_ms >= s->session_deadline_ms)
			return ss_fail(s, io, -ETIME);
	}
	ret = io->high(io->context);
	if (ret) return ss_fail(s, io, ret);
	if (s->episode_open) {
		/* Detached wall time belongs to this original episode. Keep both
		 * its origin and absolute deadline; only its remainder may fall. */
		remaining = s->session_deadline_ms - now_ms;
		s->household_remaining_ms = remaining;
		s->pending = true;
	} else {
		if (!s->household_detached_rest || !s->completed_full)
			return ss_fail(s, io, -EPERM);
		s->full_rest = true;
	}
	s->household_detached_rest = false;
	s->household_detached = false;
	return 0;
}
int t380_ss_check(struct t380_ss_state *s, const struct t380_ss_io *io,
		  t380_ss_u64 now_ms)
{
	int ret;
	if (!s || !io || !s->active || s->latched_error) return -EPERM;
	if (now_ms >= s->session_deadline_ms || now_ms >= s->lease_deadline_ms)
		return ss_fail(s, io, -ETIME);
	/* A HIGH programming transaction is not a verified live LOW lease. */
	if (s->paused_for_plan) return -EAGAIN;
	ret = io->validate_source ? io->validate_source(io->context) : -EINVAL;
	if (ret) return ss_fail(s, io, ret);
	ret = io->verify_low ? io->verify_low(io->context) : -EINVAL;
	if (ret) return ss_fail(s, io, ret);
	return 0;
}
static int ss_sample(const struct t380_ss_state *s, t380_ss_u64 now,
		     t380_ss_u64 sample, bool renewal)
{
	if (!sample || sample > now || now - sample > T380_SS_SAMPLE_MAX_AGE_MS ||
	    (renewal && sample <= s->last_sample_ms)) return -ESTALE;
	return 0;
}
int t380_ss_begin(struct t380_ss_state *s, const struct t380_ss_io *io,
		  t380_ss_u64 now_ms, t380_ss_u64 sample_ms,
		  t380_ss_u64 (*now)(void *))
{
	int ret;
	if (!s || !io || !now || !io->high || !io->low ||
	    !io->schedule_high || !io->validate_source || !io->verify_low)
		return -EINVAL;
	if (!s->spent || !s->pending || s->active || s->latched_error)
		return -EPERM;
	if (now_ms >= s->session_deadline_ms)
		return ss_fail(s, io, -ETIME);
	ret = ss_sample(s, now_ms, sample_ms, false);
	if (ret) return ss_fail(s, io, ret);
	s->pending = false;
	s->lease_deadline_ms = ss_deadline(now_ms, s->session_deadline_ms);
	ret = io->high(io->context);
	if (ret) return ss_fail(s, io, ret);
	/* Independent HIGH is queued before LOW, and before I2C-free source check. */
	ret = io->schedule_high(io->context, s->lease_deadline_ms, true);
	if (ret) return ss_fail(s, io, ret);
	if (now(io->context) >= s->lease_deadline_ms)
		return ss_fail(s, io, -ETIME);
	ret = io->validate_source(io->context);
	if (ret) return ss_fail(s, io, ret);
	ret = io->low(io->context);
	if (ret) return ss_fail(s, io, ret);
	s->active = true;
	s->last_sample_ms = sample_ms;
	return t380_ss_check(s, io, now(io->context));
}
int t380_ss_renew(struct t380_ss_state *s, const struct t380_ss_io *io,
		  t380_ss_u64 now_ms, t380_ss_u64 sample_ms,
		  t380_ss_u64 (*now)(void *))
{
	t380_ss_u64 next;
	int ret;
	if (!s || !io || !now || !io->schedule_high) return -EINVAL;
	ret = t380_ss_check(s, io, now_ms);
	if (ret) return ret;
	ret = ss_sample(s, now_ms, sample_ms, true);
	if (ret) return ss_fail(s, io, ret);
	next = ss_deadline(now_ms, s->session_deadline_ms);
	if (next < s->lease_deadline_ms) return ss_fail(s, io, -ERANGE);
	if (next > s->lease_deadline_ms) {
		ret = io->schedule_high(io->context, next, false);
		if (ret) return ss_fail(s, io, ret);
	}
	s->lease_deadline_ms = next;
	s->last_sample_ms = sample_ms;
	return t380_ss_check(s, io, now(io->context));
}

int t380_ss_pause_plan(struct t380_ss_state *s, const struct t380_ss_io *io,
		      t380_ss_u64 now_ms, t380_ss_u64 sample_ms)
{
	int ret;

	if (!s || !io || !io->high) return -EINVAL;
	ret = t380_ss_check(s, io, now_ms);
	if (ret) return ret;
	ret = ss_sample(s, now_ms, sample_ms, true);
	if (ret) return ss_fail(s, io, ret);
	ret = io->high(io->context);
	if (ret) return ss_fail(s, io, ret);
	/* Keep active/old lease/end while physically HIGH. The independent
	 * deadline worker still revokes a paused transaction on expiry. */
	s->paused_for_plan = true;
	s->paused_sample_ms = sample_ms;
	return 0;
}

int t380_ss_resume_plan(struct t380_ss_state *s, const struct t380_ss_io *io,
		       t380_ss_u64 now_ms, t380_ss_u64 sample_ms,
		       t380_ss_u64 (*now)(void *))
{
	t380_ss_u64 next, old_lease, refreshed;
	int ret;

	if (!s || !io || !now || !io->high || !io->low ||
	    !io->schedule_high || !io->validate_source || !io->verify_low)
		return -EINVAL;
	if (!s->active || !s->paused_for_plan || s->latched_error ||
	    !sample_ms || sample_ms != s->paused_sample_ms)
		return ss_fail(s, io, -EPERM);
	if (now_ms >= s->session_deadline_ms || now_ms >= s->lease_deadline_ms)
		return ss_fail(s, io, -ETIME);
	ret = ss_sample(s, now_ms, sample_ms, true);
	if (ret) return ss_fail(s, io, ret);
	ret = io->high(io->context); /* Verify HIGH after I2C, before reentry. */
	if (ret) return ss_fail(s, io, ret);
	ret = io->validate_source(io->context);
	if (ret) return ss_fail(s, io, ret);
	old_lease = s->lease_deadline_ms;
	refreshed = now(io->context);
	if (refreshed < now_ms || refreshed >= old_lease ||
	    refreshed >= s->session_deadline_ms)
		return ss_fail(s, io, -ETIME);
	ret = ss_sample(s, refreshed, sample_ms, true);
	if (ret) return ss_fail(s, io, ret);
	next = ss_deadline(refreshed, s->session_deadline_ms);
	if (next < old_lease) return ss_fail(s, io, -ERANGE);
	if (next > old_lease) {
		ret = io->schedule_high(io->context, next, false);
		if (ret) return ss_fail(s, io, ret);
	}
	s->lease_deadline_ms = next;
	refreshed = now(io->context);
	if (refreshed >= old_lease || refreshed >= s->lease_deadline_ms ||
	    refreshed >= s->session_deadline_ms)
		return ss_fail(s, io, -ETIME);
	ret = ss_sample(s, refreshed, sample_ms, true);
	if (ret) return ss_fail(s, io, ret);
	ret = io->low(io->context);
	if (ret) return ss_fail(s, io, ret);
	s->paused_for_plan = false;
	s->paused_sample_ms = 0;
	s->last_sample_ms = sample_ms;
	return t380_ss_check(s, io, now(io->context));
}

int t380_ss_enter_full_rest(struct t380_ss_state *s,
			    const struct t380_ss_io *io, t380_ss_u64 now_ms,
			    t380_ss_u64 sample_ms, t380_ss_u64 (*now)(void *))
{
	t380_ss_u64 refreshed;
	int ret;

	if (!s || !io || !io->high || !io->schedule_high || !now)
		return -EINVAL;
	ret = t380_ss_check(s, io, now_ms);
	if (ret) return ret;
	ret = ss_sample(s, now_ms, sample_ms, true);
	if (ret) return ss_fail(s, io, ret);
	ret = io->high(io->context);
	if (ret) return ss_fail(s, io, ret);
	/* An already-entered callback can cross the old LOW lease or episode
	 * deadline while HIGH is being verified. Completion may not erase that
	 * expiry and later acquire a fresh recharge budget. */
	if (s->attached_mode) {
		refreshed = now(io->context);
		if (refreshed < now_ms || refreshed >= s->lease_deadline_ms ||
		    refreshed >= s->session_deadline_ms)
			return ss_fail(s, io, -ETIME);
		ret = ss_sample(s, refreshed, sample_ms, true);
		if (ret) return ss_fail(s, io, ret);
	}
	s->active = false;
	s->full_rest = true;
	if (s->attached_mode) {
		s->episode_open = false;
		s->completed_full = true;
	}
	s->full_sample_ms = sample_ms;
	s->last_sample_ms = sample_ms;
	/* LOW lease ends here. The development profile keeps its immutable outer
	 * cutoff; attached maintenance has completed this bounded episode and
	 * starts no recharge deadline until a later qualified transition. */
	ret = s->attached_mode ? 0 :
		io->schedule_high(io->context, s->session_deadline_ms, false);
	if (ret) return ss_fail(s, io, ret);
	return 0;
}

int t380_ss_rest_check(struct t380_ss_state *s, const struct t380_ss_io *io,
		       t380_ss_u64 now_ms)
{
	int ret;

	if (!s || !io || !s->spent || !s->full_rest || s->active ||
	    s->rest_reentry_pending || s->latched_error)
		return -EPERM;
	if (!s->attached_mode && now_ms >= s->session_deadline_ms)
		return ss_fail(s, io, -ETIME);
	ret = io->validate_source ? io->validate_source(io->context) : -EINVAL;
	return ret ? ss_fail(s, io, ret) : 0;
}

int t380_ss_recharge_pause(struct t380_ss_state *s,
			   const struct t380_ss_io *io, t380_ss_u64 now_ms,
			   t380_ss_u64 sample_ms)
{
	int ret = t380_ss_rest_check(s, io, now_ms);

	if (ret) return ret;
	if (sample_ms <= s->full_sample_ms)
		return ss_fail(s, io, -ESTALE);
	ret = ss_sample(s, now_ms, sample_ms, true);
	if (ret) return ss_fail(s, io, ret);
	if (s->attached_mode) {
		if (!s->completed_full || s->episode_open ||
		    now_ms > (t380_ss_u64)~(t380_ss_u64)0 -
			T380_SS_ATTACHED_RECHARGE_MS)
			return ss_fail(s, io, -ERANGE);
		s->pending_recharge_deadline_ms = now_ms +
			T380_SS_ATTACHED_RECHARGE_MS;
	}
	s->rest_reentry_pending = true;
	s->rest_pending_sample_ms = sample_ms;
	return 0;
}

int t380_ss_recharge_resume(struct t380_ss_state *s,
			    const struct t380_ss_io *io, t380_ss_u64 now_ms,
			    t380_ss_u64 sample_ms,
			    t380_ss_u64 (*now)(void *))
{
	t380_ss_u64 next, refreshed, end;
	int ret;

	if (!s || !io || !now || !io->high || !io->low ||
	    !io->schedule_high || !io->validate_source || !io->verify_low)
		return -EINVAL;
	if (!s->full_rest || s->active || !s->rest_reentry_pending ||
	    s->latched_error || !sample_ms ||
	    sample_ms != s->rest_pending_sample_ms ||
	    sample_ms <= s->full_sample_ms)
		return ss_fail(s, io, -EPERM);
	end = s->attached_mode ? s->pending_recharge_deadline_ms :
		s->session_deadline_ms;
	if (!end || now_ms >= end)
		return ss_fail(s, io, -ETIME);
	ret = ss_sample(s, now_ms, sample_ms, true);
	if (ret) return ss_fail(s, io, ret);
	ret = io->high(io->context);
	if (ret) return ss_fail(s, io, ret);
	ret = io->validate_source(io->context);
	if (ret) return ss_fail(s, io, ret);
	refreshed = now(io->context);
	if (refreshed < now_ms || refreshed >= end)
		return ss_fail(s, io, -ETIME);
	ret = ss_sample(s, refreshed, sample_ms, true);
	if (ret) return ss_fail(s, io, ret);
	next = ss_deadline(refreshed, end);
	ret = io->schedule_high(io->context, next, false);
	if (ret) return ss_fail(s, io, ret);
	refreshed = now(io->context);
	if (refreshed >= next || refreshed >= end)
		return ss_fail(s, io, -ETIME);
	ret = ss_sample(s, refreshed, sample_ms, true);
	if (ret) return ss_fail(s, io, ret);
	s->lease_deadline_ms = next;
	s->session_deadline_ms = end;
	ret = io->low(io->context);
	if (ret) return ss_fail(s, io, ret);
	s->full_rest = false;
	s->rest_reentry_pending = false;
	s->rest_pending_sample_ms = 0;
	s->active = true;
	if (s->attached_mode) {
		s->episode_open = true;
		s->completed_full = false;
		++s->episode_number;
		s->episode_start_ms = now_ms;
		s->pending_recharge_deadline_ms = 0;
	}
	s->last_sample_ms = sample_ms;
	return t380_ss_check(s, io, now(io->context));
}
