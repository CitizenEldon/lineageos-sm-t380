/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_POWER_ACQUISITION_POLICY_H
#define T380_POWER_ACQUISITION_POLICY_H
#include "../power027-acquisition/t380_power_acquisition.h"
#include "t380_charge_policy.h"

/* Host-model adapter. A selected conservative envelope is not measured age.
 * This narrow helper sets no source/charger/region/provenance assertion. */
static inline void t380_pa_policy_inputs(const struct t380_pa_tuple *o,
                                         struct t380_cp_input *input)
{
    const struct t380_pa_profile profile = t380_pa_conservative_profile();
    input->valid &= ~(unsigned int)(T380_CP_VALID_TEMP | T380_CP_VALID_VOLTAGE |
        T380_CP_VALID_CURRENT | T380_CP_VALID_SOC | T380_CP_VALID_AVERAGE);
    input->telemetry_validated = false;
    input->normal_charge_region_validated = false;
    input->source_validated = false;
    input->source_allowance_ma = input->source_voltage_mv = 0;
    input->valid &= ~(unsigned int)(T380_CP_VALID_SOURCE | T380_CP_VALID_CHARGER);
    input->age_step = profile.selected_row;
    input->valid |= T380_CP_VALID_AGE; /* Deliberately selected conservative bound. */
    input->now_ms = o->ended_ms;
    input->sample_ms = o->started_ms;
    input->thermal_sample_ms = o->field[T380_PA_TEMP].started_ms;
    input->temp_decic = o->field[T380_PA_TEMP].value;
    input->voltage_mv = o->field[T380_PA_VOLTAGE].value / 1000;
    input->current_ma = o->field[T380_PA_CURRENT].value / 1000;
    input->soc_percent = o->field[T380_PA_SOC].value;
    input->average_current_ma = o->average_ma;
    if (o->tuple_valid) {
        input->valid |= T380_CP_VALID_TEMP | T380_CP_VALID_VOLTAGE |
                        T380_CP_VALID_CURRENT | T380_CP_VALID_SOC;
        if (o->history_ready) input->valid |= T380_CP_VALID_AVERAGE;
    }
}
#endif
