/* SPDX-License-Identifier: GPL-2.0-only */
/* Included once in existing diagnostic power owner, before observation glue. */
#ifndef T380_SOURCE_LIFECYCLE_KERNEL_H
#define T380_SOURCE_LIFECYCLE_KERNEL_H
#include <linux/math64.h>
#include <linux/spinlock.h>
#include <linux/timekeeping.h>
#include "t380_source_lifecycle.h"
#include "t380_source_lifecycle.c"

static DEFINE_SPINLOCK(t380_source_lock);
/* USB configuration survives ordinary control traffic and system sleep;
 * detach resets it. Bus suspend still selects stock USB100mA. */
static unsigned int t380_stock_usb_ma;
static struct t380_sl_state t380_source_state = {
    .generation = 1, .charger_bad = true, .muic_bad = true,
    .host_blocked = true,
};
static u64 t380_source_now(void)
{
    return div_u64(ktime_get_boottime_ns(), 1000000);
}
static u64 t380_source_ticket(void)
{
    unsigned long flags;
    u64 generation;
    spin_lock_irqsave(&t380_source_lock, flags);
    generation = t380_source_state.generation;
    spin_unlock_irqrestore(&t380_source_lock, flags);
    return generation;
}
void t380_source_usb_request(unsigned int ma)
{
    unsigned long flags;
    spin_lock_irqsave(&t380_source_lock, flags);
    t380_stock_usb_ma=ma>=100 && ma<=500?ma:0;
    t380_sl_usb(&t380_source_state, t380_source_now(), ma);
    spin_unlock_irqrestore(&t380_source_lock, flags);
}
EXPORT_SYMBOL_GPL(t380_source_usb_request);
void t380_source_usb_suspend(bool value)
{
    unsigned long flags;
    spin_lock_irqsave(&t380_source_lock, flags);
    t380_sl_bus_suspend(&t380_source_state, t380_source_now(), value);
    spin_unlock_irqrestore(&t380_source_lock, flags);
}
EXPORT_SYMBOL_GPL(t380_source_usb_suspend);
void t380_source_system_suspend(bool value)
{
    unsigned long flags;
    spin_lock_irqsave(&t380_source_lock, flags);
    t380_sl_system_suspend(&t380_source_state, t380_source_now(), value);
    spin_unlock_irqrestore(&t380_source_lock, flags);
}
EXPORT_SYMBOL_GPL(t380_source_system_suspend);
void t380_source_disconnect(void)
{
    unsigned long flags;
    spin_lock_irqsave(&t380_source_lock, flags);
    t380_stock_usb_ma=0;
    t380_sl_disconnect(&t380_source_state, t380_source_now());
    spin_unlock_irqrestore(&t380_source_lock, flags);
}
EXPORT_SYMBOL_GPL(t380_source_disconnect);
#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
void t380_source_host_setup(void)
{
    unsigned long flags;
    spin_lock_irqsave(&t380_source_lock, flags);
    t380_sl_host_setup(&t380_source_state, t380_source_now());
    spin_unlock_irqrestore(&t380_source_lock, flags);
}
EXPORT_SYMBOL_GPL(t380_source_host_setup);
static bool t380_source_qualify_fixed5v(void)
{
    unsigned long flags;
    bool qualified;
    spin_lock_irqsave(&t380_source_lock, flags);
    qualified = t380_sl_qualify_fixed5v(&t380_source_state, t380_source_now());
    spin_unlock_irqrestore(&t380_source_lock, flags);
    return qualified;
}
/* Profile-specific operator declaration; detection remains NONSTANDARD_DCD. */
static bool t380_source_qualify_operator900(void)
{
    unsigned long flags;
    bool qualified;
    spin_lock_irqsave(&t380_source_lock, flags);
    qualified = t380_sl_qualify_operator900(&t380_source_state, t380_source_now());
    spin_unlock_irqrestore(&t380_source_lock, flags);
    return qualified;
}
static void t380_source_unqualify_fixed5v(void)
{
    unsigned long flags;
    spin_lock_irqsave(&t380_source_lock, flags);
    t380_source_state.fixed5v_qualified = false;
    t380_source_state.fixed5v_input_allowance_ma = 0;
    spin_unlock_irqrestore(&t380_source_lock, flags);
}
#endif

static void t380_source_publish_charger(u64 ticket, u64 started, u64 ended,
                                        bool valid, bool present, bool acceptable)
{
    unsigned long flags;
    spin_lock_irqsave(&t380_source_lock, flags);
    t380_sl_charger(&t380_source_state, ticket, started, ended,
                   t380_source_now(), valid, present, acceptable);
    spin_unlock_irqrestore(&t380_source_lock, flags);
}
static void t380_source_publish_muic_class(u64 ticket, u64 started, u64 ended,
                                     bool valid, bool present, bool sdp, bool dcp,
                                     bool fixed5v)
{
    unsigned long flags;
    spin_lock_irqsave(&t380_source_lock, flags);
    t380_sl_muic_class(&t380_source_state, ticket, started, ended,
                t380_source_now(), valid, present, sdp, dcp, fixed5v);
    spin_unlock_irqrestore(&t380_source_lock, flags);
}
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
static bool t380_source_publish_detection_pending(u64 ticket, u64 started, u64 ended)
{
    unsigned long flags;
    bool accepted;
    spin_lock_irqsave(&t380_source_lock, flags);
    accepted = t380_sl_detection_pending(&t380_source_state, ticket,
        started, ended, t380_source_now());
    spin_unlock_irqrestore(&t380_source_lock, flags);
    return accepted;
}
#endif
static int t380_source_format(char *buf, size_t size)
{
    struct t380_sl_snapshot o;
    unsigned long flags;
    spin_lock_irqsave(&t380_source_lock, flags);
    t380_sl_copy(&t380_source_state, t380_source_now(), &o);
    spin_unlock_irqrestore(&t380_source_lock, flags);
    return scnprintf(buf, size,
        "source_record_serialized=1 request_is_configuration=1 request_heartbeat_required=0 hardware_atomic=0 current_permission=0 source_validated=0 telemetry_validated=0 normal_charge_region_validated=0 hardware_sample_age_known=0\n"
        "source_generation=%llu source_reason=%d sdp_candidate=%d dcp_candidate=%d fixed5v_candidate=%d fixed5v_qualified=%d fixed5v_input_allowance_ma=%u clean_absence=%d host_blocked=%d candidate_request_ma=%u last_requested_ma=%u source_now_ms=%llu source_oldest_ms=%llu request_callback_ms=%llu discarded_pre_epoch_reads=%llu bus_suspended=%d system_suspended=%d\n",
        (unsigned long long)o.generation, o.reason, o.sdp_candidate, o.dcp_candidate,
        o.fixed5v_candidate, o.fixed5v_qualified, o.fixed5v_input_allowance_ma, o.clean_absence, o.host_blocked,
        o.candidate_request_ma, o.last_requested_ma, (unsigned long long)o.now_ms,
        (unsigned long long)o.oldest_ms, (unsigned long long)o.request_ms,
        (unsigned long long)o.discarded, o.bus_suspended, o.system_suspended);
}
#endif
