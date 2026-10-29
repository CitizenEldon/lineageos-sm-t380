/* SPDX-License-Identifier: GPL-2.0-only */
#include "t380_trial_lease.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

int t380_tl_off(struct t380_tl_state *s, const struct t380_tl_io *io, int reason)
{
	int ret;
	if (!s || !io || !io->high) return -EINVAL;
	/* State is revoked before any potentially sleeping GPIO operation. */
	s->active = false;
	ret = io->high(io->context);
	if (reason && !s->last_error) s->last_error = reason;
	if (ret && !s->last_error) s->last_error = ret;
	return ret;
}
int t380_tl_check(struct t380_tl_state *s, const struct t380_tl_io *io,
		  t380_tl_u64 now_ms)
{
	int ret;
	if (!s || !io || !s->active) return -EPERM;
	if (now_ms >= s->deadline_ms) {
		t380_tl_off(s, io, -ETIME);
		return -ETIME;
	}
	ret = io->verify_low ? io->verify_low(io->context) : -EINVAL;
	if (ret) t380_tl_off(s, io, ret);
	return ret;
}
int t380_tl_begin(struct t380_tl_state *s, const struct t380_tl_io *io,
		  t380_tl_u64 now_ms, t380_tl_u64 (*now)(void *))
{
	int ret;
	if (!s || !io || !now || !io->high || !io->low ||
	    !io->schedule_high || !io->validate_source || !io->verify_low) return -EINVAL;
	if (s->spent || s->active) return -EPERM;
	s->spent = true; /* Even failed starts cannot rearm this boot. */
	if (now_ms > (t380_tl_u64)~(t380_tl_u64)0 - T380_TL_DURATION_MS)
		return t380_tl_off(s, io, -ERANGE) ? -EIO : -ERANGE;
	s->deadline_ms = now_ms + T380_TL_DURATION_MS;
	ret = io->high(io->context);
	if (ret) return t380_tl_off(s, io, ret) ? -EIO : ret;
	/* Reserve the independent HIGH before touching the LOW value. */
	ret = io->schedule_high(io->context, s->deadline_ms);
	if (ret) return t380_tl_off(s, io, ret) ? -EIO : ret;
	if (now(io->context) >= s->deadline_ms)
		return t380_tl_off(s, io, -ETIME) ? -EIO : -ETIME;
	ret = io->validate_source(io->context);
	if (ret) return t380_tl_off(s, io, ret) ? -EIO : ret;
	ret = io->low(io->context);
	if (ret) return t380_tl_off(s, io, ret) ? -EIO : ret;
	s->active = true;
	ret = t380_tl_check(s, io, now(io->context));
	return ret;
}
