/* SPDX-License-Identifier: GPL-2.0-only */
/* Candidate include for the existing diagnostic owner; no runtime integration. */
#ifndef T380_MUIC_OBSERVATION_SYSFS_H
#define T380_MUIC_OBSERVATION_SYSFS_H
#include <linux/i2c.h>
#include <linux/math64.h>
#include <linux/of.h>
#include <linux/sysfs.h>
#include <linux/timekeeping.h>
#include <linux/t380_diagnostic_power.h>
#include "t380_muic_observation.h"

static int t380_mo_kernel_guard(void *context)
{
	struct i2c_client *client = context;
	if (client->addr != 0x25 ||
	    !of_device_is_compatible(client->dev.of_node,
		"samsung,t380-sm5703-muic-observation") ||
	    !of_find_property(client->dev.of_node, "samsung,charge-inhibit", NULL))
		return -EINVAL;
	return t380_charge_inhibit_check(&client->dev);
}
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
static int t380_mo_trial_guard(void *context)
{
	struct i2c_client *client = context;
	return client->addr == 0x25 ?
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
		t380_charge_active_check(&client->dev) : -EINVAL;
#else
		t380_charge_trial_check(&client->dev) : -EINVAL;
#endif
}
#endif
static int t380_mo_kernel_read(void *context, unsigned char address)
{
	return i2c_smbus_read_byte_data(context, address);
}
static const char *t380_mo_pattern_name(enum t380_mo_pattern pattern)
{
	switch (pattern) {
	case T380_MO_NONE_PATTERN: return "none_pattern";
	case T380_MO_SDP_PATTERN: return "sdp_pattern";
	case T380_MO_CDP_PATTERN: return "cdp_pattern";
	case T380_MO_DCP_PATTERN: return "dcp_pattern";
	case T380_MO_FIXED5V_TIMEOUT_PATTERN: return "fixed5v_timeout_pattern";
	default: return "raw_unknown";
	}
}
/* One physical MUIC on the selected board; serialize both complete sweeps. */
static DEFINE_MUTEX(t380_muic_observation_lock);
static int t380_mo_collect_mode(struct i2c_client *client,
        struct t380_mo_observation *o, u64 *started_ms, u64 *ended_ms,
        bool active)
{
    struct t380_mo_reader reader = {
        .context = client, .inhibit_check = t380_mo_kernel_guard,
        .read_byte = t380_mo_kernel_read,
    };
    u64 ticket;
    int ret;

#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
    if (active) reader.inhibit_check = t380_mo_trial_guard;
#else
    if (active) return -EOPNOTSUPP;
#endif

    mutex_lock(&t380_muic_observation_lock);
    ticket = t380_source_ticket();
    *started_ms = t380_source_now();
    ret = t380_mo_acquire(&reader, o);
    *ended_ms = t380_source_now();
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
    if (active && ret == -ECANCELED && !o->read_error) {
        mutex_unlock(&t380_muic_observation_lock);
        return ret;
    }
    if (!active && !ret && t380_mo_exact_detection_pending(o) &&
        !t380_charge_household_pending_ready(&client->dev) &&
        t380_source_publish_detection_pending(ticket, *started_ms, *ended_ms)) {
        mutex_unlock(&t380_muic_observation_lock);
        return 0; /* Still UNKNOWN; no qualification/current permission. */
    }
#endif
    t380_source_publish_muic_class(ticket, *started_ms, *ended_ms,
        !ret && o->valid && o->stable, !ret && o->valid && o->vbusin,
        !ret && o->valid && o->pattern == T380_MO_SDP_PATTERN,
        !ret && o->valid && o->pattern == T380_MO_DCP_PATTERN,
        !ret && o->valid && o->pattern == T380_MO_FIXED5V_TIMEOUT_PATTERN);
    mutex_unlock(&t380_muic_observation_lock);
    return ret;
}
static int t380_mo_collect(struct i2c_client *client,
        struct t380_mo_observation *o, u64 *started_ms, u64 *ended_ms)
{
    return t380_mo_collect_mode(client, o, started_ms, ended_ms, false);
}
static ssize_t muic_observation_show(struct device *dev,
                                   struct device_attribute *attr, char *buf)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct t380_mo_observation o;
    u64 started_ms, ended_ms;
    int length;
    int ret = t380_mo_collect(client, &o, &started_ms, &ended_ms);
	/* Failed observations remain inspectable, explicitly invalid; signed raw
	 * fields retain errno/impossible-byte values. Never re-use an old sample. */
	length = scnprintf(buf, PAGE_SIZE,
		"read_only=1 no_actuation=1 snapshot_valid=%d stable_sweeps=%d atomic=0 hardware_sample_age_known=0 source_validated=0 current_allowance_known=0 current_permission=0 unique_chip_id_verified=0\n"
		"started_boottime_ms=%llu ended_boottime_ms=%llu error=%d read_error=%d guard_before_error=%d guard_after_error=%d failed_pass=%d failed_index=%d read_count=%d\n"
		"raw_encoding=decimal not_read=-1 pass0_devid=%d adc=%d devt1=%d devt2=%d devt3=%d rsvdid1=%d chgtype=%d\n"
		"pass1_devid=%d adc=%d devt1=%d devt2=%d devt3=%d rsvdid1=%d chgtype=%d\n"
		"family_plausible=%d vendor=%d revision=%d adc_code=%d vbusin=%d vbusout=%d chgtype_code=%d observed_pattern=%s\n",
		o.valid, o.stable ? 2 : 0,
		(unsigned long long)started_ms, (unsigned long long)ended_ms,
		ret, o.read_error, o.guard_before_error, o.guard_after_error,
		o.failed_pass, o.failed_index, o.read_count,
		o.raw[0][0], o.raw[0][1], o.raw[0][2], o.raw[0][3],
		o.raw[0][4], o.raw[0][5], o.raw[0][6],
		o.raw[1][0], o.raw[1][1], o.raw[1][2], o.raw[1][3],
		o.raw[1][4], o.raw[1][5], o.raw[1][6],
		o.family_plausible, o.vendor, o.revision, o.adc_code,
		o.vbusin, o.vbusout, o.chgtype_code, t380_mo_pattern_name(o.pattern));
	return length + t380_source_format(buf + length, PAGE_SIZE - length);
}
static DEVICE_ATTR_RO(muic_observation);
static void t380_mo_sysfs_remove(void *context)
{
	struct i2c_client *client = context;
	device_remove_file(&client->dev, &dev_attr_muic_observation);
	t380_source_disconnect();
}
static int t380_mo_sysfs_add(struct i2c_client *client)
{
	int ret = t380_mo_kernel_guard(client);
	if (ret)
		return ret;
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EOPNOTSUPP;
	ret = device_create_file(&client->dev, &dev_attr_muic_observation);
	if (ret)
		return ret;
	return devm_add_action_or_reset(&client->dev, t380_mo_sysfs_remove, client);
}
#endif
