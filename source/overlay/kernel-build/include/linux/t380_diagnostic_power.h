/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_T380_DIAGNOSTIC_POWER_H
#define _LINUX_T380_DIAGNOSTIC_POWER_H

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/of.h>

#if IS_ENABLED(CONFIG_T380_DIAGNOSTIC_POWER)
int t380_charge_inhibit_check(struct device *consumer);
/* High-only safety owner APIs; ready is ownership, never charging permission. */
int t380_charge_safety_ready(struct device *consumer);
int t380_charge_force_off(struct device *consumer);
/* Diagnostic source records only. No charge/GPIO/USB setter is called. */
void t380_source_usb_request(unsigned int ma);
void t380_source_usb_suspend(bool value);
void t380_source_system_suspend(bool value);
void t380_source_disconnect(void);
#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
void t380_source_host_setup(void);
#endif
#else
static inline int t380_charge_safety_ready(struct device *consumer) { return -ENODEV; }
static inline int t380_charge_force_off(struct device *consumer) { return -ENODEV; }
static inline void t380_source_usb_request(unsigned int ma) { }
static inline void t380_source_usb_suspend(bool value) { }
static inline void t380_source_system_suspend(bool value) { }
static inline void t380_source_disconnect(void) { }
static inline int t380_charge_inhibit_check(struct device *consumer)
{
	return of_find_property(consumer->of_node, "samsung,charge-inhibit",
				NULL) ? -ENODEV : 0;
}
#endif
#endif
