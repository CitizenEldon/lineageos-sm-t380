/* SPDX-License-Identifier: GPL-2.0-only */
/* Include only in t380_diagnostic_power.c; same 0x49 owner, no new driver. */
#ifndef T380_CHARGER_OBSERVATION_SYSFS_H
#define T380_CHARGER_OBSERVATION_SYSFS_H
#include <linux/sysfs.h>
#include <linux/timekeeping.h>
#include "t380_charger_observation.h"

static int t380_co_kernel_guard(void *context)
{
	struct i2c_client *client = context;

	if (client->addr != 0x49 ||
	    !of_find_property(client->dev.of_node, "samsung,charge-inhibit", NULL) ||
	    !of_device_is_compatible(client->dev.of_node, "samsung,t380-sm5703-diagnostic"))
		return -EINVAL;
	return t380_charge_inhibit_check(&client->dev);
}
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
static int t380_co_trial_guard(void *context)
{
	struct i2c_client *client = context;
	return client->addr == 0x49 ?
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
		t380_charge_active_check(&client->dev) : -EINVAL;
#else
		t380_charge_trial_check(&client->dev) : -EINVAL;
#endif
}
#endif

static int t380_co_kernel_read(void *context, unsigned char address)
{
	return i2c_smbus_read_byte_data(context, address);
}

/* Caller holds the existing charger control mutex across sampling/publication. */
static int t380_co_collect_locked_mode(struct i2c_client *client,
        struct t380_co_observation *o, u64 *started_ms, u64 *ended_ms,
        bool active)
{
    struct t380_co_reader reader = {
        .context = client, .inhibit_check = t380_co_kernel_guard,
        .read_byte = t380_co_kernel_read,
    };
    u64 ticket = t380_source_ticket();
    int ret;
    bool absent_uvlo;

#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
    if (active) reader.inhibit_check = t380_co_trial_guard;
#else
    if (active) return -EOPNOTSUPP;
#endif

    *started_ms = t380_source_now();
    ret = t380_co_acquire_mode(&reader, o, active);
    *ended_ms = t380_source_now();
    /* With the inhibited switch off, VBUSOK=0/UVLO=1 is the observed
     * unplugged state. Publish that valid absence instead of disconnecting
     * the source epoch before its acquisition ticket can be consumed. */
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
    if (active && ret == -ECANCELED && o->guard_cancelled) return ret;
    if (active && !ret && t380_co_expected_absent_uvlo(o, false)) {
        ret = t380_charge_household_detach_start(&client->dev);
        if (!ret) {
            t380_source_publish_charger(ticket, *started_ms, *ended_ms,
                true, false, false);
            o->guard_cancelled = true;
            return -ECANCELED;
        }
    }
#endif
    absent_uvlo = t380_co_expected_absent_uvlo(o, active);
    if (ret || !o->valid || (!active && o->chgon) || o->otgfail || o->vbusovp ||
        (o->vbusuvlo && !absent_uvlo) || o->vbus_conflict)
        t380_control_fault_locked(client, ret ? ret : -EIO);
    t380_source_publish_charger(ticket, *started_ms, *ended_ms,
        !ret && o->valid, !ret && o->valid && o->vbusok,
        !ret && o->valid && (active || !o->chgon) && !o->otgfail && !o->vbusovp &&
        !o->vbusuvlo && !o->vbus_conflict);
    return ret;
}
static int t380_co_collect_locked(struct i2c_client *client,
        struct t380_co_observation *o, u64 *started_ms, u64 *ended_ms)
{
    return t380_co_collect_locked_mode(client, o, started_ms, ended_ms, false);
}
static ssize_t charger_observation_show(struct device *dev,
                                       struct device_attribute *attr, char *buf)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct t380_co_observation o;
    struct t380_control *control = i2c_get_clientdata(client);
    u64 started_ms, ended_ms;
    int ret, length;

    mutex_lock(&control->lock);
    ret = t380_co_collect_locked(client, &o, &started_ms, &ended_ms);
    mutex_unlock(&control->lock);
	if (ret)
		return ret;
	length = scnprintf(buf, PAGE_SIZE,
		"read_only=1 inhibit=1 snapshot_valid=%d atomic=0 hardware_sample_age_known=0 telemetry_validated=0 source_validated=0 fault_coverage_complete=0 input_hardware_range_validated=0 board_allowance_validated=0\n"
		"started_boottime_ms=%llu ended_boottime_ms=%llu\n"
		"status1=0x%02x status2=0x%02x status3=0x%02x status5=0x%02x cntl=0x%02x vbuscntl=0x%02x chgcntl2=0x%02x chgcntl3=0x%02x chgcntl4=0x%02x chgcntl5=0x%02x chgcntl6=0x%02x\n"
		"chgon=%d topoff=%d done=%d aicl_active=%d otgfail=%d vbusok=%d vbusovp=%d vbusuvlo=%d vbus_conflict=%d status2_decoded=0\n"
		"operation_mode_code=%d operation_mode_supported=%d autoset=%d autostop=%d aiclen=%d\n"
		"input_limit_code=%d input_limit_stock_ma=%d fast_current_code=%d fast_current_in_setter_range=%d fast_current_stock_ma=%d float_voltage_mv=%d topoff_current_ma=%d\n"
		"aicl_threshold_code=%d aicl_threshold_supported=%d aicl_threshold_mv=%d topoff_timer_minutes=%d frequency_khz=%d\n",
		o.valid, (unsigned long long)started_ms, (unsigned long long)ended_ms,
		o.raw[T380_CO_STATUS1], o.raw[T380_CO_STATUS2], o.raw[T380_CO_STATUS3],
		o.raw[T380_CO_STATUS5], o.raw[T380_CO_CNTL], o.raw[T380_CO_VBUSCNTL],
		o.raw[T380_CO_CHGCNTL2], o.raw[T380_CO_CHGCNTL3], o.raw[T380_CO_CHGCNTL4],
		o.raw[T380_CO_CHGCNTL5], o.raw[T380_CO_CHGCNTL6],
		o.chgon, o.topoff, o.done, o.aicl_active, o.otgfail,
		o.vbusok, o.vbusovp, o.vbusuvlo, o.vbus_conflict,
		o.operation_mode_code, o.operation_mode_supported, o.autoset, o.autostop, o.aiclen,
		o.input_limit_code, o.input_limit_stock_ma,
		o.fast_current_code, o.fast_current_in_setter_range, o.fast_current_stock_ma,
		o.float_voltage_mv, o.topoff_current_ma, o.aicl_threshold_code,
		o.aicl_threshold_supported, o.aicl_threshold_mv,
		o.topoff_timer_minutes, o.frequency_khz);
	return length + t380_source_format(buf + length, PAGE_SIZE - length);
}
static DEVICE_ATTR_RO(charger_observation);

static void t380_co_sysfs_remove(void *context)
{
	struct i2c_client *client = context;

	device_remove_file(&client->dev, &dev_attr_charger_observation);
	t380_source_disconnect();
}

static int t380_co_sysfs_add(struct i2c_client *client)
{
	int ret = t380_co_kernel_guard(client);

	if (ret)
		return ret;
	ret = device_create_file(&client->dev, &dev_attr_charger_observation);
	if (ret)
		return ret;
	return devm_add_action_or_reset(&client->dev, t380_co_sysfs_remove, client);
}
#endif
