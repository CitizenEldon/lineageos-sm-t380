/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_SOURCE_LIFECYCLE_H
#define T380_SOURCE_LIFECYCLE_H
#ifdef __KERNEL__
#include <linux/types.h>
typedef u64 t380_sl_u64;
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t t380_sl_u64;
#endif

/* Software acquisition age only; matches the existing policy's bound. */
#define T380_SL_MAX_AGE_MS 30000ULL
enum t380_sl_reason {
    T380_SL_PENDING, T380_SL_SDP_CANDIDATE, T380_SL_OBSERVATION_ERROR,
    T380_SL_ABSENT, T380_SL_CONTRADICTORY, T380_SL_UNKNOWN_PATTERN,
    T380_SL_REQUEST_INVALID, T380_SL_BUS_SUSPENDED, T380_SL_SYSTEM_SUSPENDED,
    T380_SL_RESUME_REVALIDATION, T380_SL_DISCONNECTED, T380_SL_STALE,
    T380_SL_TIME_FAULT, T380_SL_GENERATION_EXHAUSTED, T380_SL_DCP_CANDIDATE,
    T380_SL_FIXED5V_CANDIDATE, T380_SL_HOST_SETUP, T380_SL_DETECTION_PENDING
};
struct t380_sl_sample {
    bool valid, present, acceptable;
    t380_sl_u64 sample_ms, generation;
};
struct t380_sl_state {
    t380_sl_u64 generation, invalidated_ms, last_event_ms, last_bad_ms;
    t380_sl_u64 fault_serial, pm_serial;
    t380_sl_u64 request_ms, request_generation, discarded;
    struct t380_sl_sample charger, muic;
    unsigned int requested_ma;
    unsigned int fixed5v_input_allowance_ma;
    bool request_valid, bus_suspended, system_suspended;
    bool charger_bad, muic_bad, muic_dcp, muic_fixed5v, dcp_callback_blocked;
    bool clean_absence, host_blocked, fixed5v_qualified;
    t380_sl_u64 fixed5v_generation;
    /* Disconnect ambiguity lives until fresh dual physical absence;
     * last_invalidation remains diagnostic event history. */
    bool disconnect_pending;
    /* OFF-only exact settling acquisition; never source qualification.
     * One attempt per completed dual-absence event, same age bound. */
    bool detection_pending, detection_used, absence_provenance;
    t380_sl_u64 detection_deadline_ms, absence_ms, absence_generation;
    t380_sl_u64 absence_fault_serial, absence_pm_serial;
    bool time_fault, exhausted;
    enum t380_sl_reason last_invalidation;
};
/* Observability only; never a qualification or permission token. */
struct t380_sl_epoch_record {
 t380_sl_u64 generation, pm_serial, fault_serial, invalidated_ms, sampled_ms;
 int last_invalidation;
 bool system_suspended, bus_suspended;
};
struct t380_sl_snapshot {
    t380_sl_u64 invalidated_ms;
    enum t380_sl_reason last_invalidation;
    t380_sl_u64 generation, now_ms, oldest_ms, request_ms, discarded;
    t380_sl_u64 absence_oldest_ms;
    t380_sl_u64 fault_serial, pm_serial;
    unsigned int last_requested_ma, candidate_request_ma;
    unsigned int fixed5v_input_allowance_ma; /* Explicit contract, same generation. */
    bool sdp_candidate, dcp_candidate, fixed5v_candidate, fixed5v_qualified;
    bool clean_absence, host_blocked, bus_suspended, system_suspended;
    /* Same-generation physical absence; never a cached old absence bit. */
    bool absence_pending, fresh_absence, ambiguous_disconnect;
    enum t380_sl_reason reason;
    /* These are permanently false in this diagnostic adapter. */
    bool source_validated, telemetry_validated, normal_charge_region_validated;
};

/* Caller serializes ALL access. Kernel glue uses one IRQ-safe spinlock, and
 * never holds it during I2C, guard checks, sysfs formatting or sleeping calls. */
void t380_sl_init(struct t380_sl_state *s);
void t380_sl_usb(struct t380_sl_state *s, t380_sl_u64 now_ms, unsigned int ma);
void t380_sl_bus_suspend(struct t380_sl_state *s, t380_sl_u64 now_ms, bool value);
void t380_sl_system_suspend(struct t380_sl_state *s, t380_sl_u64 now_ms, bool value);
void t380_sl_disconnect(struct t380_sl_state *s, t380_sl_u64 now_ms);
void t380_sl_host_setup(struct t380_sl_state *s, t380_sl_u64 now_ms);
bool t380_sl_qualify_fixed5v(struct t380_sl_state *s, t380_sl_u64 now_ms);
/* Only the explicitly selected operator900 contract may call this API. */
bool t380_sl_qualify_operator900(struct t380_sl_state *s, t380_sl_u64 now_ms);
void t380_sl_charger(struct t380_sl_state *s, t380_sl_u64 ticket,
                     t380_sl_u64 sample_ms, t380_sl_u64 ended_ms,
                     t380_sl_u64 now_ms, bool valid, bool present, bool acceptable);
void t380_sl_muic(struct t380_sl_state *s, t380_sl_u64 ticket,
                  t380_sl_u64 sample_ms, t380_sl_u64 ended_ms,
                  t380_sl_u64 now_ms, bool valid, bool present, bool sdp_pattern);
void t380_sl_muic_class(struct t380_sl_state *s, t380_sl_u64 ticket,
                  t380_sl_u64 sample_ms, t380_sl_u64 ended_ms,
                  t380_sl_u64 now_ms, bool valid, bool present,
                  bool sdp_pattern, bool dcp_pattern, bool fixed5v_pattern);
bool t380_sl_detection_pending(struct t380_sl_state *s, t380_sl_u64 ticket,
                  t380_sl_u64 start, t380_sl_u64 ended, t380_sl_u64 now_ms);
void t380_sl_copy(struct t380_sl_state *s, t380_sl_u64 now_ms,
                  struct t380_sl_snapshot *out);
#endif
