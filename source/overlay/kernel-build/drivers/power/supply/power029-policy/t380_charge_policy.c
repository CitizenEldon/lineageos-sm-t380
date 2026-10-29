/* SPDX-License-Identifier: GPL-2.0-only */
#include "t380_charge_policy.h"

/* Samsung 36c20b2 Rev05 age_data, explicit rows: no age inference. */
struct age_limits { int cv, recharge, full_voltage, full_soc; };
static const struct age_limits ages[] = {
    {4300, 4230, 4200, 93}, {4280, 4210, 4180, 92},
    {4260, 4190, 4160, 91}, {4240, 4170, 4140, 90},
    {4190, 4120, 4090, 89}
};

static int lesser(int a, int b) { return a < b ? a : b; }

void t380_charge_policy_init(struct t380_cp_state *s)
{
    if (s)
        *s = (struct t380_cp_state){ .strictest_age_step = -1 };
}

bool t380_charge_policy_attached_mode(struct t380_cp_state *s)
{
    if (!s || s->initialized || s->attached || s->fault_latched)
        return false;
    s->attached_maintenance = true;
    return true;
}

bool t380_charge_policy_household_mode(struct t380_cp_state *s)
{
    if (!t380_charge_policy_attached_mode(s)) return false;
    s->household_mode = true;
    return true;
}

/* Called only after owner HIGH and fresh dual physical absence. Debit the
 * entire uncertain interval up to that proof, then freeze an incomplete
 * episode's weighted remainder. Wall time continues while detached; neither
 * a new cable nor a process restart initializes a new charging episode. */
bool t380_charge_policy_household_detach(struct t380_cp_state *s,
                                         uint64_t now_ms)
{
    uint64_t elapsed, cap, available_ms;

    if (!s || !s->household_mode || !s->attached || s->household_detached ||
        s->fault_latched || !s->initialized || now_ms < s->last_now_ms)
        return false;
    elapsed = now_ms - s->last_now_ms;
    if (elapsed > T380_CP_MAX_SAMPLE_AGE_MS) return false;
    if (s->episode_open) {
        cap = s->episode_number == 1 ? T380_CP_SESSION_LIMIT_MS :
              T380_CP_RECHARGE_BUDGET_MS;
        if (now_ms < s->episode_start_ms ||
            now_ms - s->episode_start_ms >= cap) return false;
        s->household_remaining_wall_ms = cap -
            (now_ms - s->episode_start_ms);
        if (s->previous_would_charge && elapsed) {
            if (!s->previous_effective_ma) return false;
            available_ms = s->remaining_budget_ma_ms /
                s->previous_effective_ma;
            if (elapsed >= available_ms) return false;
            s->remaining_budget_ma_ms =
                ((available_ms - elapsed) * s->previous_effective_ma /
                 T380_CP_REFERENCE_MA) * T380_CP_REFERENCE_MA;
            if (!s->remaining_budget_ma_ms) return false;
        }
    } else if (!s->have_full_sample || s->phase != T380_CP_FULL) {
        return false;
    }
    s->previous_would_charge = false;
    s->previous_effective_ma = 0;
    s->last_now_ms = now_ms;
    s->attached = false;
    s->household_detached = true;
    return true;
}

bool t380_charge_policy_household_rebind(struct t380_cp_state *s,
                                         uint64_t now_ms)
{
    uint64_t cap, remaining;

    if (!s || !s->household_mode || !s->household_detached || s->attached ||
        s->fault_latched || now_ms < s->last_now_ms) return false;
    if (s->episode_open) {
        cap = s->episode_number == 1 ? T380_CP_SESSION_LIMIT_MS :
              T380_CP_RECHARGE_BUDGET_MS;
        if (!s->household_remaining_wall_ms ||
            s->household_remaining_wall_ms > cap) return false;
        if (!s->episode_number || now_ms < s->episode_start_ms ||
            now_ms - s->episode_start_ms >= cap) return false;
        remaining = cap - (now_ms - s->episode_start_ms);
        if (remaining > s->household_remaining_wall_ms) return false;
        /* The off interval pauses weighted debit, never the wall cap. */
        s->household_remaining_wall_ms = remaining;
    }
    s->last_now_ms = now_ms;
    s->previous_would_charge = false;
    s->previous_effective_ma = 0;
    s->attached = true;
    s->household_detached = false;
    return true;
}

bool t380_charge_policy_ack_full(struct t380_cp_state *s,
                                  uint64_t sample_ms)
{
    if (!s || !s->attached_maintenance || !s->episode_open ||
        !s->full_pending || s->phase != T380_CP_FULL ||
        s->last_full_sample_ms != sample_ms || s->fault_latched)
        return false;
    s->full_pending = false;
    s->episode_open = false;
    s->recharging = false;
    return true;
}

bool t380_charge_policy_ack_recharge(struct t380_cp_state *s,
                                      uint64_t sample_ms)
{
    if (!s || !s->attached_maintenance || s->episode_open ||
        !s->recharge_pending || s->phase != T380_CP_FIRST ||
        s->last_full_sample_ms != sample_ms || s->fault_latched)
        return false;
    s->recharge_pending = false;
    s->episode_open = true;
    ++s->episode_number;
    return true;
}

static void publish(struct t380_cp_state *s, struct t380_cp_output *o,
                    enum t380_cp_reason reason, bool latch)
{
    if (latch) {
        s->fault_latched = true;
        s->fault_reason = reason;
    }
    o->reason = reason;
    o->phase = s->phase;
    o->thermal = s->thermal;
    o->swelling_blocked = s->swelling_blocked;
    o->low_temp_derated = s->low_temp_derated;
    o->remaining_budget_ma_ms = s->remaining_budget_ma_ms;
    if (reason != T380_CP_WOULD_CHARGE) {
        o->would_charge = false;
        o->input_limit_ma = 0;
        o->charge_limit_ma = 0;
        o->float_voltage_mv = 0;
        o->topoff_ma = 0;
        s->previous_would_charge = false;
        s->previous_effective_ma = 0;
    }
}

/* Restriction is never gated on source eligibility or a new timestamp. */
static void observe_thermal_restrictions(struct t380_cp_state *s, int t)
{
    if (t >= 800)
        s->thermal = T380_CP_THERMAL_HIGH_LIMIT;
    else if (t <= 0 && s->thermal != T380_CP_THERMAL_HIGH_LIMIT)
        s->thermal = T380_CP_THERMAL_COLD;
    else if (t >= 500 && s->thermal != T380_CP_THERMAL_HIGH_LIMIT)
        s->thermal = T380_CP_THERMAL_HOT;
    if (t >= 410 || t <= 50)
        s->swelling_blocked = true;
    if (t <= 150)
        s->low_temp_derated = true;
}

/* Only a demonstrably new, valid, fresh temperature may relax a restriction. */
static void recover_thermal(struct t380_cp_state *s, int t)
{
    if (s->thermal == T380_CP_THERMAL_HIGH_LIMIT) {
        if (t > 0 && t <= 750)
            s->thermal = T380_CP_THERMAL_HOT;
    } else if (t >= 50 && t <= 450)
        s->thermal = T380_CP_THERMAL_NORMAL;
    if (t >= 100 && t <= 390)
        s->swelling_blocked = false;
    if (t >= 200)
        s->low_temp_derated = false;
}

static bool invalid_trusted_telemetry(const struct t380_cp_input *i)
{
    /* Absent validity is not permission to interpret that field's value. */
    return ((i->valid & T380_CP_VALID_TEMP) &&
            (i->temp_decic < -200 || i->temp_decic > 900)) ||
           ((i->valid & T380_CP_VALID_VOLTAGE) &&
            (i->voltage_mv < 2500 || i->voltage_mv > 4430)) ||
           ((i->valid & T380_CP_VALID_SOC) &&
            (i->soc_percent < 0 || i->soc_percent > 100)) ||
           ((i->valid & T380_CP_VALID_CURRENT) &&
            (i->current_ma < -8000 || i->current_ma > 8000)) ||
           ((i->valid & T380_CP_VALID_AVERAGE) &&
            (i->average_current_ma < -8000 || i->average_current_ma > 8000));
}

/* Only the selected operator900 household route uses electrical completion.
 * The phase adapter binds that source to the explicit900 window and qualified
 * allowance. Keep stock age rows and legacy SOC completion unchanged.4160 is
 * an operational tolerance:30mV below4190CV and40mV above4120recharge.
 * It accommodates gauge voltage quantization, not chemical/gauge FULL. */
static bool completion_voltage_soc(const struct t380_cp_state *s,
                                   const struct t380_cp_input *i,
                                   const struct age_limits *age)
{
    if (i->regular_profile)
        return s->household_mode && i->regular_model_matched &&
            i->regular_completion_allowed &&
            i->source == T380_CP_SOURCE_OPERATOR900_FIXED5V_TIMEOUT &&
            i->source_allowance_ma == 900 && s->strictest_age_step == 0 &&
            i->reported_capacity == 100 &&
            i->voltage_mv >= T380_RF_FULL_MIN_MV &&
            i->voltage_mv <= T380_RF_FLOAT_MV;
    if (s->household_mode &&
        i->source == T380_CP_SOURCE_OPERATOR900_FIXED5V_TIMEOUT &&
        i->source_allowance_ma == 900 && s->strictest_age_step == 4 &&
        age->cv == 4190 && age->recharge == 4120)
        return i->voltage_mv >= 4160 && i->voltage_mv <= age->cv;
    return i->voltage_mv >= age->full_voltage &&
           i->soc_percent >= age->full_soc;
}

static unsigned int effective_current(int input, int charge, int cv)
{
    /* Stock current-weighted budget: input 5V, 90% when input limited. */
    int input_power = input * 5000;
    if (input_power > charge * cv)
        return (unsigned int)charge;
    return (unsigned int)((input_power / cv) * 9 / 10);
}

/* A SECOND rebind first plans while HIGH without stepping policy, to avoid
 * treating pre-250 mA telemetry as SECOND completion. Start weighted debit
 * only after the verified 250 mA plan and LOW have both been acknowledged. */
bool t380_charge_policy_household_ack_second(struct t380_cp_state *s,
                                             uint64_t now_ms)
{
    uint64_t cap;

    if (!s || !s->household_mode || !s->attached || !s->episode_open ||
        s->household_detached || s->fault_latched ||
        s->phase != T380_CP_SECOND || s->previous_would_charge ||
        now_ms < s->last_now_ms || now_ms < s->episode_start_ms ||
        !s->remaining_budget_ma_ms) return false;
    cap = s->episode_number == 1 ? T380_CP_SESSION_LIMIT_MS :
        T380_CP_RECHARGE_BUDGET_MS;
    if (now_ms - s->episode_start_ms >= cap) return false;
    s->last_now_ms = now_ms;
    s->previous_effective_ma = effective_current(
        s->applied_input_ma ? s->applied_input_ma : 500,
        s->applied_charge_ma ? s->applied_charge_ma : 500,
        s->applied_float_mv ? s->applied_float_mv : 4190);
    s->previous_would_charge = true;
    return true;
}

static void charge_policy_step(struct t380_cp_state *s,
                               const struct t380_cp_input *i,
                               struct t380_cp_output *o, bool allow_completion)
{
    uint64_t elapsed = 0;
    int input_cap = 0, charge_cap = 0;
    const unsigned int required = T380_CP_VALID_TEMP | T380_CP_VALID_VOLTAGE |
        T380_CP_VALID_CURRENT | T380_CP_VALID_AVERAGE | T380_CP_VALID_SOC |
        T380_CP_VALID_CHARGER;
    bool trusted_temp, disconnected;
    const struct age_limits *age;
    int recharge_threshold;

    if (!o)
        return;
    *o = (struct t380_cp_output){ .reason = T380_CP_INVALID_ARGUMENT };
    if (!s)
        return;
    if (!i) {
        publish(s, o, T380_CP_INVALID_ARGUMENT, true);
        return;
    }
    /* Collect restrictions independently before any eligibility early return. */
    trusted_temp = i->telemetry_validated && (i->valid & T380_CP_VALID_TEMP);
    disconnected = i->source_validated && (i->valid & T380_CP_VALID_SOURCE) &&
                   i->source == T380_CP_SOURCE_NONE;
    if (i->telemetry_validated && (i->valid & T380_CP_VALID_AGE) &&
        i->age_step >= 0 && i->age_step <= 4 && i->age_step > s->strictest_age_step)
        s->strictest_age_step = i->age_step;
    if (trusted_temp && i->temp_decic >= -200 && i->temp_decic <= 900)
        observe_thermal_restrictions(s, i->temp_decic);
    if (s->fault_latched) {
        publish(s, o, s->fault_reason, false);
        return;
    }
    if (i->telemetry_validated && (i->valid & T380_CP_VALID_CHARGER) &&
        !i->charger_healthy) {
        publish(s, o, T380_CP_CHARGER_FAULT, true);
        return;
    }
    if (i->telemetry_validated &&
        (invalid_trusted_telemetry(i) ||
         (!disconnected && (i->valid & required) != required))) {
        publish(s, o, T380_CP_INVALID_TELEMETRY, true);
        return;
    }
    if (s->initialized && i->now_ms < s->last_now_ms) {
        publish(s, o, T380_CP_TIME_REGRESSION, true);
        return;
    }
    if (i->sample_ms > i->now_ms ||
        (s->initialized && i->sample_ms < s->last_sample_ms) ||
        (trusted_temp &&
         (i->thermal_sample_ms < i->sample_ms || i->thermal_sample_ms > i->now_ms ||
          (s->have_thermal_sample && i->thermal_sample_ms < s->last_thermal_sample_ms)))) {
        publish(s, o, T380_CP_SAMPLE_REGRESSION, true);
        return;
    }
    if (s->initialized)
        elapsed = i->now_ms - s->last_now_ms;
    if (i->now_ms - i->sample_ms > T380_CP_MAX_SAMPLE_AGE_MS ||
        (s->previous_would_charge && elapsed > T380_CP_MAX_SAMPLE_AGE_MS)) {
        publish(s, o, T380_CP_STALE_TELEMETRY, true);
        return;
    }
    /* Prior hypothetical setpoint accounts for the interval just elapsed. */
    if (s->previous_would_charge && elapsed) {
        /* Preserve stock's two integer divisions, including lost fractions. */
        uint64_t available_ms = s->remaining_budget_ma_ms /
                                s->previous_effective_ma;
        if (elapsed >= available_ms) {
            s->remaining_budget_ma_ms = 0;
            publish(s, o, T380_CP_WEIGHTED_TIMEOUT, true);
            return;
        }
        s->remaining_budget_ma_ms =
            ((available_ms - elapsed) * s->previous_effective_ma /
             T380_CP_REFERENCE_MA) * T380_CP_REFERENCE_MA;
        if (!s->remaining_budget_ma_ms) {
            publish(s, o, T380_CP_WEIGHTED_TIMEOUT, true);
            return;
        }
    }
    s->initialized = true;
    s->last_now_ms = i->now_ms;
    s->last_sample_ms = i->sample_ms;
    if (s->attached &&
        (s->attached_maintenance ?
         (s->episode_open && i->now_ms - s->episode_start_ms >=
             (s->episode_number == 1 ? T380_CP_SESSION_LIMIT_MS :
              T380_CP_RECHARGE_BUDGET_MS)) :
         (i->now_ms - s->session_start_ms >= T380_CP_SESSION_LIMIT_MS ||
          (s->recharging && i->now_ms - s->recharge_start_ms >=
                              T380_CP_RECHARGE_BUDGET_MS)))) {
        publish(s, o, T380_CP_SESSION_TIMEOUT, true);
        return;
    }
    if (trusted_temp && (!s->have_thermal_sample ||
                         i->thermal_sample_ms > s->last_thermal_sample_ms)) {
        recover_thermal(s, i->temp_decic);
        s->have_thermal_sample = true;
        s->last_thermal_sample_ms = i->thermal_sample_ms;
    }
    if (!(i->valid & T380_CP_VALID_SOURCE) || !i->source_validated) {
        publish(s, o, T380_CP_UNVALIDATED_SOURCE, false);
        return;
    }
    if (i->source == T380_CP_SOURCE_NONE) {
        /* A verified unplug starts a new session on the next valid attach. */
        s->attached = false;
        s->phase = T380_CP_FIRST;
        s->recharging = false;
        s->have_full_sample = false;
        if (s->attached_maintenance) {
            /* A detached development attachment is terminal, never a
             * budget reset through a source callback or cable chatter. */
            publish(s, o, T380_CP_NO_SOURCE, true);
            return;
        }
        publish(s, o, T380_CP_NO_SOURCE, false);
        return;
    }
    switch (i->source) {
    case T380_CP_SOURCE_USB: input_cap = 500; charge_cap = 500; break;
    case T380_CP_SOURCE_DCP: input_cap = 900; charge_cap = 1200; break;
    case T380_CP_SOURCE_QUALIFIED_FIXED5V_TIMEOUT:
        /* The supervised caller still supplies its explicit allowance.
         * Existing trial/DCP paths pass 100; only the selected Belkin
         * session may pass 500. This cap alone never grants admission. */
        input_cap = 500; charge_cap = 500; break;
    case T380_CP_SOURCE_OPERATOR900_FIXED5V_TIMEOUT:
        /* New explicit adapter/cable/setup declaration, never a DCP relabel.
         * Battery fast remains500; admission still belongs to the supervised owner. */
        input_cap = 900; charge_cap = 500; break;
    case T380_CP_SOURCE_CDP: input_cap = 1000; charge_cap = 1000; break;
    case T380_CP_SOURCE_MAINS: input_cap = 1800; charge_cap = 2100; break;
    default:
        publish(s, o, T380_CP_UNSUPPORTED_SOURCE, false);
        return;
    }
    if (i->source_voltage_mv != 5000 || i->source_allowance_ma < 100 ||
        i->source_allowance_ma > 2100) {
        publish(s, o, T380_CP_UNSUPPORTED_SOURCE, false);
        return;
    }
    if (!i->telemetry_validated) {
        publish(s, o, T380_CP_UNVALIDATED_TELEMETRY, false);
        return;
    }
    if (!(i->valid & T380_CP_VALID_AGE) || i->age_step < 0 || i->age_step > 4) {
        publish(s, o, T380_CP_UNSUPPORTED_AGE, false);
        return;
    }
    if (!i->normal_charge_region_validated) {
        publish(s, o, T380_CP_UNSUPPORTED_CHARGE_REGION, false);
        return;
    }
    if (i->regular_profile && (!s->household_mode ||
        !i->regular_model_matched || i->age_step != 0 ||
        s->strictest_age_step != 0 ||
        i->source != T380_CP_SOURCE_OPERATOR900_FIXED5V_TIMEOUT ||
        i->source_allowance_ma != 900 || i->reported_capacity < 0 ||
        i->reported_capacity > 100)) {
        publish(s, o, T380_CP_UNSUPPORTED_CHARGE_REGION, false);
        return;
    }
    age = &ages[s->strictest_age_step];
    if (!s->attached) {
        s->attached = true;
        s->session_start_ms = i->now_ms;
        s->remaining_budget_ma_ms =
            T380_CP_INITIAL_BUDGET_MS * T380_CP_REFERENCE_MA;
        if (s->attached_maintenance) {
            s->episode_open = true;
            s->episode_number = 1;
            s->episode_start_ms = i->now_ms;
        }
    }
    if (s->thermal != T380_CP_THERMAL_NORMAL) {
        publish(s, o, T380_CP_THERMAL_BLOCK, false);
        return;
    }
    if (s->swelling_blocked) {
        publish(s, o, T380_CP_SWELLING_BLOCK, false);
        return;
    }
    if (i->source_suspended || i->system_suspended) {
        publish(s, o, i->system_suspended ? T380_CP_SYSTEM_SUSPENDED :
                                         T380_CP_SOURCE_SUSPENDED, false);
        return;
    }
    if (s->phase == T380_CP_FULL) {
        if (s->attached_maintenance &&
            (s->episode_open || s->full_pending || s->recharge_pending)) {
            publish(s, o, T380_CP_FULL_STOP, false);
            return;
        }
        recharge_threshold = age->recharge;
        if (s->have_full_sample && i->sample_ms <= s->last_full_sample_ms) {
            publish(s, o, T380_CP_FULL_STOP, false);
            return;
        }
        if (s->low_temp_derated)
            recharge_threshold = lesser(recharge_threshold, age->cv - 150);
        if (i->voltage_mv > recharge_threshold) {
            publish(s, o, T380_CP_FULL_STOP, false);
            return;
        }
        s->phase = T380_CP_FIRST;
        s->recharging = true;
        s->recharge_start_ms = i->now_ms;
        if (s->attached_maintenance) {
            s->episode_start_ms = i->now_ms;
            s->recharge_pending = true;
        }
        s->remaining_budget_ma_ms =
            T380_CP_RECHARGE_BUDGET_MS * T380_CP_REFERENCE_MA;
        /* Do not count the recharge-triggering sample as full too. */
        s->have_full_sample = true;
        s->last_full_sample_ms = i->sample_ms;
    } else if (allow_completion &&
               (!s->have_full_sample || i->sample_ms > s->last_full_sample_ms) &&
               completion_voltage_soc(s, i, age) &&
               i->current_ma > 0 && i->current_ma < 350 &&
               i->average_current_ma > 0 &&
               i->average_current_ma < (s->phase == T380_CP_FIRST ? 350 : 250)) {
        s->have_full_sample = true;
        s->last_full_sample_ms = i->sample_ms;
        if (s->phase == T380_CP_FIRST)
            s->phase = T380_CP_SECOND;
        else {
            s->phase = T380_CP_FULL;
            if (s->attached_maintenance) s->full_pending = true;
            publish(s, o, T380_CP_FULL_STOP, false);
            return;
        }
    }
    if (i->voltage_mv > age->cv) {
        publish(s, o, T380_CP_ABOVE_FLOAT_VOLTAGE, false);
        return;
    }
    input_cap = lesser(input_cap, i->source_allowance_ma);
    /* Floor to representable 50mA steps, never round a limit upward. */
    input_cap = 100 + ((input_cap - 100) / 50) * 50;
    charge_cap = lesser(charge_cap, i->source_allowance_ma);
    if (s->low_temp_derated)
        charge_cap = lesser(charge_cap, 1050);
    charge_cap = 100 + ((charge_cap - 100) / 50) * 50;
    o->would_charge = true;
    o->input_limit_ma = input_cap;
    o->charge_limit_ma = charge_cap;
    o->float_voltage_mv = age->cv;
    /* Conservative second-stage termination setting; scoped completion above. */
    o->topoff_ma = s->phase == T380_CP_FIRST ? 350 : 250;
    s->previous_would_charge = true;
    s->previous_effective_ma = effective_current(input_cap, charge_cap, age->cv);
    s->applied_input_ma = input_cap;
    s->applied_charge_ma = charge_cap;
    s->applied_float_mv = age->cv;
    publish(s, o, T380_CP_WOULD_CHARGE, false);
}

void t380_charge_policy_step(struct t380_cp_state *s,
                            const struct t380_cp_input *i,
                            struct t380_cp_output *o)
{
    charge_policy_step(s, i, o, true);
}

void t380_charge_policy_step_without_completion(struct t380_cp_state *s,
                            const struct t380_cp_input *i,
                            struct t380_cp_output *o)
{
    charge_policy_step(s, i, o, false);
}

const char *t380_charge_policy_reason(enum t380_cp_reason r)
{
    switch (r) {
#define R(x) case x: return #x
    R(T380_CP_WOULD_CHARGE); R(T380_CP_NO_SOURCE);
    R(T380_CP_UNVALIDATED_SOURCE); R(T380_CP_UNSUPPORTED_SOURCE);
    R(T380_CP_UNVALIDATED_TELEMETRY); R(T380_CP_UNSUPPORTED_AGE);
    R(T380_CP_UNSUPPORTED_CHARGE_REGION); R(T380_CP_ABOVE_FLOAT_VOLTAGE);
    R(T380_CP_SOURCE_SUSPENDED); R(T380_CP_SYSTEM_SUSPENDED);
    R(T380_CP_THERMAL_BLOCK); R(T380_CP_SWELLING_BLOCK); R(T380_CP_FULL_STOP);
    R(T380_CP_INVALID_TELEMETRY); R(T380_CP_STALE_TELEMETRY);
    R(T380_CP_TIME_REGRESSION); R(T380_CP_SAMPLE_REGRESSION);
    R(T380_CP_CHARGER_FAULT); R(T380_CP_WEIGHTED_TIMEOUT);
    R(T380_CP_SESSION_TIMEOUT); R(T380_CP_INVALID_ARGUMENT);
    R(T380_CP_HARDWARE_DONE_INCOMPLETE);
    R(T380_CP_HARDWARE_STATE_MISMATCH);
#undef R
    default: return "T380_CP_UNKNOWN_REASON";
    }
}
