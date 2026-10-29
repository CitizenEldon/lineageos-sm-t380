/* SPDX-License-Identifier: GPL-2.0-only */
/* Read-only SM5703 source-pattern observation. No current/enable authority. */
#ifndef T380_MUIC_OBSERVATION_H
#define T380_MUIC_OBSERVATION_H
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#include <stdbool.h>
#endif

enum t380_mo_register {
	T380_MO_DEVID, T380_MO_ADC, T380_MO_DEVT1, T380_MO_DEVT2,
	T380_MO_DEVT3, T380_MO_RSVDID1, T380_MO_CHGTYPE, T380_MO_COUNT
};
/* No interrupts (read-to-clear), masks, switch/control/AFC or reset reads. */
static const unsigned char t380_mo_addresses[T380_MO_COUNT] = {
	0x01, 0x07, 0x0a, 0x0b, 0x15, 0x1d, 0x24
};
enum t380_mo_pattern {
	T380_MO_UNKNOWN, T380_MO_NONE_PATTERN, T380_MO_SDP_PATTERN,
	T380_MO_CDP_PATTERN, T380_MO_DCP_PATTERN,
	T380_MO_FIXED5V_TIMEOUT_PATTERN
};
struct t380_mo_observation {
	int valid, stable, vendor, revision, family_plausible;
	int raw[2][T380_MO_COUNT];
	int read_count, read_error, guard_before_error, guard_after_error;
	int failed_pass, failed_index, error;
	int adc_code, vbusin, vbusout, chgtype_code;
	enum t380_mo_pattern pattern;
};

static inline void t380_mo_clear(struct t380_mo_observation *out)
{
	struct t380_mo_observation empty = { 0 };
	int p, i;
	*out = empty;
	out->failed_pass = -1;
	out->failed_index = -1;
	for (p = 0; p < 2; ++p)
		for (i = 0; i < T380_MO_COUNT; ++i)
			out->raw[p][i] = -1;
}

/* Strict agreement only. Timeouts, accessories, reserved/mixed bits unknown. */
static inline int t380_mo_decode(struct t380_mo_observation *out)
{
	const int *r;
	int p, i;
	if (!out)
		return -EINVAL;
	out->valid = out->stable = 0;
	out->pattern = T380_MO_UNKNOWN;
	for (p = 0; p < 2; ++p)
		for (i = 0; i < T380_MO_COUNT; ++i)
			if (out->raw[p][i] < 0 || out->raw[p][i] > 255)
				return out->error = -EINVAL;
	r = out->raw[0];
	out->vendor = r[T380_MO_DEVID] & 0x07;
	out->revision = (r[T380_MO_DEVID] & 0xf8) >> 3;
	/* Stock reset table/vendor field: family evidence, not unique chip ID. */
	out->family_plausible = out->vendor == 1;
	if (!out->family_plausible)
		return out->error = -ENODEV;
	for (i = 0; i < T380_MO_COUNT; ++i)
		if (r[i] != out->raw[1][i])
			return out->error = -EAGAIN;
	out->stable = 1; /* Two equal sweeps; hardware age/atomicity unknown. */
	out->adc_code = r[T380_MO_ADC] & 0x1f;
	out->vbusin = !!(r[T380_MO_RSVDID1] & 0x02);
	out->vbusout = !!(r[T380_MO_RSVDID1] & 0x01);
	out->chgtype_code = r[T380_MO_CHGTYPE] & 0x1f;
	out->valid = 1;
	out->error = 0;
	if (r[T380_MO_ADC] != 0x1f || r[T380_MO_DEVT2] ||
	    (r[T380_MO_RSVDID1] & ~0x03))
		return 0;
	/* SM5703 DCD timeout is a nonstandard source, never BC1.2 DCP. */
	if (r[T380_MO_DEVT1] == 0 && r[T380_MO_DEVT3] == 4 &&
	    r[T380_MO_RSVDID1] == 3 && r[T380_MO_CHGTYPE] == 8) {
		out->pattern = T380_MO_FIXED5V_TIMEOUT_PATTERN;
		return 0;
	}
	if (r[T380_MO_DEVT3]) return 0;
	/* Stock USB data routing retains the charger VBUS route. VBUSOUT with
	 * VBUSIN is compatible with a sink, not proof of OTG/current permission. */
	if (!out->vbusin) {
		if (!out->vbusout && !r[T380_MO_DEVT1] && !r[T380_MO_CHGTYPE])
			out->pattern = T380_MO_NONE_PATTERN;
		return 0;
	}
	if (r[T380_MO_DEVT1] == 0x04 && r[T380_MO_CHGTYPE] == 0x04)
		out->pattern = T380_MO_SDP_PATTERN;
	else if (r[T380_MO_DEVT1] == 0x20 && r[T380_MO_CHGTYPE] == 0x02)
		out->pattern = T380_MO_CDP_PATTERN;
	else if (r[T380_MO_DEVT1] == 0x40 && r[T380_MO_CHGTYPE] == 0x01)
		out->pattern = T380_MO_DCP_PATTERN;
	return 0;
}

/* Observed settling tuple only. Never a source class or current allowance. */
static inline bool t380_mo_exact_detection_pending(const struct t380_mo_observation *o)
{
	static const int exact[][T380_MO_COUNT] = {
		{ 1, 31, 32, 0, 0, 3, 0 },
		{ 1, 31, 0, 0, 4, 3, 0 },
	};
	int tuple, pass, i;
	bool matches;
	if (!o || !o->valid || !o->stable || o->error || o->read_error ||
	    o->guard_before_error || o->guard_after_error ||
	    o->read_count != 2 * T380_MO_COUNT || o->pattern != T380_MO_UNKNOWN)
		return false;
	for (tuple = 0; tuple < 2; ++tuple) {
		matches = true;
		for (pass = 0; pass < 2; ++pass)
			for (i = 0; i < T380_MO_COUNT; ++i)
				if (o->raw[pass][i] != exact[tuple][i]) matches = false;
		if (matches) return true;
	}
	return false;
}
struct t380_mo_reader {
	void *context;
	int (*inhibit_check)(void *context);
	int (*read_byte)(void *context, unsigned char address);
};

static inline int t380_mo_acquire(const struct t380_mo_reader *reader,
				struct t380_mo_observation *out)
{
	int p, i, before, value, after;
	if (!out)
		return -EINVAL;
	t380_mo_clear(out);
	if (!reader || !reader->inhibit_check || !reader->read_byte)
		return out->error = -EINVAL;
	for (p = 0; p < 2; ++p) {
		for (i = 0; i < T380_MO_COUNT; ++i) {
			before = reader->inhibit_check(reader->context);
			if (before) {
				out->guard_before_error = before;
				out->failed_pass = p;
				out->failed_index = i;
				return out->error = before < 0 ? before : -EIO;
			}
			value = reader->read_byte(reader->context, t380_mo_addresses[i]);
			++out->read_count;
			/* Even failed reads get the after guard; preserve both errors. */
			after = reader->inhibit_check(reader->context);
			out->raw[p][i] = value;
			if (value < 0 || value > 255 || after) {
				out->failed_pass = p;
				out->failed_index = i;
				out->read_error = value < 0 ? value : (value > 255 ? -EIO : 0);
				out->guard_after_error = after;
				return out->error = out->read_error ? out->read_error :
					(after < 0 ? after : -EIO);
			}
			/* Fail early on incompatible family, retaining the raw ID. */
			if (i == T380_MO_DEVID && (value & 0x07) != 1) {
				out->failed_pass = p;
				out->failed_index = i;
				return out->error = -ENODEV;
			}
		}
	}
	return t380_mo_decode(out);
}
#endif
