/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_CHARGE_POLICY_H
#define T380_CHARGE_POLICY_H

/* Standalone candidate model. No kernel ABI or hardware operations. */
#ifdef __KERNEL__
#include <linux/types.h>
#ifndef UINT64_C
#define UINT64_C(value) value##ULL
#endif
#else
#include <stdbool.h>
#include <stdint.h>
#endif

#include "../power066-regular/t380_regular_full.h"

#define T380_CP_MAX_SAMPLE_AGE_MS UINT64_C(30000)
#define T380_CP_REFERENCE_MA UINT64_C(1883)
#define T380_CP_INITIAL_BUDGET_MS UINT64_C(17400000)
#define T380_CP_RECHARGE_BUDGET_MS UINT64_C(5400000)
#define T380_CP_SESSION_LIMIT_MS UINT64_C(36000000)

enum t380_cp_source {
    T380_CP_SOURCE_NONE,
    T380_CP_SOURCE_USB,
    T380_CP_SOURCE_DCP,
    T380_CP_SOURCE_CDP,
    T380_CP_SOURCE_MAINS,
    T380_CP_SOURCE_UNKNOWN,
    T380_CP_SOURCE_QUALIFIED_FIXED5V_TIMEOUT,
    T380_CP_SOURCE_OPERATOR900_FIXED5V_TIMEOUT
};

enum t380_cp_valid {
    T380_CP_VALID_TEMP = 1u << 0,
    T380_CP_VALID_VOLTAGE = 1u << 1,
    T380_CP_VALID_CURRENT = 1u << 2,
    T380_CP_VALID_AVERAGE = 1u << 3,
    T380_CP_VALID_SOC = 1u << 4,
    T380_CP_VALID_CHARGER = 1u << 5,
    T380_CP_VALID_SOURCE = 1u << 6,
    T380_CP_VALID_AGE = 1u << 7,
    T380_CP_VALID_ALL = 255u
};

enum t380_cp_phase {
    T380_CP_FIRST,
    T380_CP_SECOND,
    T380_CP_FULL
};

enum t380_cp_thermal {
    T380_CP_THERMAL_NORMAL,
    T380_CP_THERMAL_HOT,
    T380_CP_THERMAL_COLD,
    T380_CP_THERMAL_HIGH_LIMIT
};

enum t380_cp_reason {
    T380_CP_WOULD_CHARGE,
    T380_CP_NO_SOURCE,
    T380_CP_UNVALIDATED_SOURCE,
    T380_CP_UNSUPPORTED_SOURCE,
    T380_CP_UNVALIDATED_TELEMETRY,
    T380_CP_UNSUPPORTED_CHARGE_REGION,
    T380_CP_ABOVE_FLOAT_VOLTAGE,
    T380_CP_UNSUPPORTED_AGE,
    T380_CP_SOURCE_SUSPENDED,
    T380_CP_SYSTEM_SUSPENDED,
    T380_CP_THERMAL_BLOCK,
    T380_CP_SWELLING_BLOCK,
    T380_CP_FULL_STOP,
    T380_CP_INVALID_TELEMETRY,
    T380_CP_STALE_TELEMETRY,
    T380_CP_TIME_REGRESSION,
    T380_CP_SAMPLE_REGRESSION,
    T380_CP_CHARGER_FAULT,
    T380_CP_WEIGHTED_TIMEOUT,
    T380_CP_SESSION_TIMEOUT,
    T380_CP_INVALID_ARGUMENT,
    T380_CP_HARDWARE_DONE_INCOMPLETE,
    T380_CP_HARDWARE_STATE_MISMATCH
};

struct t380_cp_input {
    uint64_t now_ms;             /* Monotonic boottime, includes suspend. */
    uint64_t sample_ms;          /* Oldest member of one coherent snapshot. */
    uint64_t thermal_sample_ms;  /* Actual temperature acquisition, not oldest member. */
    unsigned int valid;
    bool telemetry_validated;   /* External provenance; model cannot prove it. */
    bool normal_charge_region_validated; /* Precharge classification is external. */
    bool source_validated;
    bool charger_healthy;
    bool source_suspended;
    bool system_suspended;
    enum t380_cp_source source;
    int source_voltage_mv;      /* Only validated 5V wired input supported. */
    int source_allowance_ma;    /* Explicit source premise; negotiated for USB. */
    int temp_decic;
    int voltage_mv;
    int current_ma;
    int average_current_ma;     /* Validated average, never synthesized here. */
    int soc_percent;
    bool regular_profile, regular_model_matched, regular_completion_allowed;
    int reported_capacity; /* Periodic cached stock scaling, not raw SOC. */
    int age_step;               /* Selected stock row0..4, not measured age.
                                * regular row0 is explicit unknown-age default. */
};

struct t380_cp_output {
    bool would_charge;         /* Hypothetical ONLY; no actuation API exists. */
    enum t380_cp_reason reason;
    enum t380_cp_phase phase;
    enum t380_cp_thermal thermal;
    bool swelling_blocked;
    bool low_temp_derated;
    int input_limit_ma;
    int charge_limit_ma;
    int float_voltage_mv;
    int topoff_ma;
    uint64_t remaining_budget_ma_ms;
};

struct t380_cp_state {
    bool initialized;
    bool attached;
    bool previous_would_charge;
    bool swelling_blocked;
    bool low_temp_derated;
    bool recharging;
    bool have_full_sample;
    bool have_thermal_sample;
    bool fault_latched;
    bool attached_maintenance, episode_open, full_pending, recharge_pending;
    bool household_mode, household_detached;
    uint64_t household_remaining_wall_ms;
    int strictest_age_step;
    int applied_input_ma, applied_charge_ma, applied_float_mv;
    unsigned int previous_effective_ma;
    enum t380_cp_phase phase;
    enum t380_cp_thermal thermal;
    enum t380_cp_reason fault_reason;
    uint64_t last_now_ms;
    uint64_t last_sample_ms;
    uint64_t last_full_sample_ms;
    uint64_t last_thermal_sample_ms;
    uint64_t session_start_ms;
    uint64_t recharge_start_ms;
    uint64_t remaining_budget_ma_ms;
    uint64_t episode_start_ms;
    unsigned int episode_number;
};

/* Init is an explicit new simulation. Never use it to recover an active charger. */
void t380_charge_policy_init(struct t380_cp_state *state);
bool t380_charge_policy_attached_mode(struct t380_cp_state *state);
bool t380_charge_policy_household_mode(struct t380_cp_state *state);
bool t380_charge_policy_household_detach(struct t380_cp_state *state,
                                         uint64_t now_ms);
bool t380_charge_policy_household_rebind(struct t380_cp_state *state,
                                         uint64_t now_ms);
bool t380_charge_policy_household_ack_second(struct t380_cp_state *state,
                                             uint64_t now_ms);
bool t380_charge_policy_ack_full(struct t380_cp_state *state,
                                  uint64_t sample_ms);
bool t380_charge_policy_ack_recharge(struct t380_cp_state *state,
                                      uint64_t sample_ms);
void t380_charge_policy_step(struct t380_cp_state *state,
                            const struct t380_cp_input *input,
                            struct t380_cp_output *output);
/* Restrictive accounting step: preserve all guards/debit, prohibit charging
 * phase completion from a hardware-stopped or contradictory active sample. */
void t380_charge_policy_step_without_completion(struct t380_cp_state *state,
                            const struct t380_cp_input *input,
                            struct t380_cp_output *output);
const char *t380_charge_policy_reason(enum t380_cp_reason reason);

#endif
