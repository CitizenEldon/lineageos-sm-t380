/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_DISPLAY_ADMISSION_H
#define T380_DISPLAY_ADMISSION_H
#include <linux/types.h>
#include <linux/errno.h>
struct t380_display_snapshot {
 u64 epoch, off_completed_ms;
 bool known_off;
};
#if defined(CONFIG_T380_HOUSEHOLD_CHARGE_DEV) && defined(CONFIG_FB_MSM_MDSS)
void t380_display_snapshot(struct t380_display_snapshot *out);
/* Success holds the display mutex until leave; take it BEFORE the GPIO owner. */
int t380_display_guard_enter(u64 epoch);
void t380_display_guard_leave(void);
#else
static inline void t380_display_snapshot(struct t380_display_snapshot *out)
{ *out = (struct t380_display_snapshot){0}; }
static inline int t380_display_guard_enter(u64 epoch) { return -EOPNOTSUPP; }
static inline void t380_display_guard_leave(void) { }
#endif
#endif
