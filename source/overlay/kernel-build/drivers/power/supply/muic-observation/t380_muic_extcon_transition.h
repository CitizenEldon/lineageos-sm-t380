/* SPDX-License-Identifier: GPL-2.0-only */
/* Same transition decision is exercised by the host failure fixture. */
#ifndef T380_MUIC_EXTCON_TRANSITION_H
#define T380_MUIC_EXTCON_TRANSITION_H
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

struct t380_ex_io {
	void *context;
	int (*get)(void *context);
	int (*set_sync)(void *context, int attached);
	int (*notify)(void *context, int attached);
};

static inline int t380_ex_publish(const struct t380_ex_io *io, int observed,
				  int *cached, int *notify_pending)
{
	int committed, published = 0, notified;

	if (!io || !io->get || !io->set_sync || !io->notify || !cached ||
	    !notify_pending || (observed != 0 && observed != 1))
		return -EINVAL;
	committed = io->get(io->context);
	if (committed < 0)
		return committed;
	if (committed != observed) {
		published = io->set_sync(io->context, observed);
		/* set_sync may have committed before a later uevent failure. */
		committed = io->get(io->context);
		if (committed >= 0)
			*cached = !!committed;
		if (committed != observed)
			return published < 0 ? published :
				(committed < 0 ? committed : -EIO);
		*notify_pending = 1;
	} else {
		*cached = !!committed;
	}
	if (!*notify_pending)
		return 0;
	notified = io->notify(io->context, observed);
	if (!notified)
		*notify_pending = 0;
	return published < 0 ? published : notified;
}
#endif
