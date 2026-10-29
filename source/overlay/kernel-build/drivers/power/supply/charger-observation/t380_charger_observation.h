/* SPDX-License-Identifier: GPL-2.0-only */
/* Pure, read-only SM5703 diagnostic acquisition/decoding. Not charge policy. */
#ifndef T380_CHARGER_OBSERVATION_H
#define T380_CHARGER_OBSERVATION_H
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stdbool.h>
#endif

enum t380_co_register {
	T380_CO_STATUS3, T380_CO_STATUS1, T380_CO_STATUS2, T380_CO_STATUS5,
	T380_CO_CNTL, T380_CO_VBUSCNTL, T380_CO_CHGCNTL2,
	T380_CO_CHGCNTL3, T380_CO_CHGCNTL4, T380_CO_CHGCNTL5,
	T380_CO_CHGCNTL6, T380_CO_COUNT
};

/* Explicit allowlist: never INT1..4, masks, gauge, MUIC or other functions. */
static const unsigned char t380_co_addresses[T380_CO_COUNT] = {
	0x0a, 0x08, 0x09, 0x6b, 0x0c, 0x0d, 0x0f, 0x10, 0x11, 0x12, 0x13
};

struct t380_co_observation {
	int valid;
	/* Internal provenance set only by the verified household guard. */
	bool guard_cancelled;
	int raw[T380_CO_COUNT];
	int chgon, topoff, done, aicl_active, otgfail;
	int vbusok, vbusovp, vbusuvlo, vbus_conflict;
	int operation_mode_code, operation_mode_supported;
	int autoset, autostop, aiclen;
	int input_limit_code, input_limit_stock_ma;
	int fast_current_code, fast_current_in_setter_range, fast_current_stock_ma;
	int float_voltage_mv, topoff_current_ma;
	int aicl_threshold_code, aicl_threshold_supported, aicl_threshold_mv;
	int topoff_timer_minutes, frequency_khz;
};

/* A disconnected, inhibited sink reports UVLO in the observed SM5703
 * status. That is evidence of absence, not a charger fault. Never make the
 * exception while active, CHGON, overvoltage or a contradictory status. */
static inline bool t380_co_expected_absent_uvlo(
        const struct t380_co_observation *o, bool active)
{
    return o && !active && o->valid && !o->vbusok && o->vbusuvlo &&
        !o->chgon && !o->otgfail && !o->vbusovp && !o->vbus_conflict;
}

/* Returns arithmetic decoding, never a healthy-cell/source/enable verdict. */
static inline int t380_co_decode(const int raw[T380_CO_COUNT],
				struct t380_co_observation *out)
{
	static const int timers[] = { 10, 20, 30, 45 };
	static const int frequencies[] = { 3000, 2400, 1500, 1800 };
	struct t380_co_observation decoded = { 0 };
	int i, value;

	if (!out)
		return -EINVAL;
	*out = decoded;
	if (!raw)
		return -EINVAL;
	for (i = 0; i < T380_CO_COUNT; ++i) {
		if (raw[i] < 0 || raw[i] > 255)
			return -EINVAL;
		decoded.raw[i] = raw[i];
	}
	decoded.chgon = !!(raw[T380_CO_STATUS3] & 0x01);
	decoded.topoff = !!(raw[T380_CO_STATUS3] & 0x04);
	decoded.done = !!(raw[T380_CO_STATUS3] & 0x08);
	decoded.aicl_active = !!(raw[T380_CO_STATUS1] & 0x01);
	decoded.otgfail = !!(raw[T380_CO_STATUS1] & 0x40);
	decoded.vbusok = !!(raw[T380_CO_STATUS5] & 0x20);
	decoded.vbusuvlo = !!(raw[T380_CO_STATUS5] & 0x40);
	decoded.vbusovp = !!(raw[T380_CO_STATUS5] & 0x80);
	value = decoded.vbusok + decoded.vbusuvlo + decoded.vbusovp;
	decoded.vbus_conflict = value > 1;
	decoded.operation_mode_code = raw[T380_CO_CNTL] & 0x07;
	value = decoded.operation_mode_code;
	decoded.operation_mode_supported = value == 0 || value >= 4;
	decoded.autoset = !!(raw[T380_CO_CNTL] & 0x10);
	decoded.autostop = !!(raw[T380_CO_CHGCNTL4] & 0x80);
	decoded.aiclen = !!(raw[T380_CO_CHGCNTL5] & 0x80);
	decoded.input_limit_code = raw[T380_CO_VBUSCNTL] & 0x3f;
	/* Exact stock getter arithmetic; silicon range/board allowance unproven. */
	decoded.input_limit_stock_ma = 100 + decoded.input_limit_code * 50;
	decoded.fast_current_code = raw[T380_CO_CHGCNTL2] & 0x3f;
	decoded.fast_current_in_setter_range = decoded.fast_current_code <= 48;
	decoded.fast_current_stock_ma = 100 + decoded.fast_current_code * 50;
	decoded.float_voltage_mv = 4120 + (raw[T380_CO_CHGCNTL3] & 0x1f) * 10;
	decoded.topoff_current_ma = 100 + ((raw[T380_CO_CHGCNTL4] & 0x78) >> 3) * 25;
	decoded.aicl_threshold_code = raw[T380_CO_CHGCNTL5] & 0x07;
	decoded.aicl_threshold_supported = decoded.aicl_threshold_code <= 6;
	decoded.aicl_threshold_mv = decoded.aicl_threshold_supported ?
		4300 + decoded.aicl_threshold_code * 100 : -1;
	decoded.topoff_timer_minutes = timers[(raw[T380_CO_CHGCNTL5] & 0x60) >> 5];
	decoded.frequency_khz = frequencies[(raw[T380_CO_CHGCNTL6] & 0x0c) >> 2];
	decoded.valid = 1;
	*out = decoded;
	return 0;
}

/* Callback surface deliberately has no write/GPIO/enable operation. */
struct t380_co_reader {
	void *context;
	int (*inhibit_check)(void *context);
	int (*read_byte)(void *context, unsigned char address);
};

static inline int t380_co_acquire_mode(const struct t380_co_reader *reader,
				 struct t380_co_observation *out, bool active)
{
	struct t380_co_observation empty = { 0 };
	int raw[T380_CO_COUNT], i, ret, first_status;

	if (!out)
		return -EINVAL;
	*out = empty; /* No stale/partial snapshot survives any failure. */
	if (!reader || !reader->inhibit_check || !reader->read_byte)
		return -EINVAL;
	ret = reader->inhibit_check(reader->context);
	if (ret) {
		out->guard_cancelled = ret == -ECANCELED;
		return ret < 0 ? ret : -EIO;
	}
	for (i = 0; i < T380_CO_COUNT; ++i) {
		ret = reader->read_byte(reader->context, t380_co_addresses[i]);
		if (ret < 0)
			return ret;
		if (ret > 255)
			return -EIO;
		raw[i] = ret;
		if (i == T380_CO_STATUS3 && !active && (ret & 0x01))
			return -EIO;
	}
	first_status = raw[T380_CO_STATUS3];
	/* Recheck CHGON after other reads; this is still not an atomic sample. */
	ret = reader->read_byte(reader->context, t380_co_addresses[T380_CO_STATUS3]);
	if (ret < 0)
		return ret;
	if (ret > 255 || (!active && (ret & 0x01)) ||
	    (active && ((ret ^ first_status) & 0x01)))
		return -EIO;
	raw[T380_CO_STATUS3] = ret;
	ret = reader->inhibit_check(reader->context);
	if (ret) {
		out->guard_cancelled = ret == -ECANCELED;
		return ret < 0 ? ret : -EIO;
	}
	return t380_co_decode(raw, out);
}
static inline int t380_co_acquire(const struct t380_co_reader *reader,
				 struct t380_co_observation *out)
{
	return t380_co_acquire_mode(reader, out, false);
}
#endif
