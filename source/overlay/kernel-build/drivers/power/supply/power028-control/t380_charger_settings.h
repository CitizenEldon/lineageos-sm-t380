/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_CHARGER_SETTINGS_H
#define T380_CHARGER_SETTINGS_H

/* Pure integer calculations only. No bus, GPIO, mode or authorization API. */
enum t380_cs_register {
    T380_CS_CNTL, T380_CS_VBUSCNTL, T380_CS_CHGCNTL2,
    T380_CS_CHGCNTL3, T380_CS_CHGCNTL4, T380_CS_CHGCNTL5,
    T380_CS_CHGCNTL6, T380_CS_COUNT
};

struct t380_cs_limits {
    int input_ma, charge_ma, float_mv, topoff_ma;
};

struct t380_cs_field {
    int address, mask, value;
};

/* Logical setting phase only; no cable event, reset or authorization API.
 * Stock init/unplug uses board AUTOSET1; managed attachment uses AUTOSET0. */
enum t380_cs_profile {
    T380_CS_BOARD_INIT = 0,
    T380_CS_MANAGED_ATTACHED = 1,
    T380_CS_STOCK_ATTACHED = 2
};

struct t380_cs_plan {
    int valid; /* Numerically valid plan; not runtime charging permission. */
    enum t380_cs_profile profile;
    struct t380_cs_limits requested, effective;
    struct t380_cs_field fields[T380_CS_COUNT];
};

struct t380_cs_decoded {
    int valid;
    struct t380_cs_limits stock_arithmetic;
    int input_code, fast_code, fast_in_stock_setter_range;
    int autoset, autostop, aiclen;
    int aicl_code, aicl_supported, aicl_mv, topoff_minutes, frequency_khz;
};

struct t380_cs_verification {
    int valid; /* All requested masked fields match; no health/source verdict. */
    unsigned int mismatch; /* Bit i identifies T380_CS register i. */
    struct t380_cs_decoded observed;
};

/* Conservative numeric envelope: input/charge100..2100mA, float4120..4300mV,
 * topoff100..350mA in EXACT25mA steps. Floor input/charge/float steps within
 * that envelope; reject other values. Lowering topoff can delay termination,
 * so never quantize that threshold. The old entry point preserves BOARD_INIT;
 * runtime policy plans explicitly select MANAGED_ATTACHED. Other config is fixed. */
int t380_cs_encode(const struct t380_cs_limits *request, struct t380_cs_plan *plan);
int t380_cs_encode_profile(const struct t380_cs_limits *request,
                          enum t380_cs_profile profile, struct t380_cs_plan *plan);

/* Pure masked merge into caller-supplied bytes; not a write transaction/order. */
int t380_cs_merge(const struct t380_cs_plan *plan, const int old[T380_CS_COUNT],
                  int merged[T380_CS_COUNT]);

/* Decode exact stock arithmetic, including numerically higher inherited codes.
 * Such decoding proves neither silicon validity nor permitted current. */
int t380_cs_decode(const int raw[T380_CS_COUNT], struct t380_cs_decoded *decoded);

/* Require exact match of every planned masked bit. Raw negative entries are
 * preserved errnos; >255 is invalid. No partial success or cached validity. */
int t380_cs_verify(const struct t380_cs_plan *plan, const int raw[T380_CS_COUNT],
                   struct t380_cs_verification *verification);
#endif
