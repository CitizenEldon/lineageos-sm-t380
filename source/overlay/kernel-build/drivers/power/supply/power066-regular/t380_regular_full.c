/* SPDX-License-Identifier: GPL-2.0-only */
#include "t380_regular_full.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif
void t380_rf_capacity_init(struct t380_rf_capacity *s)
{
 if (s) *s = (struct t380_rf_capacity){ .capacity_max = 1000 };
}
int t380_rf_raw_tenths(int word)
{
 if (word < 0) return word;
 if (word > 65535) return -ERANGE;
 /* Samsung sm5703_get_soc: unsigned 8.8 word -> tenths of percent. */
 return ((word & 0xff00) >> 8) * 10 + (word & 0xff) * 10 / 256;
}
int t380_rf_check_capacity_max(int value)
{
 /* Preserve stock's asymmetric strict lower bound, including 920. */
 if (value < 920) return 930;
 if (value > 1070) return 1070;
 return value;
}
int t380_rf_dynamic_scale(struct t380_rf_capacity *s, int raw, int capacity)
{
 int maximum;
 if (!s || raw < 0 || raw > 2559 || capacity < 0 || capacity > 100)
  return -EINVAL;
 maximum = raw < 930 ? 930 : (raw > 1070 ? 1070 : raw);
 maximum = capacity != 100 ?
  t380_rf_check_capacity_max(maximum * 100 / (capacity + 1)) :
  maximum * 99 / 100;
 s->capacity_max = maximum;
 s->capacity_old = capacity;
 ++s->dynamic_updates;
 return maximum;
}
bool t380_rf_capacity_update(struct t380_rf_capacity *s, int word,
 t380_rf_u64 now_ms, t380_rf_u64 sample_ms, bool charging, bool full_candidate)
{
 int raw, target, scaled;
 if (!s || sample_ms > now_ms) return false;
 raw = t380_rf_raw_tenths(word);
 if (raw < 0) { s->valid = false; return false; }
 if (s->initialized && (now_ms < s->updated_ms || sample_ms < s->sample_ms)) {
  s->valid = false; return false;
 }
 if (s->initialized && (sample_ms == s->sample_ms ||
     now_ms - s->updated_ms < T380_RF_CAPACITY_PERIOD_MS)) return false;
 if (!s->initialized && raw > 1000)
  t380_rf_dynamic_scale(s, raw, 100); /* Exact stock probe branch. */
 scaled = raw * 1000 / s->capacity_max;
 if (scaled > 1000) scaled = 1000;
 target = scaled / 10;
 if (!s->initialized) s->capacity_old = target;
 else {
  if (s->capacity_old < target) target = s->capacity_old + 1;
  else if (s->capacity_old > target) target = s->capacity_old - 1;
  if (!charging && s->capacity_old < target) target = s->capacity_old;
  s->capacity_old = target;
 }
 s->raw_tenths = raw; s->scaled_tenths = scaled;
 s->updated_ms = now_ms; s->sample_ms = sample_ms;
 s->valid = s->initialized = true; ++s->updates;
 /* Stock fullcheck first retains the candidate below100, then sets
  * CHARGE_FULL(displayed capacity). Learning never requires100 beforehand. */
 if (full_candidate && charging && s->capacity_old >= T380_RF_FULL_SCALED_SOC)
  t380_rf_dynamic_scale(s, raw, s->capacity_old);
 return true;
}
/* Exact sm5703_fuelgauge_impl.h addresses and Rev05 battery0 DT values.
 * Rows13..15 retain both adjacent entries and the profile endpoint.
 * DEVICE_ID is recorded only: stock prints it without a model-ID predicate.
 * CURR_CAL upperbyte is dynamic stock temperature/charge compensation. */
const unsigned char t380_rf_model_registers[T380_RF_MODEL_REGS] = {
 0x00, 0x01, 0x10, 0x13, 0x2b, 0x2c,
 0xad, 0xae, 0xaf, 0xbd, 0xbe, 0xbf, 0xcd, 0xce, 0xcf
};
static const unsigned int rf_expected[T380_RF_MODEL_REGS] = {
 0, 0xc480, 0x0007, 0x000f, 0x809e, 0x0085,
 0x210d, 0x2202, 0x2400, 0x210d, 0x2202, 0x2400,
 0x218a, 0x25f0, 0x2604
};
static const unsigned int rf_masks[T380_RF_MODEL_REGS] = {
 0, 0xe4e0, 0x00ff, 0xffff, 0xffff, 0x00ff,
 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
 0xffff, 0xffff, 0xffff
};
static int rf_model_guard(const struct t380_rf_model_io *io,
                          struct t380_rf_model *w)
{
 t380_rf_u64 now;
 int ret = io->guard(io->context);
 if (ret) return ret;
 now = io->now(io->context);
 if (now < w->started_ms || now - w->started_ms > T380_RF_MODEL_MAX_MS ||
     io->epoch(io->context) != w->epoch || !w->epoch) return -ESTALE;
 return 0;
}
void t380_rf_model_init(struct t380_rf_model *w)
{
 int i;
 if (!w) return;
 *w = (struct t380_rf_model){ .error = -ENODATA, .failed_index = -1,
  .status_before = -ENODATA, .status_after = -ENODATA,
  .primer_raw = -ENODATA, .primer_error = -ENODATA, .aux_failed_index = -1 };
 for (i = 0; i < T380_RF_MODEL_AUX_REGS; ++i) {
  w->aux_raw[i] = w->aux_error[i] = -ENODATA;
 }
 for (i = 0; i < T380_RF_MODEL_REGS; ++i) w->raw[i] = -ENODATA;
}
const unsigned char t380_rf_model_aux_registers[T380_RF_MODEL_AUX_REGS] = {
 0xac, 0x01, 0xbc, 0x05, 0xcc
};
/* Preliminary payloads have no predicate role. Their transport and guards
 * remain fatal, within the original whole-sequence deadline and epoch. */
static int rf_model_aux(const struct t380_rf_model_io *io,
                        struct t380_rf_model *w, int index)
{
 int ret;
 w->aux_failed_index = index;
 w->aux_stage[index] = T380_RF_PRIMER_PRE_GUARD;
 ret = rf_model_guard(io, w);
 if (!ret) {
  w->aux_stage[index] = T380_RF_PRIMER_READ;
  ++w->aux_reads;
  w->aux_raw[index] = io->word(io->context, t380_rf_model_aux_registers[index]);
  ret = w->aux_raw[index] < 0 ? w->aux_raw[index] :
   w->aux_raw[index] > 65535 ? -ERANGE : 0;
 }
 if (!ret) {
  w->aux_stage[index] = T380_RF_PRIMER_POST_GUARD;
  ret = rf_model_guard(io, w);
 }
 if (!ret) w->aux_stage[index] = T380_RF_PRIMER_DONE;
 w->aux_error[index] = ret;
 if (index == 0) {
  w->primer_stage = w->aux_stage[0];
  w->primer_attempted = w->aux_stage[0] >= T380_RF_PRIMER_READ;
  w->primer_reads = w->primer_attempted ? 1 : 0;
  w->primer_raw = w->aux_raw[0]; w->primer_error = ret;
 }
 if (!ret) w->aux_failed_index = -1;
 return ret;
}
int t380_rf_model_check(const struct t380_rf_model_io *io,
                       struct t380_rf_model *w)
{
 int ret, i;
 if (!w) return -EINVAL;
 t380_rf_model_init(w);
 if (!io || !io->guard || !io->word || !io->status || !io->now || !io->epoch)
  return w->error = -EINVAL;
 w->started_ms = io->now(io->context); w->epoch = io->epoch(io->context);
 ret = rf_model_guard(io, w);
 if (!ret) {
  w->status_before = io->status(io->context);
  ret = w->status_before < 0 ? w->status_before :
   w->status_before > 255 || (w->status_before & 1) ? -EIO : 0;
 }
 for (i = 0; !ret && i < T380_RF_MODEL_REGS; ++i) {
  if (i == 6) {
   ret = rf_model_aux(io, w, 0);
   if (ret) {w->failed_index = T380_RF_MODEL_PRIMER_FAILED_INDEX; break;}
  }
  if (i == 9 || i == 12) {
   int auxiliary = i == 9 ? 1 : 3;
   ret = rf_model_aux(io, w, auxiliary);
   if (!ret) ret = rf_model_aux(io, w, auxiliary + 1);
   if (ret) {w->failed_index = -2 - w->aux_failed_index; break;}
  }
  ret = rf_model_guard(io, w);
  if (ret) break;
  w->failed_index = i; ++w->reads;
  w->raw[i] = io->word(io->context, t380_rf_model_registers[i]);
  if (w->raw[i] < 0) ret = w->raw[i];
  else if (w->raw[i] > 65535) ret = -ERANGE;
  else if (((unsigned int)w->raw[i] & rf_masks[i]) != rf_expected[i])
   ret = -EXDEV;
  if (!ret) ret = rf_model_guard(io, w);
 }
 if (!ret) {
  w->status_after = io->status(io->context);
  ret = w->status_after < 0 ? w->status_after :
   w->status_after > 255 || (w->status_after & 1) ? -EIO : 0;
 }
 if (!ret) ret = rf_model_guard(io, w);
 w->ended_ms = io->now(io->context);
 if (!ret && (w->ended_ms < w->started_ms ||
     w->ended_ms - w->started_ms > T380_RF_MODEL_MAX_MS)) ret = -ESTALE;
 if (!ret) w->failed_index = -1;
 w->error = ret; w->matched = !ret;
 return ret;
}

const unsigned char t380_rf_diagnostic_registers[T380_RF_DIAGNOSTIC_REGS] = {
 0xac, 0xad, 0xae, 0xaf, 0x01, 0xbc, 0xbd, 0xbe, 0xbf, 0x05,
 0xcc, 0xcd, 0xce, 0xcf, 0x2e, 0x85, 0x86, 0x87, 0x28, 0x13
};
void t380_rf_diagnostic_init(struct t380_rf_diagnostic *w)
{
 int i;
 if (!w) return;
 *w = (struct t380_rf_diagnostic){ .error = -ENODATA, .failed_read = -1,
  .status_before = -ENODATA, .status_after = -ENODATA,
  .status_before_error = -ENODATA, .status_after_error = -ENODATA };
 for (i = 0; i < T380_RF_DIAGNOSTIC_REGS; ++i) {
  w->address[i] = t380_rf_diagnostic_registers[i];
  w->raw[i] = w->read_error[i] = -ENODATA;
 }
}
static int rf_diagnostic_guard(const struct t380_rf_model_io *io,
                               const struct t380_rf_diagnostic *w)
{
 t380_rf_u64 now;
 int ret = io->guard(io->context);
 if (ret) return ret;
 now = io->now(io->context);
 if (now < w->started_ms || now - w->started_ms > T380_RF_MODEL_MAX_MS ||
     !w->epoch || io->epoch(io->context) != w->epoch) return -ESTALE;
 return 0;
}
/* No model predicate, capacity, policy, admission or permission state. */
int t380_rf_diagnostic_run(const struct t380_rf_model_io *io,
                           struct t380_rf_diagnostic *w,
                           t380_rf_u64 run_sequence)
{
 int ret, i;
 if (!w) return -EINVAL;
 t380_rf_diagnostic_init(w); w->run_sequence = run_sequence;
 if (!run_sequence || !io || !io->guard || !io->word || !io->status ||
     !io->now || !io->epoch) return w->error = -EINVAL;
 w->started_ms = io->now(io->context); w->epoch = io->epoch(io->context);
 w->stage = T380_RF_DIAG_INITIAL_PRE;
 ret = rf_diagnostic_guard(io, w);
 if (!ret) {
  w->stage = T380_RF_DIAG_INITIAL_STATUS;
  w->status_before = io->status(io->context);
  ret = w->status_before < 0 ? w->status_before :
   w->status_before > 255 || (w->status_before & 1) ? -EIO : 0;
  if (!ret) {
   w->stage = T380_RF_DIAG_INITIAL_POST;
   ret = rf_diagnostic_guard(io, w);
  }
 }
 w->status_before_error = ret;
 for (i = 0; !ret && i < T380_RF_DIAGNOSTIC_REGS; ++i) {
  w->failed_read = i; w->stage = T380_RF_DIAG_READ_PRE;
  ret = rf_diagnostic_guard(io, w);
  if (!ret) {
   w->stage = T380_RF_DIAG_READ;
   w->read_start_ms[i] = io->now(io->context); ++w->attempted;
   w->raw[i] = io->word(io->context, w->address[i]);
   w->read_end_ms[i] = io->now(io->context);
   ret = w->raw[i] < 0 ? w->raw[i] : w->raw[i] > 65535 ? -ERANGE : 0;
   if (!ret) {
    w->stage = T380_RF_DIAG_READ_POST;
    ret = rf_diagnostic_guard(io, w);
   }
  }
  w->read_error[i] = ret;
 }
 if (!ret) {
  w->failed_read = -1; w->stage = T380_RF_DIAG_FINAL_PRE;
  ret = rf_diagnostic_guard(io, w);
  if (!ret) {
   w->stage = T380_RF_DIAG_FINAL_STATUS;
   w->status_after = io->status(io->context);
   ret = w->status_after < 0 ? w->status_after :
    w->status_after > 255 || (w->status_after & 1) ? -EIO : 0;
   if (!ret) {
    w->stage = T380_RF_DIAG_FINAL_POST;
    ret = rf_diagnostic_guard(io, w);
   }
  }
  w->status_after_error = ret;
 }
 w->ended_ms = io->now(io->context);
 if (!ret && (w->ended_ms < w->started_ms ||
     w->ended_ms - w->started_ms > T380_RF_MODEL_MAX_MS)) ret = -ESTALE;
 if (!ret) {w->stage = T380_RF_DIAG_DONE; w->complete = true;}
 w->error = ret;
 return ret;
}
