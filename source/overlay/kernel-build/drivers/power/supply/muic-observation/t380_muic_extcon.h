/* SPDX-License-Identifier: GPL-2.0-only */
/* Physical cable presence only. Never a charge-current or OTG-host authority. */
#ifndef T380_MUIC_EXTCON_H
#define T380_MUIC_EXTCON_H
#include <linux/extcon-provider.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#include "t380_muic_extcon_transition.h"
#include "../power076-muic/t380_muic_retry.h"

#define T380_MUIC_VBUS_POLL_MS 2000
static const unsigned int t380_muic_cables[] = {
	EXTCON_USB, EXTCON_USB_HOST, EXTCON_NONE
};

struct t380_muic_extcon {
	struct t380_mr_state retry;
	struct i2c_client *client;
	struct extcon_dev *edev;
	struct delayed_work work;
	u64 last_good_ms;
	int last_error;
	unsigned int failures;
	bool attached;
	int notify_pending;
	bool running;
};

#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
struct t380_me_reader_context {
	struct i2c_client *client;
	int mode;
};

static int t380_me_guard(void *context)
{
	struct t380_me_reader_context *reader = context;
	struct i2c_client *client = reader->client;
	int mode;

	if (client->addr != 0x25 ||
	    !of_device_is_compatible(client->dev.of_node,
		"samsung,t380-sm5703-muic-observation") ||
	    !of_find_property(client->dev.of_node, "samsung,charge-inhibit", NULL))
		return -EINVAL;
	mode = t380_charge_observation_mode(&client->dev);
	return mode < 0 ? mode : mode == reader->mode ? 0 : -EAGAIN;
}

static int t380_me_read_byte(void *context, unsigned char address)
{
	struct t380_me_reader_context *reader = context;

	return t380_mo_kernel_read(reader->client, address);
}
#endif

/* Reuse the existing guarded two-sweep decoder, without publishing a second
 * charger source sample. A change of guard mode during a sweep fails closed. */
static int t380_me_read(struct i2c_client *client, bool *attached)
{
	struct t380_muic_extcon *m=i2c_get_clientdata(client);
	bool awaiting_physical_check;
	struct t380_mo_observation o;
	struct t380_mo_reader reader = {
		.context = client, .inhibit_check = t380_mo_kernel_guard,
		.read_byte = t380_mo_kernel_read,
	};
	int ret;

#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
	struct t380_me_reader_context context = { .client = client };

	ret = t380_charge_observation_mode(&client->dev);
	if (ret < 0)
		return ret;
	context.mode = ret;
	reader.context = &context;
	reader.inhibit_check = t380_me_guard;
	reader.read_byte = t380_me_read_byte;
#endif
	mutex_lock(&t380_muic_observation_lock);
	ret = t380_mo_acquire(&reader, &o);
	awaiting_physical_check=m && m->retry.spent &&
		(m->retry.pending_route || m->retry.error);
	mutex_unlock(&t380_muic_observation_lock);
	if (ret)
		return ret;
	if (!o.valid || !o.stable)
		return -EAGAIN;
	/* Only the charger + MUIC joint absence observation can end a retry
	 * episode. Do not announce RESET's transient VBUS loss as a cable pull. */
	if (awaiting_physical_check && !o.vbusin)
		return -EAGAIN;
	*attached = o.vbusin;
	return 0;
}

static int t380_me_get(void *context)
{
	return extcon_get_state(context, EXTCON_USB);
}

static int t380_me_set(void *context, int attached)
{
	return extcon_set_state_sync(context, EXTCON_USB, !!attached);
}

static int t380_me_notify(void *context, int attached)
{
	int ret = extcon_blocking_sync(context, EXTCON_USB, attached);

	return ret & NOTIFY_STOP_MASK ? -EIO : 0;
}

static void t380_me_poll(struct work_struct *work)
{
	struct t380_muic_extcon *m = container_of(to_delayed_work(work),
						  struct t380_muic_extcon, work);
	struct t380_ex_io io = {
		.context = m->edev, .get = t380_me_get,
		.set_sync = t380_me_set, .notify = t380_me_notify,
	};
	bool attached;
	int ret, cached, pending;

	if (!READ_ONCE(m->running))
		return;
	ret = t380_me_read(m->client, &attached);
	if (!ret) {
		WRITE_ONCE(m->last_good_ms, t380_source_now());
		cached = READ_ONCE(m->attached);
		pending = READ_ONCE(m->notify_pending);
		ret = t380_ex_publish(&io, attached, &cached, &pending);
		WRITE_ONCE(m->attached, !!cached);
		WRITE_ONCE(m->notify_pending, pending);
	}
	WRITE_ONCE(m->last_error, ret);
	WRITE_ONCE(m->failures, ret ? READ_ONCE(m->failures) + 1 : 0);
	if (READ_ONCE(m->running))
		mod_delayed_work(system_freezable_wq, &m->work,
				 msecs_to_jiffies(T380_MUIC_VBUS_POLL_MS));
}

static ssize_t muic_vbus_extcon_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct t380_muic_extcon *m = i2c_get_clientdata(to_i2c_client(dev));
	u64 last, now;

	if (!m)
		return -ENODEV;
	last = READ_ONCE(m->last_good_ms);
	now = t380_source_now();
	return scnprintf(buf, PAGE_SIZE,
		"read_only=1 cable_attached=%d last_good_boottime_ms=%llu age_ms=%llu last_error=%d failures=%u poll_ms=%u host_asserted=0 charge_permission=0\n",
		READ_ONCE(m->attached), (unsigned long long)last,
		(unsigned long long)(now - last), READ_ONCE(m->last_error),
		READ_ONCE(m->failures), T380_MUIC_VBUS_POLL_MS);
}
static DEVICE_ATTR_RO(muic_vbus_extcon);

/* Cached diagnostics only; no raw bus read or mutation from sysfs. */
static ssize_t muic_retry_status_show(struct device *dev,
	struct device_attribute *attr,char *buf)
{
	struct t380_muic_extcon *m=i2c_get_clientdata(to_i2c_client(dev));
	struct t380_mr_state *s;
	ssize_t n;
	unsigned int i;
	if(!m)return -ENODEV;
	mutex_lock(&t380_muic_observation_lock);
	s=&m->retry;
	n=scnprintf(buf,PAGE_SIZE,"retry_v1=1 spent=%u pending_route=%u attempts=%u completed=%u failures=%u generation=%u stage=%u error=%d attempted_ms=%llu before_valid=%d after_valid=%d before=%02x,%02x,%02x,%02x,%02x,%02x after=%02x,%02x,%02x,%02x,%02x,%02x\n",
	 s->spent,s->pending_route,s->attempts,s->completed,s->failures,s->generation,
	 s->stage,s->error,s->attempted_ms,s->before_valid,s->after_valid,
	 s->before.dev1,s->before.dev2,s->before.dev3,s->before.adc,s->before.vbus,s->before.chgtype,
	 s->after.dev1,s->after.dev2,s->after.dev3,s->after.adc,s->after.vbus,s->after.chgtype);
	for(i=0;i<T380_MR_CONFIG_COUNT;i++)
	 n+=scnprintf(buf+n,PAGE_SIZE-n,"reg=%02x before=%02x after=%02x\n",
	  t380_mr_config_regs[i],s->config_before[i],s->config_after[i]);
	mutex_unlock(&t380_muic_observation_lock);
	return n;
}
static DEVICE_ATTR_RO(muic_retry_status);

static void t380_me_stop(void *context)
{
	struct t380_muic_extcon *m = context;

	WRITE_ONCE(m->running, false);
	cancel_delayed_work_sync(&m->work);
	device_remove_file(&m->client->dev, &dev_attr_muic_vbus_extcon);
	device_remove_file(&m->client->dev, &dev_attr_muic_retry_status);
}

static int t380_me_add(struct i2c_client *client)
{
	struct t380_muic_extcon *m;
	bool attached;
	int ret;

	/* No valid first reading means no provider: USB must probe-defer rather
	 * than start with an invented disconnected state. */
	ret = t380_me_read(client, &attached);
	if (ret)
		return ret == -ENODEV || ret == -EINVAL ? ret : -EPROBE_DEFER;
	m = devm_kzalloc(&client->dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->client = client;
	m->attached = attached;
	m->last_good_ms = t380_source_now();
	m->edev = devm_extcon_dev_allocate(&client->dev, t380_muic_cables);
	if (IS_ERR(m->edev))
		return PTR_ERR(m->edev);
	ret = devm_extcon_dev_register(&client->dev, m->edev);
	if (ret)
		return ret;
	ret = extcon_set_state_sync(m->edev, EXTCON_USB, attached);
	if (ret && extcon_get_state(m->edev, EXTCON_USB) != attached)
		return ret;
	m->last_error = ret;
	INIT_DELAYED_WORK(&m->work, t380_me_poll);
	i2c_set_clientdata(client, m);
	ret = device_create_file(&client->dev, &dev_attr_muic_vbus_extcon);
	if (ret)
		return ret;
	ret = device_create_file(&client->dev, &dev_attr_muic_retry_status);
	if (ret) {
		device_remove_file(&client->dev, &dev_attr_muic_vbus_extcon);
		return ret;
	}
	ret = devm_add_action_or_reset(&client->dev, t380_me_stop, m);
	if (ret)
		return ret;
	WRITE_ONCE(m->running, true);
	mod_delayed_work(system_freezable_wq, &m->work,
			 msecs_to_jiffies(T380_MUIC_VBUS_POLL_MS));
	return 0;
}

static void t380_me_suspend(struct i2c_client *client)
{
	struct t380_muic_extcon *m = i2c_get_clientdata(client);

	if (!m)
		return;
	WRITE_ONCE(m->running, false);
	cancel_delayed_work_sync(&m->work);
}

static void t380_me_resume(struct i2c_client *client)
{
	struct t380_muic_extcon *m = i2c_get_clientdata(client);

	if (!m)
		return;
	WRITE_ONCE(m->running, true);
	mod_delayed_work(system_freezable_wq, &m->work, 0);
}
#endif
