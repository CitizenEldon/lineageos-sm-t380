// SPDX-License-Identifier: GPL-2.0-only
/*
 * T380 autonomous Samsung charging, GPIO ownership and measured telemetry.
 * Register meanings and fixed-point formats come from Samsung SM5703
 * sources at 36c20b2257535bf07ef1439aae813b843932f5f4.
 */
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/t380_diagnostic_power.h>
#include <linux/t380_adc_thermal.h>
static int t380_charge_observation_mode(struct device *dev);
#include "power027-acquisition/t380_power_acquisition.h"
#include "source-lifecycle/t380_source_lifecycle_kernel.h"
#include "power028-control/t380_inhibited_control_kernel.h"
#include "charger-observation/t380_charger_observation.h"
#include "power028-control/t380_charge_safety_kernel.h"
#include "muic-observation/t380_muic_observation.h"
static DEFINE_MUTEX(t380_muic_observation_lock);
static int t380_mo_kernel_guard(void *ctx) {return t380_charge_safety_ready(&((struct i2c_client *)ctx)->dev);}
static int t380_mo_kernel_read(void *ctx,unsigned char reg) {return i2c_smbus_read_byte_data(ctx,reg);}


#define SM5703_STATUS3		0x0a
#define SM5703_STATUS5		0x6b
#define SM5703_CHGON		BIT(0)
#define SM5703_VBUSOK		BIT(5)
#define SM5703_FG_SOC		0x05
#define SM5703_FG_VOLTAGE	0x07
#define SM5703_FG_CURRENT	0x08

#include "power066-regular/t380_regular_full.c"

#define T380_BATTERY_SAMPLES 5
struct t380_ga_actor;
struct t380_battery_thermal {
 struct t380_ga_actor *actor;
	struct iio_channel *channel;
	struct mutex lock;
	struct mutex acquisition_lock;
	struct t380_pa_history history;
 struct t380_rf_capacity capacity;
 bool stock_selected;
 bool regular_selected, regular_model_matched, regular_full_off;
 struct t380_rf_model model;
 struct t380_rf_diagnostic diagnostic;
 u64 regular_full_off_ms;

	struct t380_adc_point *table;
	int count;
};

#include "power071-acquisition/t380_gauge_actor_kernel.h"

static ssize_t android_telemetry_show(struct device *dev,
 struct device_attribute *attr, char *buf)
{
 struct t380_battery_thermal *sensor = i2c_get_clientdata(to_i2c_client(dev));
 return sensor && sensor->actor ? t380_ga_android_telemetry_show(sensor, buf) : -ENODEV;
}
static DEVICE_ATTR_RO(android_telemetry);
static struct attribute *t380_android_telemetry_attrs[] = {&dev_attr_android_telemetry.attr, NULL};
static const struct attribute_group t380_android_telemetry_group = {.attrs = t380_android_telemetry_attrs};

/* The gauge owns this sensor; devres removes its supply before the channel. */
static int t380_battery_thermal_init(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *np = dev->of_node;
	struct t380_battery_thermal *sensor;
	u32 value;
	int count, i, ret;

	count = of_property_count_u32_elems(np, "samsung,adc-codes");
	if (count < 2 || count > T380_ADC_TABLE_MAX ||
	    count != of_property_count_u32_elems(np,
				"samsung,temperatures-decicelsius"))
		return -EINVAL;
	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	sensor->table = devm_kcalloc(dev, count, sizeof(*sensor->table), GFP_KERNEL);
	if (!sensor->table)
		return -ENOMEM;
	sensor->count = count;
	for (i = 0; i < count; i++) {
		ret = of_property_read_u32_index(np, "samsung,adc-codes", i,
						&sensor->table[i].adc);
		if (ret)
			return ret;
		ret = of_property_read_u32_index(np,
			"samsung,temperatures-decicelsius", i, &value);
		if (ret)
			return ret;
		sensor->table[i].decicelsius = (s32)value;
	}
	ret = t380_adc_validate_table(sensor->table, count);
	if (ret)
		return ret;
	sensor->channel = devm_iio_channel_get(dev, "battery-thermistor");
	if (IS_ERR(sensor->channel))
		return PTR_ERR(sensor->channel);
	mutex_init(&sensor->lock);
	mutex_init(&sensor->acquisition_lock);
 sensor->stock_selected=true;
 t380_rf_capacity_init(&sensor->capacity);
 t380_rf_model_init(&sensor->model);
 t380_rf_diagnostic_init(&sensor->diagnostic);
	i2c_set_clientdata(client, sensor);
	ret = t380_ga_init(client, sensor);
	if (ret) return ret;
	return devm_device_add_group(dev, &t380_android_telemetry_group);
}

#include "power027-acquisition/t380_power_acquisition_kernel.h"


#include "muic-observation/t380_muic_extcon.h"
#include "power076-muic/t380_muic_retry_kernel.h"
#include "power075-stock/t380_stock_kernel.h"

static enum power_supply_property t380_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
};

static enum power_supply_property t380_fg_props[] = {
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
};

static int t380_power_get(struct power_supply *psy,
			 enum power_supply_property prop,
			 union power_supply_propval *value)
{
	struct i2c_client *client = power_supply_get_drvdata(psy);

 int i;
 for(i=0;i<psy->desc->num_properties;i++)
  if(psy->desc->properties[i]==prop)return t380_stock_property(psy,client,prop,value);
 return -EINVAL;
}

static const struct power_supply_desc t380_charger_desc = {
	.name = "t380-diagnostic-usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = t380_charger_props,
	.num_properties = ARRAY_SIZE(t380_charger_props),
	.get_property = t380_power_get,
};

static const struct power_supply_desc t380_fg_desc = {
	/* TEMP creates a thermal zone: its name must be shorter than 20. */
	.name = "t380-diag-gauge",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = t380_fg_props,
	.num_properties = ARRAY_SIZE(t380_fg_props),
	.get_property = t380_power_get,
};

static int t380_power_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct power_supply_config config = {};
	struct t380_stock_dependencies *dependencies = NULL;
	const struct power_supply_desc *desc;
	struct power_supply *psy;
	bool gauge;
	int ret;

	/* This driver MUST have a guard, unlike unrelated USB consumers. */
	if (!of_find_property(client->dev.of_node, "samsung,charge-inhibit", NULL))
		return -EINVAL;
	ret = t380_charge_inhibit_check(&client->dev);
	if (ret)
		return ret;
	/* Separate MUIC map and extcon; stock charger worker owns redetection. */
	if (of_device_is_compatible(client->dev.of_node,
				    "samsung,t380-sm5703-muic-observation")) {
		return t380_me_add(client);
	}
	gauge = of_device_is_compatible(client->dev.of_node,
				       "samsung,t380-sm5703-fg-diagnostic");
	if (client->addr != (gauge ? 0x71 : 0x49))
		return -EINVAL;
	if (!i2c_check_functionality(client->adapter, gauge ?
				    I2C_FUNC_SMBUS_WORD_DATA : I2C_FUNC_SMBUS_BYTE_DATA))
		return -EOPNOTSUPP;
	if (!gauge) {
        ret = t380_charge_safety_ready(&client->dev);
        if (!ret) {
            /* Resolve supplier lifetime before any inhibited board programming. */
            ret = t380_stock_dependencies_get(client, &dependencies);
            if (ret) return ret;
        } else if (ret != -EOPNOTSUPP) return ret;
		ret = t380_control_init(client);
		if (ret)
			return ret;
		ret = t380_control_read_serialized(client, SM5703_STATUS3);
		if (ret < 0)
			return ret;
		/* Verified nCHGEN high inhibits charging; CHGON may settle later. */
	}
	if (gauge) {
		ret = t380_battery_thermal_init(client);
		if (ret)
			return ret;
	}
	desc = gauge ? &t380_fg_desc : &t380_charger_desc;
	config.drv_data = client;
	config.of_node = client->dev.of_node;
	psy = devm_power_supply_register(&client->dev, desc, &config);
	if (IS_ERR(psy))
		return PTR_ERR(psy);
	if (!gauge) {

        if (dependencies) {
            ret = t380_stock_start(client, dependencies, psy);
            if (ret) return ret;
        }
	} else {
		ret = t380_pa_sysfs_add(client);
		if (ret)
			return ret;
	}
	dev_info(&client->dev, "automatic Samsung stock charging and measured telemetry\n");
	return 0;
}

static const struct of_device_id t380_power_match[] = {
	{ .compatible = "samsung,t380-sm5703-muic-observation" },
	{ .compatible = "samsung,t380-sm5703-diagnostic" },
	{ .compatible = "samsung,t380-sm5703-fg-diagnostic" },
	{ }
};

static struct i2c_driver t380_power_driver = {
	.driver = {
		.name = "t380-diagnostic-power",
		.of_match_table = t380_power_match,
		.suppress_bind_attrs = true,
		.pm = &t380_stock_pm,
	},
	.probe = t380_power_probe,
	.remove = t380_stock_remove,
	.shutdown = t380_stock_shutdown,
};

/* The early high-only GPIO owner must be ready before USB device initcall. */
static int __init t380_inhibit_init(void)
{
	return platform_driver_register(&t380_inhibit_driver);
}
subsys_initcall(t380_inhibit_init);

/*
 * TEMP registers a thermal zone. thermal_init is a subsys initcall, but
 * power/ is linked before thermal/: telemetry must use a later level.
 */
static int __init t380_power_init(void)
{
	return i2c_add_driver(&t380_power_driver);
}
device_initcall(t380_power_init);

MODULE_DESCRIPTION("T380 automatic stock-equivalent SM5703 charging");
MODULE_LICENSE("GPL");
