/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_T380_ADC_THERMAL_H
#define _LINUX_T380_ADC_THERMAL_H
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/mutex.h>

#define T380_ADC_TABLE_MAX 64
struct t380_adc_point {
	u32 adc;
	s32 decicelsius;
};

static inline int t380_adc_validate_table(const struct t380_adc_point *table,
					 int count)
{
	int i;

	if (count < 2 || count > T380_ADC_TABLE_MAX)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		if (table[i].adc > 0xffff || table[i].decicelsius < -2730 ||
		    table[i].decicelsius > 20000)
			return -EINVAL;
		if (i && (table[i].adc <= table[i - 1].adc ||
			  table[i].decicelsius >= table[i - 1].decicelsius))
			return -EINVAL;
	}
	return 0;
}

/* Validated table only. Samsung signed deci-C interpolation/truncation. */
static inline int t380_adc_interpolate(const struct t380_adc_point *table,
				      int count, int adc, int *temp)
{
	s64 delta;
	int i;

	/* Never turn an out-of-calibration/saturated code into a valid endpoint. */
	if (adc < 0 || (u32)adc < table[0].adc ||
	    (u32)adc > table[count - 1].adc)
		return -ERANGE;
	for (i = 1; i < count; i++) {
		if ((u32)adc > table[i].adc)
			continue;
		delta = (s64)(table[i].decicelsius - table[i - 1].decicelsius) *
			(adc - table[i - 1].adc);
		*temp = table[i - 1].decicelsius +
			div_s64(delta, table[i].adc - table[i - 1].adc);
		return 0;
	}
	return -ERANGE;
}

/* Samsung trimmed mean: remove one minimum and maximum, including ties. */
static inline int t380_adc_read(struct iio_channel *channel, struct mutex *lock,
			       int samples, int *adc)
{
	int i, ret, val, total = 0, low = 0xffff, high = 0;

	if (samples < 3 || samples > 7)
		return -EINVAL;
	mutex_lock(lock);
	for (i = 0; i < samples; i++) {
		ret = iio_read_channel_raw(channel, &val);
		if (ret < 0)
			goto out;
		if (ret != IIO_VAL_INT) {
			ret = -EINVAL;
			goto out;
		}
		if (val < 0 || val > 0xffff) {
			ret = -ERANGE;
			goto out;
		}
		low = min(low, val);
		high = max(high, val);
		total += val;
	}
	*adc = (total - low - high) / (samples - 2);
	ret = 0;
out:
	mutex_unlock(lock);
	return ret;
}
#endif
