/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_TRIAL_LEASE_H
#define T380_TRIAL_LEASE_H
#ifdef __KERNEL__
#include <linux/types.h>
typedef u64 t380_tl_u64;
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t t380_tl_u64;
#endif

#define T380_TL_DURATION_MS 10000ULL
struct t380_tl_state {
	bool spent, active;
	t380_tl_u64 deadline_ms;
	int last_error;
};
struct t380_tl_io {
	void *context;
	/* All calls are made under the exclusive GPIO owner lock. */
	int (*high)(void *);
	int (*low)(void *);
	int (*schedule_high)(void *, t380_tl_u64);
	int (*validate_source)(void *);
	int (*verify_low)(void *);
};
int t380_tl_begin(struct t380_tl_state *, const struct t380_tl_io *,
		  t380_tl_u64 now_ms, t380_tl_u64 (*now)(void *));
int t380_tl_check(struct t380_tl_state *, const struct t380_tl_io *,
		  t380_tl_u64 now_ms);
int t380_tl_off(struct t380_tl_state *, const struct t380_tl_io *, int reason);
#endif
