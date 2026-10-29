// SPDX-License-Identifier: GPL-2.0-only
/* Samsung SM-T380 case telemetry; not a validated protection controller. */
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/t380_adc_thermal.h>

#define T380_CASE_SAMPLES 7
#define T380_CASE_TABLE_MAX T380_ADC_TABLE_MAX

struct t380_case_thermal {
	struct iio_channel *channel;
	struct mutex lock;
	struct t380_adc_point *table;
	int count;
};

static int t380_case_validate_table(const struct t380_adc_point *table,
				    int count)
{
	return t380_adc_validate_table(table, count);
}

/* Keep Samsung's deci-C interpolation/truncation, then convert to milli-C. */
static int t380_case_interpolate(struct t380_case_thermal *case_therm,
				int adc, int *temp)
{
	int decicelsius, ret;

	ret = t380_adc_interpolate(case_therm->table, case_therm->count,
				   adc, &decicelsius);
	if (!ret)
		*temp = decicelsius * 100;
	return ret;
}

/* As in Samsung sec_thermistor: seven raw reads, discard min and max. */
static int t380_case_read_adc(struct t380_case_thermal *case_therm, int *adc)
{
	return t380_adc_read(case_therm->channel, &case_therm->lock,
			     T380_CASE_SAMPLES, adc);
}

static int t380_case_get_temp(void *data, int *temp)
{
	struct t380_case_thermal *case_therm = data;
	int adc, ret;

	ret = t380_case_read_adc(case_therm, &adc);
	if (ret)
		return ret;
	return t380_case_interpolate(case_therm, adc, temp);
}

static const struct thermal_zone_of_device_ops t380_case_ops = {
	.get_temp = t380_case_get_temp,
};

static ssize_t raw_adc_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	int adc, ret;

	ret = t380_case_read_adc(dev_get_drvdata(dev), &adc);
	if (ret)
		return ret;
	return scnprintf(buf, PAGE_SIZE, "%d\n", adc);
}
static DEVICE_ATTR_RO(raw_adc);

static struct attribute *t380_case_attrs[] = {
	&dev_attr_raw_adc.attr,
	NULL,
};
static const struct attribute_group t380_case_group = {
	.attrs = t380_case_attrs,
};

static int t380_case_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct thermal_zone_device *zone;
	struct t380_case_thermal *case_therm;
	u32 value;
	int count, i, ret;

	count = of_property_count_u32_elems(np, "samsung,adc-codes");
	if (count < 2 || count > T380_CASE_TABLE_MAX ||
	    count != of_property_count_u32_elems(np,
				"samsung,temperatures-decicelsius"))
		return -EINVAL;
	case_therm = devm_kzalloc(dev, sizeof(*case_therm), GFP_KERNEL);
	if (!case_therm)
		return -ENOMEM;
	case_therm->table = devm_kcalloc(dev, count,
				       sizeof(*case_therm->table), GFP_KERNEL);
	if (!case_therm->table)
		return -ENOMEM;
	case_therm->count = count;
	for (i = 0; i < count; i++) {
		ret = of_property_read_u32_index(np, "samsung,adc-codes", i,
						&case_therm->table[i].adc);
		if (ret)
			return ret;
		ret = of_property_read_u32_index(np,
			"samsung,temperatures-decicelsius", i, &value);
		if (ret)
			return ret;
		case_therm->table[i].decicelsius = (s32)value;
	}
	ret = t380_case_validate_table(case_therm->table, count);
	if (ret)
		return ret;
	case_therm->channel = devm_iio_channel_get(dev, "case-thermistor");
	if (IS_ERR(case_therm->channel))
		return PTR_ERR(case_therm->channel);
	mutex_init(&case_therm->lock);
	platform_set_drvdata(pdev, case_therm);
	zone = devm_thermal_zone_of_sensor_register(dev, 0, case_therm,
						  &t380_case_ops);
	if (IS_ERR(zone))
		return PTR_ERR(zone);
	return devm_device_add_group(dev, &t380_case_group);
}

static const struct of_device_id t380_case_match[] = {
	{ .compatible = "samsung,t380-case-thermal" },
	{ }
};
MODULE_DEVICE_TABLE(of, t380_case_match);

static struct platform_driver t380_case_driver = {
	.probe = t380_case_probe,
	.driver = {
		.name = "t380-case-thermal",
		.of_match_table = t380_case_match,
	},
};
module_platform_driver(t380_case_driver);
MODULE_DESCRIPTION("Samsung SM-T380 raw ADC case temperature telemetry");
MODULE_LICENSE("GPL v2");
