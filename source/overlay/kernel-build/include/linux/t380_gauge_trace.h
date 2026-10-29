/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_T380_GAUGE_TRACE_H
#define _LINUX_T380_GAUGE_TRACE_H
#include <linux/types.h>
struct i2c_adapter;
struct i2c_msg;
/* Observer only: it cannot change a message, return value or retry policy.
 * Callback runs briefly under RCU; never sleep, allocate or perform I2C. */
struct t380_gauge_trace_observer {
 struct i2c_adapter *adapter;
 unsigned short address;
 void *context;
 u64 (*begin)(void *, const struct i2c_msg *, int, int);
 void (*end)(void *, u64, const struct i2c_msg *, int, int);
 u64 registration;
};
int t380_gauge_trace_register(struct t380_gauge_trace_observer *);
void t380_gauge_trace_unregister(struct t380_gauge_trace_observer *);
#endif
