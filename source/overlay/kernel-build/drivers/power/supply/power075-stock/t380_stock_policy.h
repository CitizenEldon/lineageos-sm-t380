/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_STOCK_POLICY_H
#define T380_STOCK_POLICY_H
#ifdef __KERNEL__
#include <linux/types.h>
typedef u64 t380_stock_u64;
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t t380_stock_u64;
#endif

/* Board r05 + enabled Samsung battery_v2_legacy policy, without UI/host
 * enrollment, model fingerprint, display admission or development budgets.
 * This pure policy cannot operate hardware. The owner must provide real
 * measurements, source classification, register feedback and OFF-on-error. */
enum t380_stock_source { T380_STOCK_ABSENT, T380_STOCK_SDP,
 T380_STOCK_CDP, T380_STOCK_MAINS, T380_STOCK_TIMEOUT,
 T380_STOCK_SOURCE_UNKNOWN };
enum t380_stock_health { T380_STOCK_GOOD, T380_STOCK_HOT,
 T380_STOCK_COLD, T380_STOCK_HOT_LIMIT };
enum t380_stock_phase { T380_STOCK_FIRST, T380_STOCK_SECOND,
 T380_STOCK_REST };
enum t380_stock_reason { T380_STOCK_CHARGING, T380_STOCK_NO_SOURCE,
 T380_STOCK_SOURCE_UNCLASSIFIED, T380_STOCK_MEASUREMENT_ERROR,
 T380_STOCK_HARDWARE_FAULT, T380_STOCK_BATTERY_ABSENT,
 T380_STOCK_THERMAL_STOP, T380_STOCK_SWELLING_REST,
 T380_STOCK_FULL, T380_STOCK_SAFETY_TIMER };
struct t380_stock_age { int cycles, float_mv, recharge_mv, full_mv, full_soc; };
struct t380_stock_input {
 t380_stock_u64 now_ms;
 enum t380_stock_source source;
 bool measurements_ok, battery_present, hardware_ok, charging_feedback;
 bool monitor_tick; /* Full/negative-current sample counts use stock30s cadence. */
 bool display_on, usb_configured, charger_mode;
 int temperature_decic, voltage_mv, current_ma, average_ma, capacity;
 int cycle_count; /* -1 means unavailable, never claimed to mean new. */
 int aicl_input_ma; /* 0: no lower measured AICL limit. */
};
struct t380_stock_output {
 bool charge, full, age_known;
 bool full_candidate; /* Qualifying measured current; drives stock SOC learning. */
 enum t380_stock_reason reason;
 int input_ma, fast_ma, float_mv, topoff_ma, age_step;
 enum t380_stock_phase phase;
 enum t380_stock_health health;
};
struct t380_stock_state {
 bool initialized, attached, was_charging, previous_display_on;
 bool low_current, swelling, swelling_charging, swelling_full, recharging;
 bool timer_expired;
 int age_step, negative_samples;
 enum t380_stock_phase phase;
 enum t380_stock_health health;
 t380_stock_u64 last_ms, remaining_ma_ms;
 int last_effective_ma;
};
extern const struct t380_stock_age t380_stock_ages[5];
void t380_stock_init(struct t380_stock_state *s);
void t380_stock_step(struct t380_stock_state *s,
 const struct t380_stock_input *in, struct t380_stock_output *out);
#endif
