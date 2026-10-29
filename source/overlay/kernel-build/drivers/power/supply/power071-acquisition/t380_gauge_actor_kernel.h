/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_GAUGE_ACTOR_KERNEL_H
#define T380_GAUGE_ACTOR_KERNEL_H
#include <linux/completion.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/t380_gauge_trace.h>
#include <linux/workqueue.h>

#define T380_GA_TRACE_RECORDS 256
#define T380_GA_GUARD_RECORDS 64
struct t380_ga_guard_record {
 u64 begin_ns, end_ns;
 struct t380_sl_epoch_record before, after;
 int kind, result;
};
enum t380_ga_cancel_reason { T380_GA_CANCEL_NONE, T380_GA_CANCEL_DISABLE, T380_GA_CANCEL_PM, T380_GA_CANCEL_STOP, T380_GA_CANCEL_REMOVE, T380_GA_CANCEL_TIMEOUT };
struct t380_ga_outcome { u64 request, lifetime; int cancel_reason, physical_error; };
enum t380_ga_kind { T380_GA_TUPLE, T380_GA_MODEL, T380_GA_STOCK, T380_GA_SCRATCH, T380_GA_ANDROID };
static void t380_ga_epoch_copy(struct t380_sl_epoch_record *e)
{
 unsigned long flags;
 spin_lock_irqsave(&t380_source_lock, flags);
 e->generation = t380_source_state.generation;
 e->pm_serial = t380_source_state.pm_serial;
 e->fault_serial = t380_source_state.fault_serial;
 e->invalidated_ms = t380_source_state.invalidated_ms;
 e->last_invalidation = t380_source_state.last_invalidation;
 e->system_suspended = t380_source_state.system_suspended;
 e->bus_suspended = t380_source_state.bus_suspended;
 e->sampled_ms = t380_source_now();
 spin_unlock_irqrestore(&t380_source_lock, flags);
}
struct t380_ga_trace_record {
 u64 token, begin_ns, end_ns;
 struct t380_sl_epoch_record before, after;
 int task_pid, caller, messages, attempt, result;
 unsigned short length[2], flags[2];
 unsigned char command, bytes[2], valid_bytes;
 bool foreign, data_write, finished, geometry_overflow;
};
struct t380_ga_actor {
 struct i2c_client *gauge, *charger;
 struct device *request_device;
 struct t380_battery_thermal *sensor;
 struct workqueue_struct *queue;
 struct work_struct work;
 struct completion completion, physical_done;
 spinlock_t lock, trace_lock, cache_lock;
 bool busy, waiting, cancelled, finished, available, accepting;
 bool pm_blocked, shutting_down;
 unsigned int disable_waiters, pm_blockers;
 u64 request, lifetime, request_lifetime, deadline_ms;
 struct t380_sl_epoch_record requested, battery_epoch[2];
 int epoch_reads, result, cancel_reason, request_cancel_reason, physical_error;
 /* Durable completion handoff survives prompt logical cancellation. A single
  * unconsumed failure blocks the next slot until the control worker takes it. */
 struct t380_ga_outcome physical_pending, physical_last;
 u64 physical_completed_ms, physical_sequence;
 u64 foreign_serial, foreign_started;
 enum t380_ga_kind kind;
 bool active, stock_soc;
 void (*execute)(struct t380_ga_actor *);
 struct t380_pa_history history;
 struct t380_pa_tuple tuple;
 struct t380_rf_model model, scratch;
 struct t380_rf_diagnostic diagnostic;
 /* Android measurements are never charging admission or epoch evidence. */
 struct t380_rf_capacity measured_conversion, accepted_conversion;
 u64 conversion_request, conversion_lifetime;
 struct t380_pa_tuple measured;
 bool measured_available, measured_selected;
 int measured_capacity, measured_error;
 u64 measured_ms, measured_sequence;
 bool measured_temp_valid;
 int measured_temp;
 u64 measured_temp_ms;
 struct t380_pa_tuple cache;
 struct t380_sl_epoch_record cache_phases[4];
 unsigned int cache_mismatch_flags;
 u64 cache_off_times[4];
 bool cache_available, cached_selected, cached_matched, cached_capacity_valid;
 bool cached_full_off;
 int cached_capacity, cached_status;
 u64 cached_full_off_ms, cache_sequence;
 struct t380_gauge_trace_observer observer;
 struct t380_ga_trace_record trace[T380_GA_TRACE_RECORDS];
 unsigned int trace_count, guard_count;
 struct t380_ga_guard_record guards[T380_GA_GUARD_RECORDS];
 bool tracing, trace_ready, trace_overflow, trace_foreign, trace_complete;
 u64 trace_run, trace_token, trace_started_ms, trace_ended_ms;
 int trace_result, trace_cancel_reason;
 enum t380_ga_kind trace_kind;
 struct t380_rf_model trace_model;
 struct t380_rf_diagnostic trace_diagnostic;
 struct t380_sl_epoch_record trace_requested, trace_final;
};
static bool t380_ga_owned(struct t380_ga_actor *a)
{
 return a && current_work() == &a->work;
}
static u64 t380_ga_trace_begin(void *context, const struct i2c_msg *msgs,
 int count, int attempt)
{
 struct t380_ga_actor *a = context;
 struct t380_sl_epoch_record epoch;
 struct t380_ga_trace_record *r;
 unsigned long flags;
 u64 token = 0, ns = ktime_get_boottime_ns();
 int i;
 t380_ga_epoch_copy(&epoch);
 spin_lock_irqsave(&a->trace_lock, flags);
 if (current_work() != &a->work && a->foreign_serial != ~(u64)0) ++a->foreign_serial;
 if (!a->tracing) goto done;
 if (a->trace_count == T380_GA_TRACE_RECORDS || a->trace_token == ~(u64)0) {
  a->trace_overflow = true;
  goto done;
 }
 r = &a->trace[a->trace_count++];
 memset(r, 0, sizeof(*r));
 token = r->token = ++a->trace_token;
 r->begin_ns = ns; r->before = epoch;
 r->task_pid = task_pid_nr(current); r->caller = a->kind;
 r->messages = count; r->attempt = attempt;
 if (count > 2 || count < 1) {r->geometry_overflow = true; a->trace_overflow = true;}
 r->foreign = current_work() != &a->work;
 if (r->foreign) a->trace_foreign = true;
 for (i = 0; i < count; ++i) {
  if (i < 2) {r->length[i] = msgs[i].len; r->flags[i] = msgs[i].flags;}
  if (msgs[i].addr != a->gauge->addr) continue;
  if (!(msgs[i].flags & I2C_M_RD) && msgs[i].len) {
   r->command = msgs[i].buf[0];
   if (msgs[i].len != 1) {r->data_write = true; a->trace_foreign = true;}
  }
 }
done:
 spin_unlock_irqrestore(&a->trace_lock, flags);
 return token;
}
static void t380_ga_trace_end(void *context, u64 token,
 const struct i2c_msg *msgs, int count, int result)
{
 struct t380_ga_actor *a = context;
 struct t380_sl_epoch_record epoch;
 unsigned long flags;
 unsigned int n;
 int i;
 u64 ns = ktime_get_boottime_ns();
 t380_ga_epoch_copy(&epoch);
 spin_lock_irqsave(&a->trace_lock, flags);
 if (!a->tracing || a->trace_ready) goto done;
 for (n = 0; n < a->trace_count; ++n) if (a->trace[n].token == token) {
  struct t380_ga_trace_record *r = &a->trace[n];
  r->result = result; r->end_ns = ns; r->after = epoch; r->finished = true;
  if (result == count) for (i = 0; i < count; ++i)
   if (msgs[i].addr == a->gauge->addr && (msgs[i].flags & I2C_M_RD) && msgs[i].len == 2) {
    r->bytes[0] = msgs[i].buf[0]; r->bytes[1] = msgs[i].buf[1]; r->valid_bytes = 2;
   }
  break;
 }
done:
 spin_unlock_irqrestore(&a->trace_lock, flags);
}
static void t380_ga_physical_error(struct t380_ga_actor *a, int error);
static int t380_ga_read_guard(struct t380_ga_actor *a)
{
 struct t380_sl_epoch_record now;
 unsigned long flags;
 int ret = 0;
 bool foreign_fault;
 if (!t380_ga_owned(a)) return -EPERM;
 t380_ga_epoch_copy(&now);
 spin_lock_irqsave(&a->lock, flags);
 if (a->cancelled || a->request_lifetime != a->lifetime) ret = -ECANCELED;
 else if (!a->accepting) ret = -ESHUTDOWN;
 else if (now.sampled_ms > a->deadline_ms || now.sampled_ms < a->requested.sampled_ms) ret = -ESTALE;
 else if (now.system_suspended || now.pm_serial != a->requested.pm_serial ||
          (a->kind != T380_GA_ANDROID && (now.fault_serial != a->requested.fault_serial ||
           now.generation != a->requested.generation))) ret = -ESTALE;
 spin_unlock_irqrestore(&a->lock, flags);
 spin_lock_irqsave(&a->trace_lock, flags);
 foreign_fault = a->foreign_serial != a->foreign_started || a->foreign_serial == ~(u64)0 ||
     (a->tracing && (a->trace_overflow || a->trace_foreign));
 if (!ret && foreign_fault) ret = -EIO;
 spin_unlock_irqrestore(&a->trace_lock, flags);
 if (foreign_fault) t380_ga_physical_error(a, -EIO);
 return ret;
}
static void t380_ga_guard_record(struct t380_ga_actor *a, int kind, int result,
 u64 started, const struct t380_sl_epoch_record *before)
{
 struct t380_sl_epoch_record after;
 unsigned long flags;
 u64 ended = ktime_get_boottime_ns();
 t380_ga_epoch_copy(&after);
 spin_lock_irqsave(&a->trace_lock, flags);
 if (a->tracing) {
  if (a->guard_count == T380_GA_GUARD_RECORDS) a->trace_overflow = true;
  else {
   struct t380_ga_guard_record *r = &a->guards[a->guard_count++];
   r->kind = kind; r->result = result; r->begin_ns = started; r->end_ns = ended;
   r->before = *before; r->after = after;
  }
 }
 spin_unlock_irqrestore(&a->trace_lock, flags);
}
/* Cancellation has no control/acquisition lock dependency. The GPIO OFF owner
 * still acts independently; this only invalidates evidence and pending copies. */
static void t380_ga_cancel_locked(struct t380_ga_actor *a, int reason, bool terminal)
{
 if (a->request && a->request_lifetime == a->lifetime && !a->request_cancel_reason)
  a->request_cancel_reason = reason;
 if (a->lifetime != ~(u64)0) ++a->lifetime;
 else a->accepting = false;
 if (terminal) {a->shutting_down = true; a->accepting = false;}
 a->cancel_reason = reason;
 a->cancelled = true; a->available = false;
 spin_lock(&a->cache_lock);
 a->cache_available = false; a->cached_full_off = false; ++a->cache_sequence;
 if (terminal) {a->measured_available = false; if (!a->measured_error) a->measured_error = -ESHUTDOWN;}
 spin_unlock(&a->cache_lock);
 complete_all(&a->completion);
}
static void t380_ga_cancel_reason(struct t380_ga_actor *a, int reason, bool terminal)
{
 unsigned long flags;
 if (!a) return;
 spin_lock_irqsave(&a->lock, flags);
 t380_ga_cancel_locked(a, reason, terminal);
 spin_unlock_irqrestore(&a->lock, flags);
}
/* Retained internal cancellation helper; callers with a lifecycle cause name it. */
static void t380_ga_cancel(struct t380_ga_actor *a, bool terminal)
{
 t380_ga_cancel_reason(a, terminal ? T380_GA_CANCEL_REMOVE : T380_GA_CANCEL_STOP, terminal);
}
static void t380_ga_physical_error(struct t380_ga_actor *a, int error)
{
 unsigned long flags;
 if (error >= 0) return;
 spin_lock_irqsave(&a->lock, flags);
 if (!a->physical_error) a->physical_error = error;
 spin_lock(&a->cache_lock);
 if (!a->measured_error) a->measured_error = error;
 a->measured_available = false;
 spin_unlock(&a->cache_lock);
 spin_unlock_irqrestore(&a->lock, flags);
}
static void t380_ga_trace_seal(struct t380_ga_actor *a, int result)
{
 unsigned long flags;
 unsigned int n;
 struct t380_sl_epoch_record final;
 int reason, physical;
 spin_lock_irqsave(&a->lock, flags);
 reason = a->request_cancel_reason; physical = a->physical_error;
 spin_unlock_irqrestore(&a->lock, flags);
 if (physical) result = physical;
 else if (reason == T380_GA_CANCEL_TIMEOUT) result = -ETIMEDOUT;
 t380_ga_epoch_copy(&final);
 spin_lock_irqsave(&a->trace_lock, flags);
 a->trace_final = final; a->trace_model = a->scratch; a->trace_diagnostic = a->diagnostic;
 a->tracing = false; a->trace_result = result; a->trace_cancel_reason = reason; a->trace_ended_ms = t380_source_now();
 a->trace_complete = a->trace_started_ms && !a->trace_overflow && !a->trace_foreign;
 for (n = 0; n < a->trace_count; ++n) if (!a->trace[n].finished) a->trace_complete = false;
 /* End callbacks now refuse mutation. Unfinished/foreign rows remain explicit. */
 a->trace_ready = true;
 spin_unlock_irqrestore(&a->trace_lock, flags);
}
/* Request lock is held. Report-only conversion cannot authorize charging.
 * OFF ANDROID acquisition records real source epochs: mismatch still invalidates
 * the strict tuple/history; source chatter does not erase physical measurements. */
static void t380_ga_measured_finish_locked(struct t380_ga_actor *a, int final_result)
{
 struct t380_pa_tuple *t = &a->tuple;
 struct t380_battery_thermal *sensor = a->sensor;
 if ((a->kind != T380_GA_ANDROID && a->kind != T380_GA_TUPLE) ||
     a->cancelled || !a->accepting || a->request_lifetime != a->lifetime ||
     t->ended_ms < t->started_ms || t->ended_ms > a->deadline_ms ||
     t->ended_ms - t->started_ms > 500ULL ||
     t->epoch_record[0].system_suspended || t->epoch_record[1].system_suspended ||
     t->epoch_record[0].pm_serial != t->epoch_record[1].pm_serial)
  return;
 /* A real independently successful ADC observation remains thermal evidence
  * even if a later gauge word fails. Foreign/final guard rejection does not. */
 if (!final_result && !t->field[T380_PA_TEMP].error && (t->valid & 1U)) {
  spin_lock(&a->cache_lock);
  a->measured_temp = t->field[T380_PA_TEMP].value;
  a->measured_temp_ms = t->field[T380_PA_TEMP].ended_ms;
  a->measured_temp_valid = true;
  spin_unlock(&a->cache_lock);
 }

}
static void t380_ga_work(struct work_struct *work)
{
 struct t380_ga_actor *a = container_of(work, struct t380_ga_actor, work);
 unsigned long flags;
 int ret = t380_ga_read_guard(a), final_guard = ret;
 bool executed = false;
 bool capture = a->kind == T380_GA_STOCK || a->kind == T380_GA_SCRATCH;
 if (!ret && capture) {
  /* Barrier excludes an already-started bypass transfer from capture overlap. */
  i2c_lock_bus(a->gauge->adapter, I2C_LOCK_SEGMENT);
  ret = t380_ga_read_guard(a);
  if (!ret) {
   spin_lock_irqsave(&a->trace_lock, flags);
   a->trace_started_ms = t380_source_now(); a->tracing = true;
   spin_unlock_irqrestore(&a->trace_lock, flags);
  }
  i2c_unlock_bus(a->gauge->adapter, I2C_LOCK_SEGMENT);
 }
 if (!ret) {executed = true; a->execute(a); ret = a->result;}
 if (executed) final_guard = t380_ga_read_guard(a);
 if (!ret) ret = final_guard;
 if (capture) t380_ga_trace_seal(a, ret);
 if (a->request_device) {put_device(a->request_device); a->request_device = NULL;}
 spin_lock_irqsave(&a->lock, flags);
 if (!a->waiting && a->request_cancel_reason != T380_GA_CANCEL_NONE) {
  a->physical_last = (struct t380_ga_outcome){ a->request, a->request_lifetime,
   a->request_cancel_reason, a->physical_error };
  a->physical_completed_ms = t380_source_now();
  if (a->physical_sequence != ~(u64)0) ++a->physical_sequence;
  if (a->physical_error && !a->physical_pending.physical_error)
   a->physical_pending = a->physical_last;
 }
 if (executed) t380_ga_measured_finish_locked(a, final_guard);
 a->result = ret; a->finished = true;
 a->available = !a->cancelled && a->request_lifetime == a->lifetime && a->accepting;
 if (!a->waiting) a->busy = false;
 complete_all(&a->completion);
 complete_all(&a->physical_done);
 spin_unlock_irqrestore(&a->lock, flags);
}
/* Caller holds acquisition_lock, but the actor NEVER acquires it or control.
 * Persistent request/output storage outlives a timed-out/cancelled waiter. */
static int t380_ga_submit(struct t380_ga_actor *a, enum t380_ga_kind kind,
 void (*execute)(struct t380_ga_actor *),
 void (*setup)(struct t380_ga_actor *, void *), void *context, unsigned int bound_ms, u64 *ticket, struct t380_ga_outcome *outcome)
{
 unsigned long flags;
 struct t380_sl_epoch_record epoch;
 unsigned long waited;
 int ret;
 *ticket = 0;
 if (outcome) memset(outcome, 0, sizeof(*outcome));
 t380_ga_epoch_copy(&epoch);
 spin_lock_irqsave(&a->lock, flags);
 if (a->physical_pending.physical_error) {
  if (outcome) *outcome = a->physical_pending;
  ret = a->physical_pending.physical_error;
  spin_unlock_irqrestore(&a->lock, flags); return ret;
 }
 if (!a->accepting || a->busy || a->request == ~(u64)0 || a->lifetime == ~(u64)0) {
  ret = !a->accepting ? -ESHUTDOWN : a->busy ? -EBUSY : -EOVERFLOW;
  if (a->accepting && a->busy && (a->request_cancel_reason == T380_GA_CANCEL_DISABLE ||
      a->request_cancel_reason == T380_GA_CANCEL_PM ||
      a->request_cancel_reason == T380_GA_CANCEL_STOP ||
      a->request_cancel_reason == T380_GA_CANCEL_REMOVE)) {
   ret = -ECANCELED;
   if (outcome) {outcome->cancel_reason = a->request_cancel_reason;
    outcome->request = a->request; outcome->lifetime = a->request_lifetime;
    outcome->physical_error = a->physical_error;}
   if (a->physical_error) ret = a->physical_error;
  }
  if (outcome && !a->accepting) {outcome->cancel_reason = a->cancel_reason; outcome->lifetime = a->lifetime;}
  spin_unlock_irqrestore(&a->lock, flags); return ret;
 }
 ++a->request; a->request_lifetime = a->lifetime; a->requested = epoch;
 a->deadline_ms = epoch.sampled_ms + bound_ms;
 if (a->deadline_ms < epoch.sampled_ms) {
  spin_unlock_irqrestore(&a->lock, flags); return -EOVERFLOW;
 }
 if ((kind == T380_GA_STOCK || kind == T380_GA_SCRATCH) && a->trace_run == ~(u64)0) {
  spin_unlock_irqrestore(&a->lock, flags); return -EOVERFLOW;
 }
 *ticket = a->request;
 a->kind = kind; a->execute = execute; a->request_cancel_reason = T380_GA_CANCEL_NONE; a->physical_error = 0; setup(a, context);
 if (kind == T380_GA_STOCK || kind == T380_GA_SCRATCH) {
  spin_lock(&a->trace_lock);
  ++a->trace_run; a->trace_kind = kind; a->trace_requested = epoch;
  a->trace_ready = false; a->trace_result = -EINPROGRESS; a->trace_cancel_reason = T380_GA_CANCEL_NONE;
  memset(&a->scratch, 0, sizeof(a->scratch)); memset(&a->diagnostic, 0, sizeof(a->diagnostic));
  a->trace_count = a->guard_count = 0; a->trace_overflow = a->trace_foreign = a->trace_complete = false;
  a->trace_started_ms = a->trace_ended_ms = 0;
  memset(&a->trace_final, 0, sizeof(a->trace_final));
  memset(&a->trace_model, 0, sizeof(a->trace_model));
  memset(&a->trace_diagnostic, 0, sizeof(a->trace_diagnostic));
  spin_unlock(&a->trace_lock);
 }
 spin_lock(&a->trace_lock); a->foreign_started = a->foreign_serial; spin_unlock(&a->trace_lock); a->busy = a->waiting = true;
 a->cancelled = a->finished = a->available = false;
 reinit_completion(&a->completion); reinit_completion(&a->physical_done);
 /* Queue handoff is atomic with admission closure; quiesce cannot miss it. */
 if (!queue_work(a->queue, &a->work)) {
  if (a->request_device) {put_device(a->request_device); a->request_device = NULL;}
  a->waiting = false; a->finished = true;
  complete_all(&a->physical_done);
  spin_unlock_irqrestore(&a->lock, flags);
  if (kind == T380_GA_STOCK || kind == T380_GA_SCRATCH) t380_ga_trace_seal(a, -EBUSY);
  spin_lock_irqsave(&a->lock, flags); a->busy = false;
  spin_unlock_irqrestore(&a->lock, flags);
  return -EBUSY;
 }
 spin_unlock_irqrestore(&a->lock, flags);
 waited = wait_for_completion_timeout(&a->completion, msecs_to_jiffies(bound_ms));
 spin_lock_irqsave(&a->lock, flags);
 a->waiting = false;
 if (!waited || !a->finished || !a->available) {
  a->cancelled = true; a->available = false;
  if (!waited) {
   a->cancel_reason = a->request_cancel_reason = T380_GA_CANCEL_TIMEOUT;
   if (a->lifetime != ~(u64)0) ++a->lifetime; else a->accepting = false;
   spin_lock(&a->cache_lock); a->cache_available = false; spin_unlock(&a->cache_lock);
  }
  ret = !waited ? -ETIMEDOUT : -ECANCELED;
 } else ret = a->result;
 if (outcome) {outcome->request = a->request; outcome->lifetime = a->request_lifetime; outcome->cancel_reason = a->request_cancel_reason; outcome->physical_error = a->physical_error;}
 if (a->physical_error) ret = a->physical_error;
 if (a->finished) a->busy = false;
 spin_unlock_irqrestore(&a->lock, flags);
 return ret;
}
/* Caller holds request spinlock; publication shares cancellation linearization. */
static bool t380_ga_take_locked(struct t380_ga_actor *a, u64 ticket)
{
 u64 now = t380_source_now();
 return ticket && a->request == ticket && a->finished && a->available &&
  !a->cancelled && a->accepting && a->request_lifetime == a->lifetime &&
  now >= a->requested.sampled_ms && now <= a->deadline_ms;
}
/* Strict conversion is imported one-way, bound to the original acquisition.
 * Caller owns acquisition_lock; no reporting state is written back to policy. */
static void t380_ga_accept_conversion(struct t380_battery_thermal *sensor,
 const struct t380_pa_tuple *t, u64 cycle_ended_ms)
{
 struct t380_ga_actor *a = sensor->actor;
 unsigned long flags;
 if (!a || !sensor->capacity.valid || !sensor->capacity.initialized ||
     sensor->capacity.updated_ms != cycle_ended_ms ||
     sensor->capacity.sample_ms != t->field[T380_PA_SOC].started_ms ||
     cycle_ended_ms < t->ended_ms || cycle_ended_ms < t->started_ms ||
     cycle_ended_ms - t->started_ms > 500ULL) return;
 spin_lock_irqsave(&a->lock, flags);
 spin_lock(&a->cache_lock);
 if (t->acquisition_request == a->request && t->acquisition_lifetime == a->lifetime &&
     !a->cancelled && a->accepting && !a->measured_error) {
  a->accepted_conversion = sensor->capacity;
  a->conversion_request = t->acquisition_request;
  a->conversion_lifetime = t->acquisition_lifetime;
 }
 spin_unlock(&a->cache_lock);
 spin_unlock_irqrestore(&a->lock, flags);
}
static void t380_ga_measured_publish_locked(struct t380_ga_actor *a,
 struct t380_battery_thermal *sensor, const struct t380_pa_tuple *t,
 int status, bool physical_cycle_ok)
{
 /* Reporting may use the conservative physical SOC while charge-model
  * admission is unavailable. A failed/cancelled Resume must not suppress
  * fresh battery readings. Only a matched model enables scaled reporting;
  * no reporting result restores model, capacity or charging authority. */
 bool selected = sensor->stock_selected || (sensor->regular_selected && sensor->regular_model_matched);
 bool imported = false;
 if (!a->finished || !a->available || a->cancelled || !a->accepting || a->result ||
     t->acquisition_request != a->request || t->acquisition_lifetime != a->lifetime ||
     a->measured_error || a->physical_error || t->guard_before_error || t->guard_after_error ||
     t->valid != 15U || t->ended_ms < t->started_ms || t->ended_ms > a->deadline_ms ||
     t->ended_ms - t->started_ms > 500ULL ||
     t->epoch_record[0].system_suspended || t->epoch_record[1].system_suspended ||
     t->epoch_record[0].pm_serial != t->epoch_record[1].pm_serial) return;
 if (a->measured_selected != selected || !a->measured_conversion.valid)
  t380_rf_capacity_init(&a->measured_conversion);
 /* One-way import of an already accepted exact-cycle manufacturer conversion.
  * This carries natural-FULL learning to reporting without letting reporting
  * history create a candidate, FULL status, or charging admission. */
 if (selected && physical_cycle_ok && t->tuple_valid && t->epoch_coherent &&
     a->accepted_conversion.valid && a->accepted_conversion.initialized &&
     a->conversion_request == t->acquisition_request &&
     a->conversion_lifetime == t->acquisition_lifetime) {
  a->measured_conversion = a->accepted_conversion;
  imported = true;
 }
 if (selected && !imported) t380_rf_capacity_update(&a->measured_conversion,
  t->field[T380_PA_SOC].raw, t->ended_ms, t->field[T380_PA_SOC].started_ms,
  physical_cycle_ok && status == POWER_SUPPLY_STATUS_CHARGING, false);
 if (selected && !a->measured_conversion.valid) return;
 if (a->measured_available && a->measured_ms == t->ended_ms &&
     a->measured.acquisition_request == t->acquisition_request) return;
 a->measured = *t; a->measured_ms = t->ended_ms;
 a->measured_capacity = selected ? a->measured_conversion.capacity_old :
  min(t->field[T380_PA_SOC].raw >> 8, 100);
 a->measured_selected = selected; a->measured_available = true; ++a->measured_sequence;
}
static void t380_ga_cache_publish_full(struct t380_battery_thermal *sensor,
 const struct t380_pa_tuple *tuple, int status, bool physical_ok,
 const struct t380_sl_epoch_record *phases, const u64 *off_times, unsigned int mismatch)
{
 struct t380_ga_actor *a = sensor->actor;
 unsigned long flags;
 /* acquisition_lock holds sensor metadata stable. One publication critical
  * section exposes a whole cycle, including unavailable failed diagnostics. */
 spin_lock_irqsave(&a->lock, flags);
 spin_lock(&a->cache_lock);
 t380_ga_measured_publish_locked(a, sensor, tuple, status, physical_ok);
 a->cache = *tuple; a->cached_status = status;
 if (phases) memcpy(a->cache_phases, phases, sizeof(a->cache_phases));
 else memset(a->cache_phases, 0, sizeof(a->cache_phases));
 if (off_times) memcpy(a->cache_off_times, off_times, sizeof(a->cache_off_times));
 else memset(a->cache_off_times, 0, sizeof(a->cache_off_times));
 a->cache_mismatch_flags = mismatch;
 if (a->measured_available && (a->measured_selected !=
     (sensor->stock_selected || (sensor->regular_selected && sensor->regular_model_matched))))
  a->measured_available = false;
 a->cached_selected = sensor->regular_selected;
 a->cached_matched = sensor->regular_model_matched;
 a->cached_capacity_valid = sensor->capacity.valid;
 a->cached_capacity = sensor->capacity.capacity_old;
 a->cached_full_off = sensor->regular_full_off;
 a->cached_full_off_ms = sensor->regular_full_off_ms;
 a->cache_available = physical_ok && tuple->tuple_valid && tuple->epoch_coherent &&
  a->accepting && !a->cancelled && tuple->acquisition_lifetime == a->lifetime;
 ++a->cache_sequence;
 spin_unlock(&a->cache_lock);
 spin_unlock_irqrestore(&a->lock, flags);
}
static void t380_ga_cache_publish(struct t380_battery_thermal *sensor,
 const struct t380_pa_tuple *tuple, int status, bool physical_ok)
{
 t380_ga_cache_publish_full(sensor, tuple, status, physical_ok, NULL, NULL, 0);
}
static u64 t380_ga_command_ticket(struct t380_ga_actor *a)
{
 unsigned long flags;
 u64 ticket;
 spin_lock_irqsave(&a->lock, flags);
 ticket = a->accepting && !a->physical_pending.physical_error ? a->lifetime : 0;
 spin_unlock_irqrestore(&a->lock, flags);
 return ticket;
}
static bool t380_ga_command_current(struct t380_ga_actor *a, u64 ticket)
{
 unsigned long flags;
 bool current_ticket;
 spin_lock_irqsave(&a->lock, flags);
 current_ticket = ticket && a->accepting && !a->physical_pending.physical_error && a->lifetime == ticket;
 spin_unlock_irqrestore(&a->lock, flags);
 return current_ticket;
}
static void t380_ga_disable_begin(struct t380_ga_actor *a)
{
 unsigned long flags;
 spin_lock_irqsave(&a->lock, flags);
 if (a->disable_waiters != ~0U) ++a->disable_waiters;
 else a->shutting_down = true;
 a->accepting = false;
 t380_ga_cancel_locked(a, T380_GA_CANCEL_DISABLE, false);
 spin_unlock_irqrestore(&a->lock, flags);
}
static void t380_ga_disable_end(struct t380_ga_actor *a)
{
 unsigned long flags;
 spin_lock_irqsave(&a->lock, flags);
 if (a->disable_waiters) --a->disable_waiters;
 a->accepting = !a->disable_waiters && !a->pm_blocked && !a->shutting_down && a->lifetime != ~(u64)0;
 spin_unlock_irqrestore(&a->lock, flags);
}
/* Return ownership of this newly added blocker, not somebody else's bit. */
static bool t380_ga_pm_begin_mode(struct t380_ga_actor *a, int reason, unsigned int blocker)
{
 unsigned long flags;
 bool owned;
 spin_lock_irqsave(&a->lock, flags);
 owned = !(a->pm_blockers & blocker);
 a->pm_blockers |= blocker; a->pm_blocked = true; a->accepting = false;
 t380_ga_cancel_locked(a, reason, false);
 spin_unlock_irqrestore(&a->lock, flags);
 return owned;
}
static void t380_ga_pm_end_mode(struct t380_ga_actor *a, unsigned int blocker)
{
 unsigned long flags;
 spin_lock_irqsave(&a->lock, flags);
 a->pm_blockers &= ~blocker; a->pm_blocked = a->pm_blockers != 0;
 a->accepting = !a->pm_blocked && !a->disable_waiters && !a->shutting_down && a->lifetime != ~(u64)0;
 spin_unlock_irqrestore(&a->lock, flags);
}
/* No worker-required locks are held. Logical cancellation is not physical idle. */
static int t380_ga_quiesce(struct t380_ga_actor *a)
{
 return wait_for_completion_timeout(&a->physical_done, msecs_to_jiffies(2000)) ? 0 : -EBUSY;
}
static int t380_ga_suspend(struct t380_ga_actor *a, unsigned int blocker, bool terminal)
{
 bool owned = t380_ga_pm_begin_mode(a, T380_GA_CANCEL_PM, blocker);
 int ret = t380_charge_force_off(&a->gauge->dev);
 int drain_error = 0;
 /* Terminal drain is unconditional even if independent HIGH verification fails. */
 if (terminal) flush_work(&a->work);
 else if (!ret) drain_error = t380_ga_quiesce(a);
 if (!ret) ret = drain_error;
 /* The failing device need not receive resume. Undo only this callback's bit;
  * the canceled lifetime/physical slot/cache stay invalid and cannot authorize. */
 if (ret && !terminal && owned) t380_ga_pm_end_mode(a, blocker);
 return ret;
}
static int t380_ga_pm_mode(struct t380_ga_actor *a, bool suspend, unsigned int blocker)
{
 if (suspend) return t380_ga_suspend(a, blocker, false);
 t380_ga_cancel_reason(a, T380_GA_CANCEL_PM, false);
 t380_ga_pm_end_mode(a, blocker);
 return 0;
}
static bool t380_ga_pm_begin(struct t380_ga_actor *a, int reason)
{
 return t380_ga_pm_begin_mode(a, reason, 2);
}
static int t380_ga_pm(struct t380_ga_actor *a, bool suspend)
{
 return t380_ga_pm_mode(a, suspend, 1);
}
static int t380_ga_consumer_pm(struct t380_ga_actor *a, bool suspend)
{
 return t380_ga_pm_mode(a, suspend, 2);
}
/* Caller holds control mutex through pure-policy evaluation/fault publication.
 * New actor work is denied until the pending failure is handed to that owner. */
static bool t380_ga_physical_take(struct t380_ga_actor *a, struct t380_ga_outcome *out)
{
 unsigned long flags;
 bool present;
 spin_lock_irqsave(&a->lock, flags);
 present = a->physical_pending.physical_error != 0;
 if (present) {*out = a->physical_pending; memset(&a->physical_pending, 0, sizeof(a->physical_pending));}
 spin_unlock_irqrestore(&a->lock, flags);
 return present;
}
static void t380_ga_cleanup(void *context)
{
 struct t380_ga_actor *a = context;
 t380_ga_cancel(a, true);
 cancel_work_sync(&a->work);
 if (a->request_device) {put_device(a->request_device); a->request_device = NULL;}
 t380_gauge_trace_unregister(&a->observer);
 destroy_workqueue(a->queue);
}
static int t380_ga_init(struct i2c_client *gauge, struct t380_battery_thermal *sensor)
{
 struct t380_ga_actor *a = devm_kzalloc(&gauge->dev, sizeof(*a), GFP_KERNEL);
 int ret;
 if (!a) return -ENOMEM;
 a->gauge = gauge; a->sensor = sensor; a->lifetime = 1; a->accepting = true;
 spin_lock_init(&a->lock); spin_lock_init(&a->trace_lock); spin_lock_init(&a->cache_lock);
 init_completion(&a->completion); init_completion(&a->physical_done); complete_all(&a->physical_done); INIT_WORK(&a->work, t380_ga_work);
 a->queue = alloc_ordered_workqueue("t380-gauge-acquisition", WQ_MEM_RECLAIM);
 if (!a->queue) return -ENOMEM;
 a->observer.adapter = gauge->adapter; a->observer.address = gauge->addr;
 a->observer.context = a; a->observer.begin = t380_ga_trace_begin; a->observer.end = t380_ga_trace_end;
 ret = t380_gauge_trace_register(&a->observer);
 if (ret) {destroy_workqueue(a->queue); return ret;}
 sensor->actor = a;
 return devm_add_action_or_reset(&gauge->dev, t380_ga_cleanup, a);
}
/* Existing acquisition/report freshness bound; never used as charge authority. */
#define T380_GA_CACHE_MAX_AGE_MS 2000ULL
static void t380_ga_meta_publish(struct t380_battery_thermal *sensor)
{
 struct t380_ga_actor *a = sensor->actor;
 unsigned long flags;
 spin_lock_irqsave(&a->cache_lock, flags);
 if (a->measured_available && (a->measured_selected !=
     (sensor->stock_selected || (sensor->regular_selected && sensor->regular_model_matched))))
  a->measured_available = false;
 a->cached_selected = sensor->regular_selected;
 a->cached_matched = sensor->regular_model_matched;
 a->cached_capacity_valid = sensor->capacity.valid;
 a->cached_capacity = sensor->capacity.capacity_old;
 a->cached_full_off = sensor->regular_full_off;
 a->cached_full_off_ms = sensor->regular_full_off_ms;
 /* Metadata transitions invalidate the whole serving record until a cycle. */
 a->cache_available = false; ++a->cache_sequence;
 spin_unlock_irqrestore(&a->cache_lock, flags);
}
static int t380_ga_cached_property(struct t380_battery_thermal *sensor,
 enum power_supply_property prop, int *value)
{
 struct t380_ga_actor *a = sensor->actor;
 struct t380_sl_epoch_record epoch;
 struct t380_pa_tuple c;
 unsigned long flags;
 bool available, selected, matched, capacity_valid, full_off;
 int capacity, status, field, ret = 0;
 u64 full_ms;
 t380_ga_epoch_copy(&epoch);
 spin_lock_irqsave(&a->cache_lock, flags);
 c = a->cache; available = a->cache_available;
 selected = a->cached_selected; matched = a->cached_matched;
 capacity_valid = a->cached_capacity_valid; capacity = a->cached_capacity;
 status = a->cached_status; full_off = a->cached_full_off; full_ms = a->cached_full_off_ms;
 spin_unlock_irqrestore(&a->cache_lock, flags);
 if (!available) return -ENODATA;
 if (epoch.system_suspended || epoch.generation != c.epoch_after ||
     epoch.pm_serial != c.epoch_record[1].pm_serial ||
     epoch.fault_serial != c.epoch_record[1].fault_serial ||
     epoch.sampled_ms < c.ended_ms || epoch.sampled_ms - c.ended_ms > T380_GA_CACHE_MAX_AGE_MS)
  return -ESTALE;
 if (prop == POWER_SUPPLY_PROP_STATUS) {
  *value = status;
  if (status == POWER_SUPPLY_STATUS_NOT_CHARGING && selected && matched &&
      capacity_valid && capacity == 100 && full_off && epoch.sampled_ms >= full_ms &&
      epoch.sampled_ms - full_ms <= 2000)
   *value = POWER_SUPPLY_STATUS_FULL;
  return 0;
 }
 field = prop == POWER_SUPPLY_PROP_TEMP ? T380_PA_TEMP :
  prop == POWER_SUPPLY_PROP_VOLTAGE_NOW ? T380_PA_VOLTAGE :
  prop == POWER_SUPPLY_PROP_CURRENT_NOW ? T380_PA_CURRENT : T380_PA_SOC;
 if (c.field[field].error || !(c.valid & (1U << field)))
  return c.field[field].error ? c.field[field].error : -ENODATA;
 if (prop == POWER_SUPPLY_PROP_CAPACITY) {
  if (selected) {
   ret = matched && capacity_valid ? 0 : -ENODATA; *value = capacity;
  } else *value = min(c.field[T380_PA_SOC].raw >> 8, 100);
 } else *value = c.field[field].value;
 return ret;
}
/* PROPOSED finite reporting bound. Root adjudication required before freeze.
 * CLOCK_BOOTTIME age includes sleep; source events never extend this clock. */
#define T380_GA_MEASURED_MAX_AGE_MS 65000ULL /* 30s autonomous sleep polling plus wake latency */
static int t380_ga_android_property(struct t380_battery_thermal *sensor,
 enum power_supply_property prop, int *value)
{
 struct t380_ga_actor *a = sensor->actor;
 struct t380_pa_tuple c;
 struct t380_sl_epoch_record epoch;
 unsigned long flags;
 bool available;
 int capacity, error, field;
 u64 measured_ms, temp_ms;
 int temp; bool temp_valid;
 t380_ga_epoch_copy(&epoch);
 spin_lock_irqsave(&a->cache_lock, flags);
 c = a->measured; available = a->measured_available;
 capacity = a->measured_capacity; error = a->measured_error; measured_ms = a->measured_ms;
 temp = a->measured_temp; temp_ms = a->measured_temp_ms; temp_valid = a->measured_temp_valid;
 spin_unlock_irqrestore(&a->cache_lock, flags);
 if (prop == POWER_SUPPLY_PROP_TEMP) {
  if (!temp_valid || epoch.sampled_ms < temp_ms || epoch.sampled_ms - temp_ms > T380_GA_MEASURED_MAX_AGE_MS)
   return -ENODATA;
  *value = temp; return 0;
 }
 if (prop == POWER_SUPPLY_PROP_STATUS) {
  /* CHARGING/FULL remains bound to the entire original admission epoch. */
  if (error || !available || epoch.sampled_ms < measured_ms ||
      epoch.sampled_ms - measured_ms > T380_GA_MEASURED_MAX_AGE_MS) {
   *value = POWER_SUPPLY_STATUS_UNKNOWN; return 0;
  }
  if (sensor->stock_selected) {
   spin_lock_irqsave(&a->cache_lock,flags); *value=a->cached_status; spin_unlock_irqrestore(&a->cache_lock,flags);
  } else if (t380_ga_cached_property(sensor, prop, value)) *value = POWER_SUPPLY_STATUS_UNKNOWN;
  return 0;
 }
 if (error || !available || epoch.sampled_ms < measured_ms ||
     epoch.sampled_ms - measured_ms > T380_GA_MEASURED_MAX_AGE_MS) {
  if (prop == POWER_SUPPLY_PROP_CAPACITY_LEVEL) {
   *value = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN; return 0;
  }
  return error ? error : !available ? -ENODATA : -ESTALE;
 }
 if (prop == POWER_SUPPLY_PROP_CAPACITY_LEVEL) {
  return -ENODATA; /* Preserve ordinary Android UNSUPPORTED fallback. */
 }
 if (prop == POWER_SUPPLY_PROP_CAPACITY) {*value = capacity; return 0;}
 field = prop == POWER_SUPPLY_PROP_TEMP ? T380_PA_TEMP :
  prop == POWER_SUPPLY_PROP_VOLTAGE_NOW ? T380_PA_VOLTAGE : T380_PA_CURRENT;
 if (c.field[field].error || !(c.valid & (1U << field)))
  return c.field[field].error ? c.field[field].error : -ENODATA;
 *value = c.field[field].value;
 return 0;
}

static ssize_t t380_ga_android_telemetry_show(struct t380_battery_thermal *sensor, char *buf)
{
 struct t380_ga_actor *a = sensor->actor;
 struct t380_pa_tuple c;
 struct t380_sl_epoch_record before, after;
 unsigned long flags;
 u64 seq, ms, temp_ms, now;
 int capacity, error, temp, status, cause = 0;
 bool available, temp_valid, valid;
 t380_ga_epoch_copy(&before); now = before.sampled_ms;
 spin_lock_irqsave(&a->cache_lock, flags);
 c = a->measured; seq = a->measured_sequence; ms = a->measured_ms;
 capacity = a->measured_capacity; error = a->measured_error;
 available = a->measured_available;
 temp_valid = a->measured_temp_valid; temp = a->measured_temp; temp_ms = a->measured_temp_ms;
 status = a->cached_status;
 spin_unlock_irqrestore(&a->cache_lock, flags);
 valid = available && !error && now >= ms && now - ms <= T380_GA_MEASURED_MAX_AGE_MS;
 temp_valid = temp_valid && now >= temp_ms && now - temp_ms <= T380_GA_MEASURED_MAX_AGE_MS;
 if (error) cause = 3; else if (!available) cause = seq ? 4 : 1; else if (!valid) cause = 2;
 if (!valid) status = POWER_SUPPLY_STATUS_UNKNOWN;
 /* A complete stock snapshot remains truthful as of this read. A newer
  * publication is not an I/O failure and must not flash an unknown battery. */
 if (!sensor->stock_selected) {
 status = POWER_SUPPLY_STATUS_UNKNOWN;
 if (valid) t380_ga_android_property(sensor, POWER_SUPPLY_PROP_STATUS, &status);
 t380_ga_epoch_copy(&after);
 if (before.generation != after.generation || before.pm_serial != after.pm_serial ||
     before.fault_serial != after.fault_serial || after.system_suspended)
  status = POWER_SUPPLY_STATUS_UNKNOWN;
 spin_lock_irqsave(&a->cache_lock, flags);
 if (seq != a->measured_sequence || ms != a->measured_ms || error != a->measured_error ||
     available != a->measured_available || temp_ms != a->measured_temp_ms) {
  spin_unlock_irqrestore(&a->cache_lock, flags); return -EAGAIN;
 }
 spin_unlock_irqrestore(&a->cache_lock, flags);
 }
 return scnprintf(buf, PAGE_SIZE,
  "t380_measured_v1=1 seq=%llu sampled_ms=%llu valid=%u cause=%d error=%d capacity=%d voltage_uv=%d current_ua=%d temp_valid=%u temp=%d temp_ms=%llu status=%d\n",
  (unsigned long long)seq, (unsigned long long)ms, valid, cause, error,
  valid ? capacity : -1, c.field[T380_PA_VOLTAGE].value, c.field[T380_PA_CURRENT].value,
  temp_valid, temp, (unsigned long long)temp_ms, status);
}

#endif
