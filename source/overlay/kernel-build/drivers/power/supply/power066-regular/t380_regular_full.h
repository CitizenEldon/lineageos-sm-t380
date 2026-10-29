/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_REGULAR_FULL_H
#define T380_REGULAR_FULL_H
#ifdef __KERNEL__
#include <linux/types.h>
typedef u64 t380_rf_u64;
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t t380_rf_u64;
#endif
#define T380_RF_PROFILE "stock_default_age_unknown4300_4230"
#define T380_RF_FLOAT_MV 4300
#define T380_RF_RECHARGE_MV 4230
#define T380_RF_FULL_MIN_MV 4200
#define T380_RF_FULL_SCALED_SOC 93
#define T380_RF_INITIAL_UPPER_MV 4280
#define T380_RF_CAPACITY_PERIOD_MS 30000ULL
/* Rev05 stock default, not a measured age row. No gauge writes or persistence. */
struct t380_rf_capacity {
 bool initialized, valid;
 int capacity_max, capacity_old, raw_tenths, scaled_tenths;
 t380_rf_u64 updated_ms, sample_ms;
 unsigned int updates, dynamic_updates;
};
void t380_rf_capacity_init(struct t380_rf_capacity *);
int t380_rf_raw_tenths(int word);
int t380_rf_check_capacity_max(int value);
int t380_rf_dynamic_scale(struct t380_rf_capacity *, int raw, int capacity);
bool t380_rf_capacity_update(struct t380_rf_capacity *, int word,
 t380_rf_u64 now_ms, t380_rf_u64 sample_ms, bool charging, bool full_candidate);
/* Fixed Rev05 row0 endpoint+adjacent/model witness, not arbitrary register ABI. */
#define T380_RF_MODEL_REGS 15
#define T380_RF_MODEL_AUX_REGS 5
#define T380_RF_DIAGNOSTIC_REGS 20
#define T380_RF_MODEL_PRIMER_FAILED_INDEX (-2)
#define T380_RF_MODEL_MAX_MS 500ULL /* existing owner fresh-sample bound */
enum t380_rf_primer_stage { T380_RF_PRIMER_UNATTEMPTED,
 T380_RF_PRIMER_PRE_GUARD, T380_RF_PRIMER_READ,
 T380_RF_PRIMER_POST_GUARD, T380_RF_PRIMER_DONE };
struct t380_rf_model {
 bool matched, primer_attempted;
 int primer_raw, primer_error, primer_stage, primer_reads;
 int error, failed_index, reads, status_before, status_after;
 int raw[T380_RF_MODEL_REGS];
 int aux_raw[T380_RF_MODEL_AUX_REGS], aux_error[T380_RF_MODEL_AUX_REGS];
 int aux_stage[T380_RF_MODEL_AUX_REGS], aux_reads, aux_failed_index;
 t380_rf_u64 started_ms, ended_ms, epoch;
};
struct t380_rf_model_io {
 void *context;
 int (*guard)(void *);
 int (*word)(void *, unsigned char);
 int (*status)(void *);
 t380_rf_u64 (*now)(void *);
 t380_rf_u64 (*epoch)(void *);
};
void t380_rf_model_init(struct t380_rf_model *);
extern const unsigned char t380_rf_model_registers[T380_RF_MODEL_REGS];
int t380_rf_model_check(const struct t380_rf_model_io *, struct t380_rf_model *);

/* Observation-only witness: never used by admission or capacity policy. */
enum t380_rf_diagnostic_stage { T380_RF_DIAG_UNATTEMPTED,
 T380_RF_DIAG_INITIAL_PRE, T380_RF_DIAG_INITIAL_STATUS,
 T380_RF_DIAG_INITIAL_POST, T380_RF_DIAG_READ_PRE, T380_RF_DIAG_READ,
 T380_RF_DIAG_READ_POST, T380_RF_DIAG_FINAL_PRE,
 T380_RF_DIAG_FINAL_STATUS, T380_RF_DIAG_FINAL_POST, T380_RF_DIAG_DONE };
struct t380_rf_diagnostic {
 bool complete;
 int error, failed_read, stage, attempted;
 int status_before, status_after, status_before_error, status_after_error;
 unsigned char address[T380_RF_DIAGNOSTIC_REGS];
 int raw[T380_RF_DIAGNOSTIC_REGS], read_error[T380_RF_DIAGNOSTIC_REGS];
 t380_rf_u64 read_start_ms[T380_RF_DIAGNOSTIC_REGS];
 t380_rf_u64 read_end_ms[T380_RF_DIAGNOSTIC_REGS];
 t380_rf_u64 run_sequence, started_ms, ended_ms, epoch;
};
extern const unsigned char t380_rf_model_aux_registers[T380_RF_MODEL_AUX_REGS];
extern const unsigned char t380_rf_diagnostic_registers[T380_RF_DIAGNOSTIC_REGS];
void t380_rf_diagnostic_init(struct t380_rf_diagnostic *);
int t380_rf_diagnostic_run(const struct t380_rf_model_io *,
 struct t380_rf_diagnostic *, t380_rf_u64 run_sequence);

#endif
