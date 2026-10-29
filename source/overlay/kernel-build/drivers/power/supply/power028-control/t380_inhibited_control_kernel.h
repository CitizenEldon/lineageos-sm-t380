/* SPDX-License-Identifier: GPL-2.0-only */
/* Included once by the existing 0x49 power owner, before observation glue. */
#ifndef T380_INHIBITED_CONTROL_KERNEL_H
#define T380_INHIBITED_CONTROL_KERNEL_H
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include "t380_charger_settings.c"
#include "t380_inhibited_control.c"

struct t380_periodic;
struct t380_stock_runtime;
struct t380_control {
	struct t380_periodic *periodic;
 struct t380_stock_runtime *stock;
	struct mutex lock;
	struct t380_cs_plan plan;
	struct t380_ic_result initial;
	u64 initialized_ms;
	int fault;
	bool inhibited_programming;
};

static int t380_control_off(void *context)
{
	struct i2c_client *client = context;

	return t380_charge_force_off(&client->dev);
}
static int t380_control_guard(void *context)
{
	struct i2c_client *client = context;

	return t380_charge_inhibit_check(&client->dev);
}
static int t380_control_read(void *context, int address)
{
	return i2c_smbus_read_byte_data(context, (u8)address);
}
static int t380_control_write(void *context, int address, int value)
{
	return i2c_smbus_write_byte_data(context, (u8)address, (u8)value);
}
static void t380_control_cleanup(void *context)
{
	/* Independent of the I2C adapter, transaction lock and register state. */
	int ret = t380_control_off(context);

	if (ret)
		dev_err(&((struct i2c_client *)context)->dev,
			"GPIO HIGH cleanup failed: %d\n", ret);
}
static int t380_control_init(struct i2c_client *client)
{
	/* Explicit conservative cap, not cycle count or source permission. */
	const struct t380_cs_limits limits = { 100, 100, 4190, 350 };
	struct t380_ic_io io = {
		.context = client, .off = t380_control_off,
		.guard = t380_control_guard, .read = t380_control_read,
		.write = t380_control_write,
	};
	struct t380_control *control;
	int ret;

	control = devm_kzalloc(&client->dev, sizeof(*control), GFP_KERNEL);
	if (!control)
		return -ENOMEM;
	mutex_init(&control->lock);
	i2c_set_clientdata(client, control);
	/* Preserve diagnostic-hog telemetry with no settings/GPIO writes. */
	ret = t380_charge_safety_ready(&client->dev);
	if (ret == -EOPNOTSUPP)
		return 0;
	if (ret)
		return ret;
	control->inhibited_programming = true;
	ret = devm_add_action_or_reset(&client->dev, t380_control_cleanup, client);
	if (ret)
		return ret;
	ret = t380_cs_encode_profile(&limits, T380_CS_BOARD_INIT, &control->plan);
	if (ret)
		return ret;
	mutex_lock(&control->lock);
	ret = t380_ic_apply(&io, &control->plan, &control->initial);
	control->initialized_ms = t380_source_now();
	control->fault = ret;
	mutex_unlock(&control->lock);
	return ret;
}
static void t380_control_fault_locked(struct i2c_client *client, int error)
{
	struct t380_control *control = i2c_get_clientdata(client);
	int off_error = control->inhibited_programming ? t380_control_off(client) :
		t380_control_guard(client);

	if (!control->fault)
		control->fault = error ? error : off_error;
	t380_source_disconnect();
}
static int t380_control_read_serialized(struct i2c_client *client, u8 address)
{
	struct t380_control *control = i2c_get_clientdata(client);
	bool active = false;
	int ret;

	if (!control)
		return -ENODEV;
	mutex_lock(&control->lock);
 ret=t380_charge_observation_mode(&client->dev);
 active=ret==1;
 if (ret>=0) ret=i2c_smbus_read_byte_data(client,address);

	if (ret < 0 || (address == 0x0a && (ret & 1) && !active))
		t380_control_fault_locked(client, ret < 0 ? ret : -EIO);
	mutex_unlock(&control->lock);
	return ret;
}
#endif
