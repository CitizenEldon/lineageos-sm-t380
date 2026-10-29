/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_REGULAR_KERNEL_H
#define T380_REGULAR_KERNEL_H
/* Included after the owner/control implementation. Caller holds control->lock;
 * acquire sensor lock second. Never take control lock from a battery getter. */
static int t380_rf_model_observe(struct i2c_client *charger,
                                struct i2c_client *gauge);
static void t380_rf_select(struct i2c_client *gauge, bool selected)
{
 struct t380_battery_thermal *sensor = i2c_get_clientdata(gauge);
 mutex_lock(&sensor->acquisition_lock);
 if (sensor->regular_selected != selected)
  t380_rf_capacity_init(&sensor->capacity);
 sensor->regular_selected = selected;
 sensor->regular_full_off = false;
 t380_ga_meta_publish(sensor);
 mutex_unlock(&sensor->acquisition_lock);
}
static void t380_rf_clear_full(struct i2c_client *gauge)
{
 struct t380_battery_thermal *sensor = i2c_get_clientdata(gauge);
 mutex_lock(&sensor->acquisition_lock);
 sensor->regular_full_off = false;
 t380_ga_meta_publish(sensor);
 mutex_unlock(&sensor->acquisition_lock);
}
/* Hardware callbacks run only on the dedicated acquisition actor. They never
 * acquire control->lock or acquisition_lock, which the submitting caller owns. */
struct t380_rf_kernel_context {struct i2c_client *charger, *gauge;};
static struct t380_ga_actor *t380_rf_actor(void *context)
{
 struct t380_rf_kernel_context *k = context;
 struct t380_battery_thermal *sensor = i2c_get_clientdata(k->gauge);
 return sensor->actor;
}
static int t380_rf_kernel_guard(void *context)
{
 struct t380_rf_kernel_context *k = context;
 struct t380_ga_actor *a = t380_rf_actor(context);
 struct t380_sl_epoch_record before;
 u64 started = ktime_get_boottime_ns();
 int ret;
 t380_ga_epoch_copy(&before);
 ret = t380_ga_read_guard(a);
 if (!ret) ret = t380_charge_inhibit_check(&k->charger->dev);
 if (!ret) ret = t380_ga_read_guard(a);
 t380_ga_guard_record(a, 0, ret, started, &before);
 return ret;
}
static int t380_rf_kernel_word(void *context, unsigned char address)
{
 struct t380_rf_kernel_context *k = context;
 struct t380_ga_actor *a = t380_rf_actor(context);
 unsigned char bytes[2];
 int ret = t380_ga_read_guard(a);
 if (ret) return ret;
 ret = i2c_smbus_read_i2c_block_data(k->gauge, address, 2, bytes);
 if (ret != 2) {t380_ga_physical_error(a, ret < 0 ? ret : -EIO); return ret < 0 ? ret : -EIO;}
 ret = t380_ga_read_guard(a);
 return ret ? ret : bytes[0] | ((unsigned int)bytes[1] << 8);
}
static int t380_rf_kernel_status(void *context)
{
 struct t380_rf_kernel_context *k = context;
 struct t380_ga_actor *a = t380_rf_actor(context);
 struct t380_sl_epoch_record before;
 u64 started = ktime_get_boottime_ns();
 int ret = t380_ga_read_guard(a), guard;
 t380_ga_epoch_copy(&before);
 if (!ret) {
  ret = i2c_smbus_read_byte_data(k->charger, SM5703_STATUS3);
  if (ret < 0) t380_ga_physical_error(a, ret);
  else {guard = t380_ga_read_guard(a); if (guard) ret = guard;}
 }
 t380_ga_guard_record(a, 1, ret, started, &before);
 return ret;
}
static u64 t380_rf_kernel_now(void *context) {return t380_source_now();}
static u64 t380_rf_kernel_epoch(void *context)
{
 struct t380_rf_kernel_context *k = context;
 return t380_pa_kernel_epoch(k->gauge);
}
static int t380_rf_diagnostic_guard(void *context)
{
 struct t380_rf_kernel_context *k = context;
 struct t380_ga_actor *a = t380_rf_actor(context);
 struct t380_sl_epoch_record before;
 unsigned long flags;
 u64 started = ktime_get_boottime_ns();
 bool suspended, revoked;
 int ret;
 t380_ga_epoch_copy(&before);
 ret = t380_ga_read_guard(a);
 if (!ret) ret = t380_charge_diagnostic_off(&k->charger->dev);
 spin_lock_irqsave(&t380_source_lock, flags);
 /* Stock-order diagnostic keeps its accepted USB suspend restriction. Strict
  * scratch requires actual OFF and stable lifecycle, but allows USB idle. */
 suspended = t380_source_state.system_suspended ||
  (a->kind == T380_GA_STOCK && t380_source_state.bus_suspended);
 revoked = t380_source_state.time_fault || t380_source_state.exhausted;
 spin_unlock_irqrestore(&t380_source_lock, flags);
 if (!ret) ret = suspended ? -ESHUTDOWN : revoked ? -ESTALE : t380_ga_read_guard(a);
 t380_ga_guard_record(a, 2, ret, started, &before);
 return ret;
}
static u64 t380_rf_diagnostic_epoch(void *context) {return t380_source_ticket();}
struct t380_ga_rf_setup {struct i2c_client *charger;};
static void t380_ga_rf_setup(struct t380_ga_actor *a, void *context)
{
 struct t380_ga_rf_setup *in = context;
 a->charger = in->charger;
 a->request_device = get_device(&in->charger->dev);
 t380_rf_model_init(&a->model); t380_rf_model_init(&a->scratch);
 t380_rf_diagnostic_init(&a->diagnostic);
}
static void t380_ga_rf_execute(struct t380_ga_actor *a)
{
 struct t380_rf_kernel_context k = {a->charger, a->gauge};
 struct t380_rf_model_io io = {&k, t380_rf_kernel_guard,
  t380_rf_kernel_word, t380_rf_kernel_status, t380_rf_kernel_now,
  t380_rf_kernel_epoch};
 if (a->kind != T380_GA_MODEL) io.guard = t380_rf_diagnostic_guard;
 if (a->kind == T380_GA_STOCK) {
  io.epoch = t380_rf_diagnostic_epoch;
  a->result = t380_rf_diagnostic_run(&io, &a->diagnostic, a->trace_run);
 } else a->result = t380_rf_model_check(&io,
  a->kind == T380_GA_SCRATCH ? &a->scratch : &a->model);
}
static int t380_rf_request(struct i2c_client *charger, struct i2c_client *gauge,
 enum t380_ga_kind kind)
{
 struct t380_battery_thermal *sensor = i2c_get_clientdata(gauge);
 struct t380_ga_rf_setup in = {charger};
 unsigned long flags;
 u64 ticket = 0;
 int ret;
 if (!sensor) return -ENODEV;
 mutex_lock(&sensor->acquisition_lock);
 if (kind != T380_GA_MODEL && sensor->regular_selected) {ret = -EBUSY; goto done;}
 ret = t380_ga_submit(sensor->actor, kind, t380_ga_rf_execute,
  t380_ga_rf_setup, &in, T380_RF_MODEL_MAX_MS, &ticket, NULL);
 spin_lock_irqsave(&sensor->actor->lock, flags);
 if (kind == T380_GA_MODEL) {
  /* Failed or cancelled requests never retain matching admission evidence. */
  if (t380_ga_take_locked(sensor->actor, ticket)) sensor->model = sensor->actor->model;
  else t380_rf_model_init(&sensor->model);
  sensor->model.error = ret; sensor->model.matched = !ret;
  sensor->regular_model_matched = !ret && t380_ga_take_locked(sensor->actor, ticket);
  if (!sensor->regular_model_matched) {
   if (!ret) ret = -ECANCELED;
   sensor->model.error = ret; sensor->model.matched = false;
   sensor->regular_full_off = false; sensor->capacity.valid = false;
  }
 } else if (kind == T380_GA_STOCK && t380_ga_take_locked(sensor->actor, ticket))
  sensor->diagnostic = sensor->actor->diagnostic;
 spin_unlock_irqrestore(&sensor->actor->lock, flags);
 if (kind == T380_GA_MODEL) t380_ga_meta_publish(sensor);
done:
 mutex_unlock(&sensor->acquisition_lock);
 return ret;
}
static int t380_rf_model_observe(struct i2c_client *charger, struct i2c_client *gauge)
{return t380_rf_request(charger, gauge, T380_GA_MODEL);}
static int t380_rf_stock_diagnostic(struct i2c_client *charger, struct i2c_client *gauge)
{return t380_rf_request(charger, gauge, T380_GA_STOCK);}
static int t380_rf_strict_scratch(struct i2c_client *charger, struct i2c_client *gauge)
{return t380_rf_request(charger, gauge, T380_GA_SCRATCH);}
#endif
