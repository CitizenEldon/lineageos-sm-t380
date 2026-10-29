/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_POWER_ACQUISITION_H
#define T380_POWER_ACQUISITION_H
#ifdef __KERNEL__
#include <linux/types.h>
typedef u64 t380_pa_u64;
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t t380_pa_u64;
#endif

#include "../power066-regular/t380_regular_full.h"
#include "../source-lifecycle/t380_source_lifecycle.h"

enum t380_pa_field { T380_PA_TEMP, T380_PA_VOLTAGE, T380_PA_CURRENT,
                      T380_PA_SOC, T380_PA_FIELDS };
#define T380_PA_HISTORY_MIN_MS 1000ULL
#define T380_PA_HISTORY_MAX_MS 30000ULL
struct t380_pa_field_sample {
    t380_pa_u64 started_ms, ended_ms;
    int raw, value, error;
};
struct t380_pa_history {
    t380_pa_u64 epoch, last_ms;
    int previous_ma, average_ma;
    unsigned int samples;
};
struct t380_pa_tuple {
    t380_pa_u64 acquisition_lifetime, acquisition_request;
    int acquisition_cancel_reason, acquisition_physical_error;
    struct t380_sl_epoch_record epoch_record[2];
    t380_pa_u64 guard_before_start_ms, guard_before_end_ms;
    t380_pa_u64 guard_after_start_ms, guard_after_end_ms;
    struct t380_pa_field_sample field[T380_PA_FIELDS];
    t380_pa_u64 started_ms, ended_ms, epoch_before, epoch_after;
    unsigned int valid;
    int guard_before_error, guard_after_error;
    int average_ma;
    bool epoch_coherent, tuple_valid, history_ready;
    bool conservative_voltage_blocked;
    /* True only on the no-physical-error typed household cancellation path. */
    bool guard_cancelled;
    /* Unconditionally false; software history is not validated provenance. */
    bool telemetry_validated, average_validated, source_validated;
    bool normal_charge_region_validated, current_permission;
};
struct t380_pa_reader {
    void *context;
    int (*guard)(void *);
    int (*word)(void *, unsigned char);
    int (*temperature)(void *, int *adc, int *decic);
    t380_pa_u64 (*now)(void *);
    t380_pa_u64 (*epoch)(void *);
    /* Revokes software source state only; no hardware setter exists. */
    void (*revoke_epoch)(void *);
    bool stock_soc; /* Only a selected, matching regular model reader. */
};
struct t380_pa_profile {
    int selected_row, float_mv, full_mv, full_soc, recharge_mv;
    bool conservative_profile, measured_age_known;
};
/* Caller serializes tuple collection AND history. Per-field errno retained. */
int t380_pa_acquire(const struct t380_pa_reader *, struct t380_pa_history *,
                     struct t380_pa_tuple *);
struct t380_pa_profile t380_pa_conservative_profile(void);
struct t380_pa_profile t380_pa_stock_default_profile(void);
#endif
