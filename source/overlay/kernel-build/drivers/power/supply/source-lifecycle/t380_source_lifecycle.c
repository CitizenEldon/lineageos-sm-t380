/* SPDX-License-Identifier: GPL-2.0-only */
#include "t380_source_lifecycle.h"

static void sl_revoke(struct t380_sl_state *s, t380_sl_u64 now,
                       enum t380_sl_reason reason)
{
    s->request_valid = false;
    s->fixed5v_qualified = false;
    s->fixed5v_input_allowance_ma = 0;
    s->detection_pending = false;
    s->absence_provenance = false;
    s->invalidated_ms = now;
    s->last_invalidation = reason;
    /* Gadget disconnect is ambiguous on physical unplug. It never proves
     * absence; the bounded HIGH wait still requires both hardware sensors.
     * Host SETUP, read/unknown/time/PM errors remain sticky across absence. */
    if (reason != T380_SL_ABSENT && reason != T380_SL_DISCONNECTED &&
        s->fault_serial != (t380_sl_u64)~(t380_sl_u64)0)
        ++s->fault_serial;
    if (s->generation == (t380_sl_u64)~(t380_sl_u64)0) {
        s->exhausted = true;
        return;
    }
    ++s->generation;
}

static bool sl_time(struct t380_sl_state *s, t380_sl_u64 now)
{
    if (now < s->last_event_ms) {
        s->time_fault = true;
        sl_revoke(s, s->last_event_ms, T380_SL_TIME_FAULT);
    } else {
        s->last_event_ms = now;
    }
    if (s->detection_pending && now >= s->detection_deadline_ms) {
        /* Deadline never extends on repeated pending or qualification reads. */
        sl_revoke(s, now, T380_SL_UNKNOWN_PATTERN);
        s->muic.valid = false;
    }
    return !s->time_fault && !s->exhausted;
}

void t380_sl_init(struct t380_sl_state *s)
{
    struct t380_sl_state empty = { 0 };
    *s = empty;
    s->generation = 1;
    s->charger_bad = s->muic_bad = true;
    s->host_blocked = true;
    s->last_invalidation = T380_SL_PENDING;
}

void t380_sl_usb(struct t380_sl_state *s, t380_sl_u64 now, unsigned int ma)
{
    if (!sl_time(s, now))
        return;
    /* A callback inside OFF-only settling is a new USB event, not inherited
     * physical-absence provenance. Preserve ordinary callbacks outside it. */
    if (s->fixed5v_qualified || s->detection_pending)
        sl_revoke(s, now, T380_SL_REQUEST_INVALID);
    /* A gadget callback during a DCP epoch is contradictory. Do not let
     * later physical samples revive DCP without a real detach boundary. */
    if (s->muic_dcp || ma < 100) s->dcp_callback_blocked = true;
    s->requested_ma = ma;
    s->request_ms = now;
    /* First adapter supports only the observed SDP route. USB policy row
     * caps500mA; neither this callback nor its range proves an allowance. */
    if (ma < 100 || ma > 500 || s->bus_suspended || s->system_suspended) {
        sl_revoke(s, now, T380_SL_REQUEST_INVALID);
        return;
    }
    s->request_generation = s->generation;
    s->request_valid = true;
}

void t380_sl_bus_suspend(struct t380_sl_state *s, t380_sl_u64 now, bool value)
{
    if (!sl_time(s, now))
        return;
    if (s->bus_suspended != value) {
        s->bus_suspended = value;
        if (value) s->dcp_callback_blocked = true;
        sl_revoke(s, now, value ? T380_SL_BUS_SUSPENDED : T380_SL_RESUME_REVALIDATION);
    }
}

void t380_sl_system_suspend(struct t380_sl_state *s, t380_sl_u64 now, bool value)
{
    if (!sl_time(s, now))
        return;
    if (s->system_suspended != value) {
        s->system_suspended = value;
        if (value) {
            /* Wake cannot inherit pre-sleep absence or a hostless ticket.
             * A detached household episode needs new dual physical absence
             * before any later exact-source qualification. */
            s->clean_absence = false;
            s->host_blocked = true;
            s->charger.valid = s->muic.valid = false;
        }
        if (s->pm_serial != (t380_sl_u64)~(t380_sl_u64)0)
            ++s->pm_serial;
        sl_revoke(s, now, value ? T380_SL_SYSTEM_SUSPENDED : T380_SL_RESUME_REVALIDATION);
    }
}

void t380_sl_disconnect(struct t380_sl_state *s, t380_sl_u64 now)
{
    if (!sl_time(s, now))
        return;
    sl_revoke(s, now, T380_SL_DISCONNECTED);
    s->disconnect_pending = true;
    s->charger.valid = s->muic.valid = false;
    s->dcp_callback_blocked = false;
}

void t380_sl_host_setup(struct t380_sl_state *s, t380_sl_u64 now)
{
    bool keep_sdp_request;

    if (!sl_time(s, now)) return;
    /* EP0 activity vetoes the hostless profile, but it is normal for a PC
     * to send more control requests after its one-shot power callback.
     * Preserve only a still-valid SDP-range callback; physical badness,
     * suspend and non-SDP classes continue to revoke it. */
    keep_sdp_request = s->request_valid && s->requested_ma >= 100 &&
        s->requested_ma <= 500 && s->request_ms > s->last_bad_ms &&
        !s->muic_dcp && !s->muic_fixed5v &&
        !s->bus_suspended && !s->system_suspended &&
        s->charger.present && s->muic.present &&
        s->charger.acceptable && s->muic.acceptable &&
        s->charger.sample_ms && s->muic.sample_ms &&
        now >= s->charger.sample_ms && now >= s->muic.sample_ms &&
        now - s->charger.sample_ms <= T380_SL_MAX_AGE_MS &&
        now - s->muic.sample_ms <= T380_SL_MAX_AGE_MS;
    s->host_blocked = true;
    s->clean_absence = false;
    s->charger.valid = s->muic.valid = false;
    sl_revoke(s, now, T380_SL_HOST_SETUP);
    if (keep_sdp_request && !s->exhausted) {
        s->request_valid = true;
        s->request_generation = s->generation;
    }
}

static bool sl_qualify_fixed5v_input(struct t380_sl_state *s, t380_sl_u64 now,
                                      unsigned int input_ma)
{
    struct t380_sl_snapshot source;
    t380_sl_copy(s, now, &source);
    if (!source.fixed5v_candidate || source.now_ms < source.oldest_ms ||
        source.now_ms - source.oldest_ms > 2000ULL)
        return false;
    s->fixed5v_qualified = true;
    s->fixed5v_generation = s->generation;
    s->fixed5v_input_allowance_ma = input_ma;
    return true;
}

/* Check prior samples before publication can overwrite the stale evidence.
 * Clear both caches once, so concurrent old-ticket publishers cannot repeatedly
 * advance the epoch or replenish it. Exactly30000ms remains acceptable. */
static bool sl_expire_physical(struct t380_sl_state *s, t380_sl_u64 now)
{
    if ((s->charger.valid && now - s->charger.sample_ms > T380_SL_MAX_AGE_MS) ||
        (s->muic.valid && now - s->muic.sample_ms > T380_SL_MAX_AGE_MS)) {
        sl_revoke(s, now, T380_SL_STALE);
        s->charger.valid = s->muic.valid = false;
        return true;
    }
    return false;
}

static void sl_observe(struct t380_sl_state *s, struct t380_sl_sample *sample,
                       bool *was_bad, t380_sl_u64 ticket, t380_sl_u64 start,
                       t380_sl_u64 ended, t380_sl_u64 now,
                       bool valid, bool present, bool acceptable)
{
    bool bad = !valid || !present || !acceptable;

    if (!sl_time(s, now))
        return;
    if (!valid || start > ended || ended > now ||
        /* Equal starts cannot order concurrent reads of the same sensor. */
        (sample->valid && start <= sample->sample_ms)) {
        sample->valid = false;
        *was_bad = true;
        s->last_bad_ms = now;
        sl_revoke(s, now, T380_SL_OBSERVATION_ERROR);
        return;
    }
    sl_expire_physical(s, now);
    if (ticket != s->generation) {
        if (s->discarded != (t380_sl_u64)~(t380_sl_u64)0)
            ++s->discarded;
        return; /* A completed pre-invalidation read cannot enter a new epoch. */
    }
    if (bad) {
        s->last_bad_ms = now;
        /* Repeat absence observations still block, but do not repeatedly
         * erase a pending new callback during the mixed return transition. */
        if (!*was_bad)
            sl_revoke(s, now, present ? T380_SL_UNKNOWN_PATTERN : T380_SL_ABSENT);
        else if (present && !acceptable &&
                 s->fault_serial != (t380_sl_u64)~(t380_sl_u64)0)
            /* An unknown present source after absence is a new household
             * fault even though this sensor was already in the bad state. */
            ++s->fault_serial;
        if (!present) s->dcp_callback_blocked = false;
    }
    *was_bad = bad;
    sample->valid = true;
    sample->present = present;
    sample->acceptable = acceptable;
    sample->sample_ms = start;
    sample->generation = s->generation;
    /* Only both fresh physical sensors reporting absence establish a clean
     * next attachment. Generic software disconnect and reset never do. */
    if (s->charger.valid && s->muic.valid &&
        !s->charger.present && !s->muic.present &&
        now >= s->charger.sample_ms && now >= s->muic.sample_ms &&
        now - s->charger.sample_ms <= T380_SL_MAX_AGE_MS &&
        now - s->muic.sample_ms <= T380_SL_MAX_AGE_MS) {
        s->host_blocked = false;
        s->clean_absence = true;
        /* Repeated absence within the same event cannot renew a used attempt. */
        if (!s->absence_provenance && !s->system_suspended &&
            s->charger.generation == s->generation &&
            s->muic.generation == s->generation &&
            s->charger.sample_ms >= s->invalidated_ms &&
            s->muic.sample_ms >= s->invalidated_ms) {
            s->detection_used = false;
            s->absence_provenance = true;
            s->absence_generation = s->generation;
            s->absence_fault_serial = s->fault_serial;
            s->absence_pm_serial = s->pm_serial;
        }
        s->absence_ms = s->charger.sample_ms < s->muic.sample_ms ?
            s->charger.sample_ms : s->muic.sample_ms;
    }
    /* The settling interval spans subsequent ordinary qualification reads.
     * Neither physical acceptability nor this record grants authority. */
}

bool t380_sl_detection_pending(struct t380_sl_state *s, t380_sl_u64 ticket,
        t380_sl_u64 start, t380_sl_u64 ended, t380_sl_u64 now)
{
    /* A bus-suspended level inherited through fresh dual absence is not a
     * new event or an acceptable source class. Any later bus edge revokes
     * generation/fault provenance below; host and system-PM vetoes remain. */
    if (!sl_time(s, now) || start > ended || ended > now ||
        !start || ticket != s->generation || !s->absence_provenance ||
        s->absence_generation != s->generation ||
        s->fault_serial != s->absence_fault_serial ||
        s->pm_serial != s->absence_pm_serial || s->host_blocked ||
        s->system_suspended ||
        !s->clean_absence || now < s->absence_ms ||
        now - s->absence_ms > T380_SL_MAX_AGE_MS ||
        (s->muic.valid && start <= s->muic.sample_ms) ||
        !s->charger.valid || !s->charger.present || !s->charger.acceptable ||
        s->charger.generation != s->generation ||
        now < s->charger.sample_ms ||
        now - s->charger.sample_ms > 2000ULL)
        return false;
    if (!s->detection_pending) {
        if (!s->muic.valid || s->muic.present ||
            s->muic.generation != s->generation) return false;
        if (s->detection_used || now > (t380_sl_u64)~(t380_sl_u64)0 -
            T380_SL_MAX_AGE_MS) return false;
        s->detection_deadline_ms = now + T380_SL_MAX_AGE_MS;
        s->detection_used = true;
        s->detection_pending = true;
    }
    s->fixed5v_qualified = false;
    s->fixed5v_input_allowance_ma = 0;
    s->request_valid = false;
    s->muic = (struct t380_sl_sample) { true, true, false, start, s->generation };
    s->muic_bad = true;
    s->muic_dcp = s->muic_fixed5v = false;
    return true;
}

bool t380_sl_qualify_fixed5v(struct t380_sl_state *s, t380_sl_u64 now)
{ return sl_qualify_fixed5v_input(s, now, 500); }
bool t380_sl_qualify_operator900(struct t380_sl_state *s, t380_sl_u64 now)
{ return sl_qualify_fixed5v_input(s, now, 900); }

void t380_sl_charger(struct t380_sl_state *s, t380_sl_u64 ticket,
                     t380_sl_u64 start, t380_sl_u64 ended, t380_sl_u64 now,
                     bool valid, bool present, bool acceptable)
{
    sl_observe(s, &s->charger, &s->charger_bad, ticket, start, ended, now,
               valid, present, acceptable);
}

void t380_sl_muic(struct t380_sl_state *s, t380_sl_u64 ticket,
                  t380_sl_u64 start, t380_sl_u64 ended, t380_sl_u64 now,
                  bool valid, bool present, bool sdp_pattern)
{
    t380_sl_muic_class(s, ticket, start, ended, now, valid, present,
                       sdp_pattern, false, false);
}

void t380_sl_muic_class(struct t380_sl_state *s, t380_sl_u64 ticket,
                  t380_sl_u64 start, t380_sl_u64 ended, t380_sl_u64 now,
                  bool valid, bool present, bool sdp_pattern, bool dcp_pattern,
                  bool fixed5v_pattern)
{
#ifndef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
    fixed5v_pattern = false;
#endif
    if ((int)sdp_pattern + (int)dcp_pattern + (int)fixed5v_pattern > 1) {
        sl_observe(s, &s->muic, &s->muic_bad, ticket, start, ended, now,
                   false, present, false);
        return;
    }
    /* Settling never authorizes a different class. Only the ordinary exact
     * fixed5V classifier may end this narrow interval without a real fault. */
    if (s->detection_pending && (!valid || !present || !fixed5v_pattern)) {
        if (sl_time(s, now)) sl_revoke(s, now, T380_SL_UNKNOWN_PATTERN);
        s->muic.valid = false;
    }
    if (valid && present && s->muic.valid && s->muic.acceptable &&
        (s->muic_dcp != dcp_pattern ||
         s->muic_fixed5v != fixed5v_pattern)) {
        if (sl_time(s, now)) sl_revoke(s, now, T380_SL_UNKNOWN_PATTERN);
        s->muic.valid = false;
        s->muic_dcp = dcp_pattern;
        s->muic_fixed5v = fixed5v_pattern;
    }
    sl_observe(s, &s->muic, &s->muic_bad, ticket, start, ended, now,
               valid, present, sdp_pattern || dcp_pattern || fixed5v_pattern);
    if (s->detection_pending && s->muic.valid && s->muic.acceptable &&
        s->charger.valid && s->charger.present && s->charger.acceptable &&
        s->charger.generation == s->generation &&
        s->muic.generation == s->generation) {
        s->detection_pending = false;
        s->absence_provenance = false;
    }
    if (s->muic.valid && s->muic.present && s->muic.acceptable)
        s->absence_provenance = false;
    if (s->muic.valid && s->muic.generation == s->generation) {
        s->muic_dcp = dcp_pattern;
        s->muic_fixed5v = fixed5v_pattern;
    }
}

void t380_sl_copy(struct t380_sl_state *s, t380_sl_u64 now,
                  struct t380_sl_snapshot *out)
{
    struct t380_sl_snapshot o = { 0 };
    enum t380_sl_reason reason = T380_SL_PENDING;
    t380_sl_u64 oldest = 0;
    bool expired = false;

    if (sl_time(s, now))
        expired = sl_expire_physical(s, now);
    if (s->exhausted)
        reason = T380_SL_GENERATION_EXHAUSTED;
    else if (s->time_fault)
        reason = T380_SL_TIME_FAULT;
    else if (s->system_suspended)
        reason = T380_SL_SYSTEM_SUSPENDED;
    else if (s->bus_suspended && !s->muic_fixed5v)
        reason = T380_SL_BUS_SUSPENDED;
    else if (expired)
        reason = T380_SL_STALE;
    else if (s->detection_pending)
        reason = T380_SL_DETECTION_PENDING;
    else if (!s->charger.valid || !s->muic.valid)
        reason = T380_SL_OBSERVATION_ERROR;
    else if (s->charger.present != s->muic.present)
        reason = T380_SL_CONTRADICTORY;
    else if (!s->charger.present)
        reason = T380_SL_ABSENT;
    else if (!s->charger.acceptable || !s->muic.acceptable)
        reason = T380_SL_UNKNOWN_PATTERN;
    else if (s->charger.generation != s->generation ||
             s->muic.generation != s->generation ||
             /* A tied millisecond cannot prove request-after-bad ordering. */
             s->charger.sample_ms < s->invalidated_ms ||
             s->muic.sample_ms < s->invalidated_ms)
        reason = T380_SL_REQUEST_INVALID;
    else if (s->muic_fixed5v) {
        /* Never fall through to SDP, even if an old request exists. */
        if (s->clean_absence && !s->host_blocked) {
            reason = T380_SL_FIXED5V_CANDIDATE;
            oldest = s->charger.sample_ms < s->muic.sample_ms ?
                     s->charger.sample_ms : s->muic.sample_ms;
            o.fixed5v_candidate = true;
            o.fixed5v_qualified = s->fixed5v_qualified &&
                                  s->fixed5v_generation == s->generation;
            if (o.fixed5v_qualified)
                o.fixed5v_input_allowance_ma = s->fixed5v_input_allowance_ma;
        } else {
            reason = T380_SL_REQUEST_INVALID;
        }
    }
    else if (s->muic_dcp && !s->request_valid && !s->dcp_callback_blocked) {
        reason = T380_SL_DCP_CANDIDATE;
        oldest = s->charger.sample_ms < s->muic.sample_ms ?
                 s->charger.sample_ms : s->muic.sample_ms;
        o.dcp_candidate = true;
    }
    else if (s->muic_dcp)
        reason = T380_SL_REQUEST_INVALID;
    else if (!s->request_valid || s->request_generation != s->generation ||
             s->request_ms <= s->last_bad_ms)
        reason = T380_SL_REQUEST_INVALID;
    else {
        reason = T380_SL_SDP_CANDIDATE;
        oldest = s->charger.sample_ms < s->muic.sample_ms ?
                 s->charger.sample_ms : s->muic.sample_ms;
        /* Configuration callbacks are events, not sensor heartbeats.
         * Fresh physical observations and the observed epoch gate this record. */
        o.sdp_candidate = true;
        o.candidate_request_ma = s->requested_ma;
    }
    o.reason = reason;
    o.generation = s->generation;
    o.now_ms = now;
    o.oldest_ms = oldest;
    o.request_ms = s->request_ms;
    o.last_requested_ma = s->requested_ma;
    o.discarded = s->discarded;
    o.fault_serial = s->fault_serial;
    o.invalidated_ms = s->invalidated_ms;
    o.last_invalidation = s->last_invalidation;
    o.pm_serial = s->pm_serial;
    o.bus_suspended = s->bus_suspended;
    o.system_suspended = s->system_suspended;
    o.host_blocked = s->host_blocked;
    o.clean_absence = s->clean_absence;
    o.absence_pending = s->last_invalidation == T380_SL_ABSENT &&
        ((s->charger.valid && !s->charger.present &&
          s->charger.generation == s->generation &&
          now >= s->charger.sample_ms &&
          now - s->charger.sample_ms <= 2000) ||
         (s->muic.valid && !s->muic.present &&
          s->muic.generation == s->generation &&
          now >= s->muic.sample_ms &&
          now - s->muic.sample_ms <= 2000));
    /* Physical absence is independent of USB bus policy reason precedence.
     * Bus suspend grants no source or charging permission, but cannot hide a
     * wholly fresh dual-sensor absence proof while the system remains awake. */
    o.fresh_absence = !s->time_fault && !s->exhausted &&
        !s->system_suspended && !expired &&
        s->clean_absence && !s->host_blocked &&
        s->charger.valid && s->muic.valid &&
        !s->charger.present && !s->muic.present &&
        s->charger.generation == s->generation &&
        s->muic.generation == s->generation &&
        s->charger.sample_ms >= s->invalidated_ms &&
        s->muic.sample_ms >= s->invalidated_ms &&
        now >= s->charger.sample_ms && now >= s->muic.sample_ms &&
        now - s->charger.sample_ms <= 2000 &&
        now - s->muic.sample_ms <= 2000;
    if (o.fresh_absence) {
        /* Only the existing current-epoch, post-invalidation dual
         * absence proof resolves this disconnect. Present samples or
         * qualification alone cannot retire an ambiguous event. */
        s->disconnect_pending = false;
        o.absence_oldest_ms = s->charger.sample_ms < s->muic.sample_ms ?
            s->charger.sample_ms : s->muic.sample_ms;
    }
    o.ambiguous_disconnect = s->disconnect_pending &&
        s->last_invalidation == T380_SL_DISCONNECTED;
    *out = o; /* No externally validated fact or current permission is set. */
}
