/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_PERIODIC_POLICY_H
#define T380_PERIODIC_POLICY_H
#include "t380_charge_policy.h"
#include "../power027-acquisition/t380_power_acquisition.h"
#include "../source-lifecycle/t380_source_lifecycle.h"
#include "../charger-observation/t380_charger_observation.h"
#include "../muic-observation/t380_muic_observation.h"

#define T380_PP_INTERVAL_MS 2000ULL
#define T380_PP_ACQUISITION_LIMIT_MS 2000ULL
struct t380_pp_cycle {
    struct t380_sl_epoch_record epoch_start, epoch_final;
    uint64_t off_before_start_ms, off_before_end_ms, off_after_start_ms, off_after_end_ms;
    unsigned int epoch_mismatch_flags;
    struct t380_co_observation charger[2];
    struct t380_mo_observation muic;
    struct t380_pa_tuple battery;
    struct t380_sl_snapshot source;
    uint64_t started_ms, ended_ms, charger_started[2], charger_ended[2];
    uint64_t muic_started_ms, muic_ended_ms, epoch_started;
    int charger_error[2], muic_error, battery_error, readback_error;
    int off_before_error, off_after_error;
    bool trial_active, detach_cancelled;
};
/* Observability only: the existing strict cross-epoch predicate is unchanged. */
#define T380_PP_EPOCH_START_GENERATION 1U
#define T380_PP_EPOCH_BATTERY_BEFORE_GENERATION 2U
#define T380_PP_EPOCH_BATTERY_AFTER_GENERATION 4U
#define T380_PP_EPOCH_START_PM 8U
#define T380_PP_EPOCH_BATTERY_BEFORE_PM 16U
#define T380_PP_EPOCH_BATTERY_AFTER_PM 32U
#define T380_PP_EPOCH_START_FAULT 64U
#define T380_PP_EPOCH_BATTERY_BEFORE_FAULT 128U
#define T380_PP_EPOCH_BATTERY_AFTER_FAULT 256U
static inline unsigned int t380_pp_epoch_flags(const struct t380_pp_cycle *c)
{
 const struct t380_sl_epoch_record *s[3] = {&c->epoch_start,
  &c->battery.epoch_record[0], &c->battery.epoch_record[1]};
 unsigned int flags = 0, i;
 for (i = 0; i < 3; ++i) {
  if (s[i]->generation != c->epoch_final.generation) flags |= 1U << i;
  if (s[i]->pm_serial != c->epoch_final.pm_serial) flags |= 8U << i;
  if (s[i]->fault_serial != c->epoch_final.fault_serial) flags |= 64U << i;
 }
 return flags;
}
struct t380_pp_state {
    struct t380_cp_state policy;
    uint64_t evaluations, last_ms;
    int latched_error;
};
struct t380_pp_result {
    struct t380_cp_input input;
    struct t380_cp_output decision;
    int cycle_error, latched_error, integrity_error;
    bool acquisition_valid, permission;
};
void t380_pp_init(struct t380_pp_state *state);
void t380_pp_evaluate(struct t380_pp_state *, const struct t380_pp_cycle *,
                       struct t380_pp_result *);
#endif
