/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_SOURCE_LIFECYCLE_POLICY_H
#define T380_SOURCE_LIFECYCLE_POLICY_H
#include "../source-lifecycle/t380_source_lifecycle.h"
#include "t380_charge_policy.h"

/* Reuse the current host policy ABI without converting an observed callback
 * request into a negotiated allowance or inventing a measured5V source. */
static inline void t380_sl_policy_inputs(const struct t380_sl_snapshot *source,
                                         struct t380_cp_input *input)
{
    input->valid &= ~(unsigned int)T380_CP_VALID_SOURCE;
    input->telemetry_validated = false;
    input->normal_charge_region_validated = false;
    input->source_validated = false;
    input->source = source->fixed5v_qualified ?
        T380_CP_SOURCE_QUALIFIED_FIXED5V_TIMEOUT :
        source->sdp_candidate ? T380_CP_SOURCE_USB : T380_CP_SOURCE_UNKNOWN;
    input->source_voltage_mv = 0;
    input->source_allowance_ma = 0;
    input->source_suspended = source->bus_suspended && !source->fixed5v_qualified;
    input->system_suspended = source->system_suspended;
}
#endif
