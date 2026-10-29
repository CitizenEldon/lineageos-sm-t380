// SPDX-License-Identifier: GPL-2.0-only
/* Minimal SM-T380 FTS8 input port. Protocol and rail ordering are from
 * Samsung fts_ts.c/fts_ts.h at 36c20b2257535bf07ef1439aae813b843932f5f4.
 * No firmware update, calibration, flash, factory or secure-touch commands.
 */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>

#define T380_FTS8_FINGERS 10
#define T380_FTS8_FIFO 32
#define T380_FTS8_EVENT_SIZE 8

struct t380_fts8 {
	struct i2c_client *client;
	struct input_dev *input;
	struct regulator *avdd;
	struct regulator *iovdd;
	struct pinctrl *pinctrl;
	struct pinctrl_state *active;
	struct pinctrl_state *sleep;
	/* Serializes transfers, reporting and controller state. */
	struct mutex lock;
	u32 max_x;
	u32 max_y;
	unsigned int empty_irqs;
	unsigned long empty_window;
	bool avdd_enabled;
	bool iovdd_enabled;
	bool irq_requested;
	bool irq_enabled;
	bool running;
};

static int t380_fts8_transfer(struct t380_fts8 *ts, const u8 *cmd,
			      size_t cmd_len, u8 *data, size_t data_len)
{
	struct i2c_msg msgs[] = {
		{ .addr = ts->client->addr, .len = cmd_len, .buf = (u8 *)cmd },
		{ .addr = ts->client->addr, .flags = I2C_M_RD,
		  .len = data_len, .buf = data },
	};
	int count = data_len ? 2 : 1;
	int attempt, ret;

	/* Unlike the legacy port, a short nonnegative transfer is an error. */
	for (attempt = 0; attempt < 3; attempt++) {
		ret = i2c_transfer(ts->client->adapter, msgs, count);
		if (ret == count)
			return 0;
		if (ret >= 0)
			ret = -EIO;
		if (attempt != 2)
			usleep_range(10000, 10100);
	}
	return ret;
}

static int t380_fts8_command(struct t380_fts8 *ts, u8 command)
{
	return t380_fts8_transfer(ts, &command, 1, NULL, 0);
}

static int t380_fts8_interrupts(struct t380_fts8 *ts, bool enable)
{
	u8 command[] = { 0xb6, 0x00, 0x2c, enable ? 0x48 : 0x08 };

	return t380_fts8_transfer(ts, command, sizeof(command), NULL, 0);
}

static int t380_fts8_power_off(struct t380_fts8 *ts)
{
	int ret, first_error = 0;

	/* Legacy order: IO off, then analog off, then sleep pinctrl. */
	if (ts->iovdd_enabled) {
		ret = regulator_disable(ts->iovdd);
		if (ret)
			return ret; /* Keep AVDD on if IO could not be removed first. */
		ts->iovdd_enabled = false;
	}
	if (ts->avdd_enabled) {
		ret = regulator_disable(ts->avdd);
		if (ret && !first_error)
			first_error = ret;
		if (!ret)
			ts->avdd_enabled = false;
	}
	if (!ts->avdd_enabled && !ts->iovdd_enabled) {
		ret = pinctrl_select_state(ts->pinctrl, ts->sleep);
		if (ret && !first_error)
			first_error = ret;
	}
	return first_error;
}

static int t380_fts8_power_on(struct t380_fts8 *ts)
{
	int ret, cleanup;

	if (ts->avdd_enabled && ts->iovdd_enabled)
		return 0;
	if (ts->avdd_enabled || ts->iovdd_enabled)
		return -EIO;

	ret = regulator_enable(ts->avdd);
	if (ret)
		return ret;
	ts->avdd_enabled = true;
	usleep_range(1000, 1100);
	ret = regulator_enable(ts->iovdd);
	if (ret)
		goto power_off;
	ts->iovdd_enabled = true;
	ret = pinctrl_select_state(ts->pinctrl, ts->active);
	if (ret)
		goto power_off;
	usleep_range(5000, 5100);
	return 0;

power_off:
	cleanup = t380_fts8_power_off(ts);
	if (cleanup)
		dev_err(&ts->client->dev, "power rollback failed: %d\n", cleanup);
	return ret;
}

static void t380_fts8_release(struct t380_fts8 *ts)
{
	int slot;

	for (slot = 0; slot < T380_FTS8_FINGERS; slot++) {
		input_mt_slot(ts->input, slot);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
	}
	input_report_key(ts->input, KEY_APPSELECT, 0);
	input_report_key(ts->input, KEY_BACK, 0);
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

/* Returns a fatal controller error. Caller holds lock. */
static int t380_fts8_event(struct t380_fts8 *ts, const u8 *event)
{
	unsigned int id = event[0] & 0x0f;
	unsigned int slot, x, y;
	bool down;

	if (id >= 3 && id <= 5) {
		slot = event[0] >> 4;
		if (slot >= T380_FTS8_FINGERS)
			return 0;
		x = event[1] | ((event[2] & 0x0f) << 8);
		y = (event[2] >> 4) | (event[3] << 4);
		down = id != 4;
		/* Captured max_coords are inclusive legacy ABS axis maxima.
		 * Release malformed contacts instead of retaining stuck slots.
		 */
		if (x > ts->max_x || y > ts->max_y)
			down = false;
		input_mt_slot(ts->input, slot);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, down);
		if (!down)
			return 0;
		input_report_abs(ts->input, ABS_MT_POSITION_X, x);
		input_report_abs(ts->input, ABS_MT_POSITION_Y, y);
		input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR,
				 max(event[4], event[5]));
		input_report_abs(ts->input, ABS_MT_TOUCH_MINOR,
				 min(event[4], event[5]));
		input_report_abs(ts->input, ABS_MT_PRESSURE, event[7]);
		return 0;
	}
	/* Full-byte IDs for non-pointer events, as in Samsung FTS8. */
	switch (event[0]) {
	case 0x0e:
		input_report_key(ts->input, KEY_APPSELECT, !!(event[2] & BIT(0)));
		input_report_key(ts->input, KEY_BACK, !!(event[2] & BIT(1)));
		break;
	case 0x0f:
		return -EIO;
	case 0x10:
	case 0x11:
		/* Unexpected reset needs explicit reinitialization, not stale input. */
		return -EPIPE;
	default:
		break;
	}
	return 0;
}

static bool t380_fts8_empty_storm(struct t380_fts8 *ts, unsigned long now)
{
	/* Tolerate isolated empty notifications, but bound a stuck level IRQ. */
	if (!ts->empty_irqs ||
	    time_after(now, ts->empty_window + msecs_to_jiffies(100))) {
		ts->empty_window = now;
		ts->empty_irqs = 1;
	} else {
		ts->empty_irqs++;
	}
	return ts->empty_irqs >= 16;
}

static irqreturn_t t380_fts8_irq(int irq, void *data)
{
	struct t380_fts8 *ts = data;
	const u8 count_cmd[] = { 0xb6, 0x00, 0x23 };
	const u8 events_cmd = 0x86;
	u8 count[2], events[T380_FTS8_FIFO * T380_FTS8_EVENT_SIZE];
	unsigned int total, i;
	int ret;

	mutex_lock(&ts->lock);
	if (!ts->running)
		goto unlock;
	ret = t380_fts8_transfer(ts, count_cmd, sizeof(count_cmd),
				  count, sizeof(count));
	if (ret)
		goto fault;
	/* FTS8 count is second byte / 2; first byte is not an event. */
	total = min_t(unsigned int, count[1] / 2, T380_FTS8_FIFO);
	if (!total) {
		if (t380_fts8_empty_storm(ts, jiffies)) {
			ret = -EIO;
			goto fault;
		}
		goto unlock;
	}
	ts->empty_irqs = 0;
	ret = t380_fts8_transfer(ts, &events_cmd, 1, events,
				  total * T380_FTS8_EVENT_SIZE);
	if (ret)
		goto fault;
	for (i = 0; i < total; i++) {
		ret = t380_fts8_event(ts, &events[i * T380_FTS8_EVENT_SIZE]);
		if (ret)
			goto fault;
	}
	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
	goto unlock;

fault:
	dev_err_ratelimited(&ts->client->dev,
			    "touch IRQ stopped after controller/transfer error: %d\n",
			    ret);
	/* A level-low line must not turn bus failures into an IRQ storm.
	 * Recovery is a subsequent successful system suspend/resume cycle.
	 */
	ts->running = false;
	disable_irq_nosync(ts->client->irq);
	ts->irq_enabled = false;
	t380_fts8_release(ts);
unlock:
	mutex_unlock(&ts->lock);
	return IRQ_HANDLED;
}

static int t380_fts8_init(struct t380_fts8 *ts)
{
	const u8 reset[] = { 0xb6, 0x00, 0x28, 0x80 };
	const u8 identity[] = { 0xb6, 0x00, 0x04 };
	const u8 event_cmd = 0x85;
	u8 event[T380_FTS8_EVENT_SIZE], chip[7];
	int ret, attempt;

	ret = t380_fts8_transfer(ts, reset, sizeof(reset), NULL, 0);
	if (ret)
		return ret;
	usleep_range(10000, 10100);
	for (attempt = 0; attempt < 10; attempt++) {
		ret = t380_fts8_transfer(ts, &event_cmd, 1, event, sizeof(event));
		if (ret)
			return ret;
		if (event[0] == 0x10)
			break;
		if (event[0] == 0x0f && event[1] == 0x03)
			return -EBADMSG; /* Do not attempt a firmware repair. */
		msleep(20);
	}
	if (attempt == 10)
		return -ETIMEDOUT;
	ret = t380_fts8_transfer(ts, identity, sizeof(identity), chip, sizeof(chip));
	if (ret)
		return ret;
	if (chip[1] != 0x36 || chip[2] != 0x70) {
		dev_err(&ts->client->dev, "unexpected touch ID %02x %02x\n",
			chip[1], chip[2]);
		return -ENODEV;
	}
	ret = t380_fts8_interrupts(ts, false);
	if (ret)
		return ret;
	ret = t380_fts8_command(ts, 0xa1); /* Flush FIFO, not calibration. */
	if (ret)
		return ret;
	usleep_range(10000, 10100);
	ret = t380_fts8_command(ts, 0x93); /* Sense on. */
	if (ret)
		return ret;
	return t380_fts8_command(ts, 0x9b); /* Two touch keys on. */
}

static int t380_fts8_stop(struct t380_fts8 *ts)
{
	int ret = 0, power_ret;

	mutex_lock(&ts->lock);
	ts->running = false;
	if (ts->irq_requested && ts->irq_enabled) {
		disable_irq_nosync(ts->client->irq);
		ts->irq_enabled = false;
	}
	mutex_unlock(&ts->lock);
	/* Never wait for an IRQ thread while holding the mutex it needs. */
	if (ts->irq_requested)
		synchronize_irq(ts->client->irq);
	mutex_lock(&ts->lock);
	if (ts->avdd_enabled && ts->iovdd_enabled) {
		ret = t380_fts8_interrupts(ts, false);
		if (!ret)
			ret = t380_fts8_command(ts, 0xa1);
	}
	t380_fts8_release(ts);
	power_ret = t380_fts8_power_off(ts);
	if (power_ret)
		ret = power_ret;
	mutex_unlock(&ts->lock);
	return ret;
}

static int t380_fts8_start(struct t380_fts8 *ts)
{
	int ret, cleanup;

	mutex_lock(&ts->lock);
	if (ts->running) {
		ret = 0;
		goto unlock;
	}
	ret = t380_fts8_power_on(ts);
	if (ret)
		goto unlock;
	ret = t380_fts8_init(ts);
	if (!ret)
		ret = t380_fts8_interrupts(ts, true);
	if (ret) {
		cleanup = t380_fts8_power_off(ts);
		if (cleanup)
			dev_err(&ts->client->dev, "start rollback failed: %d\n", cleanup);
		goto unlock;
	}
	ts->empty_irqs = 0;
	ts->running = true;
	if (!ts->irq_enabled) {
		ts->irq_enabled = true;
		enable_irq(ts->client->irq);
	}
unlock:
	mutex_unlock(&ts->lock);
	return ret;
}

static int t380_fts8_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct t380_fts8 *ts;
	u32 bounds[2];
	int ret, cleanup;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	if (client->irq <= 0)
		return -EINVAL;
	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->client = client;
	mutex_init(&ts->lock);
	i2c_set_clientdata(client, ts);
	ret = of_property_read_u32_array(dev->of_node, "samsung,max-coordinates",
					 bounds, ARRAY_SIZE(bounds));
	if (ret || !bounds[0] || !bounds[1] || bounds[0] > 4095 || bounds[1] > 4095)
		return -EINVAL;
	ts->max_x = bounds[0];
	ts->max_y = bounds[1];
	ts->avdd = devm_regulator_get(dev, "avdd");
	if (IS_ERR(ts->avdd))
		return PTR_ERR(ts->avdd);
	ts->iovdd = devm_regulator_get(dev, "iovdd");
	if (IS_ERR(ts->iovdd))
		return PTR_ERR(ts->iovdd);
	ts->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(ts->pinctrl))
		return PTR_ERR(ts->pinctrl);
	ts->active = pinctrl_lookup_state(ts->pinctrl, "active");
	if (IS_ERR(ts->active))
		return PTR_ERR(ts->active);
	ts->sleep = pinctrl_lookup_state(ts->pinctrl, "sleep");
	if (IS_ERR(ts->sleep))
		return PTR_ERR(ts->sleep);
	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;
	ts->input->name = "T380 FTS8 Touchscreen";
	ts->input->id.bustype = BUS_I2C;
	input_set_capability(ts->input, EV_KEY, KEY_APPSELECT);
	input_set_capability(ts->input, EV_KEY, KEY_BACK);
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, ts->max_x, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, ts->max_y, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, 255, 0, 0);
	ret = input_mt_init_slots(ts->input, T380_FTS8_FINGERS, INPUT_MT_DIRECT);
	if (ret)
		return ret;
	/* Controller interrupts stay disabled until input and IRQ are registered. */
	ret = t380_fts8_power_on(ts);
	if (ret)
		return ret;
	ret = t380_fts8_init(ts);
	if (ret)
		goto power_off;
	ret = input_register_device(ts->input);
	if (ret)
		goto power_off;
	ret = devm_request_threaded_irq(dev, client->irq, NULL, t380_fts8_irq,
					IRQF_ONESHOT, dev_name(dev), ts);
	if (ret)
		goto power_off;
	ts->irq_requested = true;
	ts->irq_enabled = true;
	mutex_lock(&ts->lock);
	ts->running = true;
	ret = t380_fts8_interrupts(ts, true);
	mutex_unlock(&ts->lock);
	if (ret) {
		cleanup = t380_fts8_stop(ts);
		if (cleanup)
			dev_err(dev, "probe rollback failed: %d\n", cleanup);
		return ret;
	}
	dev_info(dev, "FTS8 ID 36 70, %ux%u inclusive raw bounds, 10 slots\n",
		 ts->max_x, ts->max_y);
	return 0;

power_off:
	cleanup = t380_fts8_power_off(ts);
	if (cleanup)
		dev_err(dev, "probe power rollback failed: %d\n", cleanup);
	return ret;
}

static int t380_fts8_remove(struct i2c_client *client)
{
	int ret = t380_fts8_stop(i2c_get_clientdata(client));

	if (ret)
		dev_err(&client->dev, "touch stop failed: %d\n", ret);
	return 0;
}

static void t380_fts8_shutdown(struct i2c_client *client)
{
	int ret = t380_fts8_stop(i2c_get_clientdata(client));

	if (ret)
		dev_err(&client->dev, "touch shutdown failed: %d\n", ret);
}

static int __maybe_unused t380_fts8_suspend(struct device *dev)
{
	struct t380_fts8 *ts = i2c_get_clientdata(to_i2c_client(dev));
	int ret = t380_fts8_stop(ts);

	/* A bus command failure still permits suspend when both gates are off. */
	if (ret && !ts->avdd_enabled && !ts->iovdd_enabled) {
		dev_warn(dev, "touch suspend cleanup failed after power off: %d\n", ret);
		return 0;
	}
	return ret;
}

static int __maybe_unused t380_fts8_resume(struct device *dev)
{
	return t380_fts8_start(i2c_get_clientdata(to_i2c_client(dev)));
}

static SIMPLE_DEV_PM_OPS(t380_fts8_pm, t380_fts8_suspend, t380_fts8_resume);

static const struct of_device_id t380_fts8_of_match[] = {
	{ .compatible = "samsung,t380-fts8" },
	{ }
};
MODULE_DEVICE_TABLE(of, t380_fts8_of_match);

static const struct i2c_device_id t380_fts8_id[] = {
	{ "t380_fts8", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, t380_fts8_id);

static struct i2c_driver t380_fts8_driver = {
	.driver = {
		.name = "t380_fts8",
		.of_match_table = t380_fts8_of_match,
		.pm = &t380_fts8_pm,
	},
	.probe = t380_fts8_probe,
	.remove = t380_fts8_remove,
	.shutdown = t380_fts8_shutdown,
	.id_table = t380_fts8_id,
};
module_i2c_driver(t380_fts8_driver);

MODULE_DESCRIPTION("Minimal Samsung SM-T380 FTS8 touch driver");
MODULE_LICENSE("GPL v2");
