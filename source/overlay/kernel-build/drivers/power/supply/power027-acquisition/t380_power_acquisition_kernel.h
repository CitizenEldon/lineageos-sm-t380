/* SPDX-License-Identifier: GPL-2.0-only */
/* Included once after the existing battery temperature getter. */
#ifndef T380_POWER_ACQUISITION_KERNEL_H
#define T380_POWER_ACQUISITION_KERNEL_H
#include "t380_power_acquisition.c"

static int t380_pa_kernel_guard(void *context)
{
    struct i2c_client *client = context;
    struct t380_battery_thermal *sensor = i2c_get_clientdata(client);
    int ret = t380_ga_read_guard(sensor->actor);
    if (!ret) ret = t380_charge_safety_ready(&client->dev);
    if (!ret) ret = t380_ga_read_guard(sensor->actor);
    return ret;
}
static int t380_pa_kernel_word(void *context, unsigned char address)
{
    struct i2c_client *client = context;
    struct t380_battery_thermal *sensor = i2c_get_clientdata(client);
    int ret = t380_ga_read_guard(sensor->actor);
    if (ret) return ret;
    ret = i2c_smbus_read_word_data(client, address);
    t380_ga_physical_error(sensor->actor, ret);
    if (ret >= 0) {
        int guard = t380_ga_read_guard(sensor->actor);
        if (guard) return guard;
    }
    return ret;
}
static int t380_pa_kernel_temperature(void *context, int *adc, int *temp)
{
    struct i2c_client *client = context;
    struct t380_battery_thermal *sensor = i2c_get_clientdata(client);
    int ret;

    if (!sensor) return -ENODEV;
    ret = t380_ga_read_guard(sensor->actor);
    if (ret) return ret;
    ret = t380_adc_read(sensor->channel, &sensor->lock, T380_BATTERY_SAMPLES, adc);
    if (!ret) ret = t380_adc_interpolate(sensor->table, sensor->count, *adc, temp);
    t380_ga_physical_error(sensor->actor, ret);
    if (!ret) ret = t380_ga_read_guard(sensor->actor);
    return ret;
}
static u64 t380_pa_kernel_epoch(void *context)
{
    struct t380_sl_snapshot snapshot;
    unsigned long flags;

    spin_lock_irqsave(&t380_source_lock, flags);
    t380_sl_copy(&t380_source_state, t380_source_now(), &snapshot);
    {
        struct t380_battery_thermal *sensor = i2c_get_clientdata(context);
        struct t380_ga_actor *a = sensor ? sensor->actor : NULL;
        if (a && current_work() == &a->work && (a->kind == T380_GA_TUPLE || a->kind == T380_GA_ANDROID) && a->epoch_reads < 2) {
            struct t380_sl_epoch_record *e = &a->tuple.epoch_record[a->epoch_reads++];
            e->generation = snapshot.generation; e->pm_serial = snapshot.pm_serial;
            e->fault_serial = snapshot.fault_serial; e->invalidated_ms = snapshot.invalidated_ms;
            e->last_invalidation = snapshot.last_invalidation; e->sampled_ms = snapshot.now_ms;
            e->system_suspended = snapshot.system_suspended; e->bus_suspended = snapshot.bus_suspended;
        }
    }
    spin_unlock_irqrestore(&t380_source_lock, flags);
    /* Stock physical tuple coherence is an I/O/PM boundary, not a host
     * enrollment epoch. The controller rereads physical source/fault status
     * immediately before programming or enabling. */
    return ((struct t380_battery_thermal *)i2c_get_clientdata(context))->stock_selected ?
        snapshot.pm_serial + 1 : snapshot.generation;
}
static u64 t380_pa_kernel_now(void *context)
{
    return t380_source_now();
}
static void t380_pa_kernel_revoke(void *context)
{
    unsigned long flags;
    u64 now;

    spin_lock_irqsave(&t380_source_lock, flags);
    now = t380_source_now();
    if (sl_time(&t380_source_state, now)) {
        t380_source_state.last_bad_ms = now;
        sl_revoke(&t380_source_state, now, T380_SL_OBSERVATION_ERROR);
        t380_source_state.charger.valid = t380_source_state.muic.valid = false;
    }
    spin_unlock_irqrestore(&t380_source_lock, flags);
}
struct t380_ga_tuple_setup {struct t380_pa_history *history; bool active, stock;};
static void t380_ga_tuple_setup(struct t380_ga_actor *a, void *context)
{
 struct t380_ga_tuple_setup *in = context;
 a->history = *in->history; a->active = in->active; a->stock_soc = in->stock;
 a->epoch_reads = 0; memset(&a->tuple, 0, sizeof(a->tuple));

}
static void t380_ga_tuple_execute(struct t380_ga_actor *a)
{
 struct t380_pa_reader reader = {
  a->gauge, t380_pa_kernel_guard, t380_pa_kernel_word,
  t380_pa_kernel_temperature, t380_pa_kernel_now,
  t380_pa_kernel_epoch, t380_pa_kernel_revoke, false
 };
 reader.stock_soc = a->stock_soc;

 a->result = t380_pa_acquire(&reader, &a->history, &a->tuple);
 a->tuple.acquisition_lifetime = a->request_lifetime;
 a->tuple.acquisition_request = a->request;
}
static int t380_pa_collect_mode(struct i2c_client *client,
 struct t380_pa_history *history, struct t380_pa_tuple *sample, bool active)
{
 struct t380_battery_thermal *sensor = i2c_get_clientdata(client);
 struct t380_ga_tuple_setup in = {history, active, false};
 unsigned long flags;
 u64 ticket;
 struct t380_ga_outcome outcome;
 int ret;
 if (!sensor) return -ENODEV;
#ifndef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
 if (active) return -EOPNOTSUPP;
#endif
 mutex_lock(&sensor->acquisition_lock);
 in.stock = sensor->stock_selected || (sensor->regular_selected && sensor->regular_model_matched);
 ret = t380_ga_submit(sensor->actor, active ? T380_GA_TUPLE : T380_GA_ANDROID, t380_ga_tuple_execute,
  t380_ga_tuple_setup, &in, 2000, &ticket, &outcome);
 spin_lock_irqsave(&sensor->actor->lock, flags);
 if (t380_ga_take_locked(sensor->actor, ticket)) {
  *sample = sensor->actor->tuple; *history = sensor->actor->history;
 } else {
  if (ticket && sensor->actor->request == ticket) {
   outcome.cancel_reason = sensor->actor->request_cancel_reason;
   outcome.physical_error = sensor->actor->physical_error;
  }
  if (outcome.physical_error) ret = outcome.physical_error;
  else if (!ret) ret = -ECANCELED;
  memset(sample, 0, sizeof(*sample)); memset(history, 0, sizeof(*history));
  sample->guard_before_error = ret ? ret : -ECANCELED;
  sample->acquisition_request = outcome.request;
  sample->acquisition_lifetime = outcome.lifetime;
  sample->acquisition_cancel_reason = outcome.cancel_reason;
  sample->acquisition_physical_error = outcome.physical_error;
  sample->guard_cancelled = !outcome.physical_error &&
   (outcome.cancel_reason == T380_GA_CANCEL_DISABLE ||
    outcome.cancel_reason == T380_GA_CANCEL_PM ||
    outcome.cancel_reason == T380_GA_CANCEL_STOP ||
    outcome.cancel_reason == T380_GA_CANCEL_REMOVE);
 }
 spin_unlock_irqrestore(&sensor->actor->lock, flags);
 mutex_unlock(&sensor->acquisition_lock);
 return ret;
}
static ssize_t battery_observation_show(struct device *dev,
                                         struct device_attribute *attr, char *buf)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct t380_battery_thermal *sensor = i2c_get_clientdata(client);
    struct t380_pa_profile profile = t380_pa_conservative_profile();
    const char *provenance = "conservative_oldest_row";
    struct t380_pa_tuple sample;
    int i, ret, length;
    bool selected, matched;
    struct t380_sl_epoch_record phases[4];
    u64 off_times[4];
    unsigned int mismatch;
    u64 cache_sequence, physical_ms, physical_sequence;
    struct t380_ga_outcome physical_pending, physical_last;

    if (!sensor) return -ENODEV;
    {
        unsigned long flags;
        spin_lock_irqsave(&sensor->actor->cache_lock, flags);
        sample = sensor->actor->cache; cache_sequence = sensor->actor->cache_sequence;
        memcpy(phases, sensor->actor->cache_phases, sizeof(phases));
        memcpy(off_times, sensor->actor->cache_off_times, sizeof(off_times));
        mismatch = sensor->actor->cache_mismatch_flags;
        selected = sensor->actor->cached_selected; matched = sensor->actor->cached_matched;
        ret = sensor->actor->cache_available ? 0 : -ENODATA;
        spin_unlock_irqrestore(&sensor->actor->cache_lock, flags);
    }
    {
        unsigned long flags;
        spin_lock_irqsave(&sensor->actor->lock, flags);
        physical_pending = sensor->actor->physical_pending;
        physical_last = sensor->actor->physical_last;
        physical_ms = sensor->actor->physical_completed_ms;
        physical_sequence = sensor->actor->physical_sequence;
        spin_unlock_irqrestore(&sensor->actor->lock, flags);
    }
    if (selected && matched) {
        profile = t380_pa_stock_default_profile();
        provenance = "stock_default_age_unknown4300_4230";
    }
    length = scnprintf(buf, PAGE_SIZE,
        "cache_sequence=%llu acquisition_request=%llu acquisition_lifetime=%llu acquisition_cancel_reason=%d acquisition_physical_error=%d\n"
        "read_only=1 cached_only=1 current_permission=0 telemetry_validated=0 average_validated=0 source_validated=0 normal_charge_region_validated=0 hardware_atomic=0 hardware_sample_age_known=0\n"
        "acquisition_rc=%d guard_before_error=%d guard_after_error=%d tuple_valid=%d valid=0x%x source_epoch_coherent=%d source_epoch_before=%llu source_epoch_after=%llu started_boottime_ms=%llu ended_boottime_ms=%llu\n"
        "history_ready=%d average_current_ma=%d history_min_interval_ms=1000 history_max_interval_ms=30000 history_seed=first_actual_current\n"
        "profile_provenance=%s measured_age_known=0 selected_row=%d float_mv=%d full_mv=%d full_soc=%d recharge_mv=%d conservative_voltage_blocked=%d\n",
        (unsigned long long)cache_sequence, (unsigned long long)sample.acquisition_request,
        (unsigned long long)sample.acquisition_lifetime, sample.acquisition_cancel_reason, sample.acquisition_physical_error,
        ret, sample.guard_before_error, sample.guard_after_error, sample.tuple_valid,
        sample.valid, sample.epoch_coherent,
        (unsigned long long)sample.epoch_before, (unsigned long long)sample.epoch_after,
        (unsigned long long)sample.started_ms, (unsigned long long)sample.ended_ms,
        sample.history_ready, sample.average_ma, provenance, profile.selected_row, profile.float_mv,
        profile.full_mv, profile.full_soc, profile.recharge_mv, sample.conservative_voltage_blocked);
    length += scnprintf(buf + length, PAGE_SIZE - length,
        "physical_completion_sequence=%llu physical_completed_ms=%llu physical_request=%llu physical_lifetime=%llu physical_cancel_reason=%d physical_error=%d physical_pending_request=%llu physical_pending_lifetime=%llu physical_pending_error=%d\n",
        (unsigned long long)physical_sequence, (unsigned long long)physical_ms,
        (unsigned long long)physical_last.request, (unsigned long long)physical_last.lifetime,
        physical_last.cancel_reason, physical_last.physical_error,
        (unsigned long long)physical_pending.request, (unsigned long long)physical_pending.lifetime,
        physical_pending.physical_error);
    for (i = 0; i < T380_PA_FIELDS; ++i) {
        const struct t380_pa_field_sample *field = &sample.field[i];
        length += scnprintf(buf + length, PAGE_SIZE - length,
            "field=%d raw=%d value=%d error=%d started_ms=%llu ended_ms=%llu\n",
            i, field->raw, field->value, field->error,
            (unsigned long long)field->started_ms, (unsigned long long)field->ended_ms);
    }
    length += scnprintf(buf + length, PAGE_SIZE - length,
      "epoch_mismatch_flags=%u cycle_start_generation_mismatch=%d battery_before_generation_mismatch=%d battery_after_generation_mismatch=%d cycle_start_pm_mismatch=%d battery_before_pm_mismatch=%d battery_after_pm_mismatch=%d cycle_start_fault_mismatch=%d battery_before_fault_mismatch=%d battery_after_fault_mismatch=%d off_before_start_ms=%llu off_before_end_ms=%llu off_after_start_ms=%llu off_after_end_ms=%llu guard_before_start_ms=%llu guard_before_end_ms=%llu guard_after_start_ms=%llu guard_after_end_ms=%llu\n",
      mismatch, !!(mismatch & 1), !!(mismatch & 2), !!(mismatch & 4),
      !!(mismatch & 8), !!(mismatch & 16), !!(mismatch & 32),
      !!(mismatch & 64), !!(mismatch & 128), !!(mismatch & 256),
      (unsigned long long)off_times[0], (unsigned long long)off_times[1],
      (unsigned long long)off_times[2], (unsigned long long)off_times[3],
      (unsigned long long)sample.guard_before_start_ms, (unsigned long long)sample.guard_before_end_ms,
      (unsigned long long)sample.guard_after_start_ms, (unsigned long long)sample.guard_after_end_ms);
    for (i = 0; i < 4; ++i) {
      const char *name[4] = {"cycle_start", "battery_before", "battery_after", "cycle_final"};
      const struct t380_sl_epoch_record *e = &phases[i];
      length += scnprintf(buf + length, PAGE_SIZE - length,
       "phase=%s generation=%llu pm_serial=%llu fault_serial=%llu invalidated_ms=%llu invalidation_reason=%d sampled_ms=%llu system_pm=%d bus_pm=%d\n",
       name[i], (unsigned long long)e->generation, (unsigned long long)e->pm_serial,
       (unsigned long long)e->fault_serial, (unsigned long long)e->invalidated_ms,
       e->last_invalidation, (unsigned long long)e->sampled_ms, e->system_suspended, e->bus_suspended);
    }
    if (length >= PAGE_SIZE - 1) return -EOVERFLOW;
    return length;
}
static DEVICE_ATTR_RO(battery_observation);
static void t380_pa_sysfs_remove(void *context)
{
    struct i2c_client *client = context;

    device_remove_file(&client->dev, &dev_attr_battery_observation);
    t380_pa_kernel_revoke(client);
}
static int t380_pa_sysfs_add(struct i2c_client *client)
{
    int ret = device_create_file(&client->dev, &dev_attr_battery_observation);

    if (ret) return ret;
    return devm_add_action_or_reset(&client->dev, t380_pa_sysfs_remove, client);
}
#endif
