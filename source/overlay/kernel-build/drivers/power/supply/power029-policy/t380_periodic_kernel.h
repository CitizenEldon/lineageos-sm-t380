/* SPDX-License-Identifier: GPL-2.0-only */
/* Same existing 0x49 owner. Default route remains inhibited; trial is opt-in. */
#ifndef T380_PERIODIC_KERNEL_H
#define T380_PERIODIC_KERNEL_H
#include <linux/workqueue.h>
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
#include <linux/t380_display_admission.h>
#endif
#include "t380_charge_policy.c"
#include "t380_periodic_policy.c"
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
#include <linux/delay.h>
#include "t380_phase_shadow.c"
#define T380_PP_PHASE_WAIT_MS 100ULL
#define T380_PP_PHASE_WAIT_READS 21
enum t380_phase_apply_stage {
 T380_PHASE_NONE, T380_PHASE_PAUSE, T380_PHASE_ENCODE, T380_PHASE_WAIT,
 T380_PHASE_APPLY, T380_PHASE_EXTRA_STATUS, T380_PHASE_RUNNING,
 T380_PHASE_RESUME, T380_PHASE_ACK, T380_PHASE_DONE,
 T380_PHASE_FULL_OWNER, T380_PHASE_POLICY_ACK
};
enum t380_transaction_kind { T380_TX_NONE, T380_TX_SECOND, T380_TX_FULL, T380_TX_RECHARGE };
struct t380_phase_apply_witness {
 bool valid, ic_ran;
 u64 sequence, sample_ms, started_ms, ended_ms, wait_started_ms, wait_elapsed_ms;
 enum t380_phase_apply_stage stage;
 int first_error, cleanup_error, wait_reads, wait_first, wait_last, extra_status;
 enum t380_transaction_kind kind;
 u64 policy_sample_ms;
 int owner_return, pause_return, resume_return, phase_ack, policy_ack;
 struct t380_ic_result ic;
};
struct t380_rest_stop_witness {
 bool valid;
 u64 sequence, sample_ms, policy_sample_ms;
 int reason, error, cleanup_error;
};

#endif
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
#include <linux/capability.h>
#include "../power030-trial/t380_trial_policy.c"
#define T380_PP_ARM_SAMPLE_DELAY_MS (T380_PA_HISTORY_MIN_MS + 100ULL)
#endif

#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
struct t380_startup_witness {
 u64 started_ms, generation, fault_serial, pm_serial;
 u64 apply_started_ms, apply_ended_ms, prepare_started_ms, prepare_ended_ms;
 int apply_before, apply_after, apply_error, prepare_status3, prepare_error;
 u64 low_ms, post_started_ms, post_ended_ms;
 bool post_attempted, first_recorded;
 int post_status3, guard_before, read_error, guard_after, source_error;
 int off_error, error;
 struct t380_sl_snapshot post_source[2];
 u64 first_started_ms, first_ended_ms, first_generation;
 int first_status3[2], first_current_ua, first_error;
};
#endif
struct t380_pp_dependencies {
	struct i2c_client *gauge, *muic;
};
struct t380_periodic {
	struct i2c_client *charger;
	struct t380_pp_dependencies *dependencies;
	struct delayed_work work;
	struct t380_pa_history history;
	struct t380_pp_state state;
	struct t380_pp_cycle last;
	struct t380_pp_result result;
	bool running;
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
	struct t380_ps_state phase_shadow;
	struct t380_ps_output phase_candidate;
 struct t380_phase_apply_witness phase_apply, full_apply, recharge_apply;
 struct t380_rest_stop_witness rest_stop;
 u64 transaction_sequence;
#endif
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
	bool trial_requested, trial_spent, trial_active, trial_fixed5v;
	u64 arm_not_before_ms;
	struct t380_cp_state trial_policy;
	struct t380_cp_output trial_decision;
	int trial_error;
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
	bool session_requested, session_spent, session_active, session_fixed5v;
	enum t380_trial_voltage_window session_window;
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
	bool session_phase_selected;
	bool session_recharge_selected, session_rest;
#ifdef CONFIG_T380_ATTACHED_MAINTENANCE_DEV
	bool session_attached_selected;
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
	bool household_enabled, household_absence_seen, household_pm_detached;
 bool household_quiet_pending, household_quiet_timeout, household_quiet_retry;
 u64 household_quiet_deadline_ms, household_quiet_sample_ms, household_quiet_epoch;
 u64 household_quiet_generation, household_quiet_fault_serial, household_quiet_pm_serial;
 struct t380_display_snapshot household_quiet_display;
 struct t380_startup_witness startup;
	bool household_detaching, household_detached, household_detached_rest;
	bool household_rebind_pending, household_qualification_pending;
	u64 household_qualification_generation, household_qualification_ms;
	enum t380_household_voltage_window household_window;
	int household_admission_refusal;
	u64 household_enable_ms;
#endif
#endif
#endif
	u64 session_budget_ms;
	struct t380_cp_state session_policy;
	struct t380_cp_output session_decision;
	int session_error;
#endif
#endif
};
static void t380_pp_put_dependencies(void *context)
{
	struct t380_pp_dependencies *d = context;

	if (d->muic) put_device(&d->muic->dev);
	if (d->gauge) put_device(&d->gauge->dev);
}
static int t380_pp_supplier(struct i2c_client *consumer, const char *property,
			   int address, const char *compatible,
			   struct i2c_client **output)
{
	struct device_node *node = of_parse_phandle(consumer->dev.of_node, property, 0);
	struct i2c_client *supplier;
	struct device_link *link;
	int ret = 0;

	if (!node) return -EINVAL;
	if (!of_device_is_compatible(node, compatible)) {
		of_node_put(node); return -EINVAL;
	}
	supplier = of_find_i2c_device_by_node(node);
	of_node_put(node);
	if (!supplier) return -EPROBE_DEFER;
	device_lock(&supplier->dev);
	if (supplier->addr != address) ret = -EINVAL;
	else if (!device_is_bound(&supplier->dev)) ret = -EPROBE_DEFER;
	else {
		/* Managed links unbind the consumer before supplier resources vanish. */
		link = device_link_add(&consumer->dev, &supplier->dev,
				       DL_FLAG_AUTOREMOVE_CONSUMER);
		if (!link) ret = -ENOMEM;
	}
	device_unlock(&supplier->dev);
	if (ret) { put_device(&supplier->dev); return ret; }
	*output = supplier;
	return 0;
}
static int t380_pp_dependencies_get(struct i2c_client *client,
				    struct t380_pp_dependencies **output)
{
	struct t380_pp_dependencies *d;
	int ret;

	d = devm_kzalloc(&client->dev, sizeof(*d), GFP_KERNEL);
	if (!d) return -ENOMEM;
	ret = devm_add_action_or_reset(&client->dev, t380_pp_put_dependencies, d);
	if (ret) return ret;
	ret = t380_pp_supplier(client, "samsung,policy-gauge", 0x71,
		"samsung,t380-sm5703-fg-diagnostic", &d->gauge);
	if (ret) return ret;
	ret = t380_pp_supplier(client, "samsung,policy-muic", 0x25,
		"samsung,t380-sm5703-muic-observation", &d->muic);
	if (ret) return ret;
	*output = d;
	return 0;
}
static void t380_pp_source_copy(struct t380_sl_snapshot *out)
{
	unsigned long flags;

	spin_lock_irqsave(&t380_source_lock, flags);
	t380_sl_copy(&t380_source_state, t380_source_now(), out);
	spin_unlock_irqrestore(&t380_source_lock, flags);
}
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
/* Cached initial startup evidence only; values never authorize policy. */
static void t380_pp_startup_reset(struct t380_periodic *p,
                                  const struct t380_sl_snapshot *source)
{
 p->startup = (struct t380_startup_witness) {
  .started_ms = t380_source_now(), .generation = source->generation,
  .fault_serial = source->fault_serial, .pm_serial = source->pm_serial,
  .apply_before = -ENODATA, .apply_after = -ENODATA,
  .prepare_status3 = -ENODATA, .post_status3 = -ENODATA,
  .first_status3 = { -ENODATA, -ENODATA },
 };
}
static int t380_pp_startup_source(struct t380_periodic *p, unsigned int index)
{
 struct t380_sl_snapshot *source = &p->startup.post_source[index];
 t380_pp_source_copy(source);
 return source->fixed5v_qualified && !source->system_suspended &&
  !source->host_blocked && source->generation == p->startup.generation &&
  source->fault_serial == p->startup.fault_serial &&
  source->pm_serial == p->startup.pm_serial && source->oldest_ms &&
  source->now_ms >= source->oldest_ms &&
  source->now_ms - source->oldest_ms <= 2000 ? 0 : -ESTALE;
}
/* Caller holds charger control lock. Owner APIs release their own lock;
 * this read runs after display_guard_leave. Exactly one STATUS3 read. */
static int t380_pp_startup_post_locked(struct t380_periodic *p,
                                       struct t380_control *control)
{
 struct t380_startup_witness *w = &p->startup;
 struct t380_household_quiet_snapshot owner;
 int ret, source_before, source_after, value;
 if (!w->started_ms || w->post_attempted) return -EPERM;
 w->post_attempted = true;
 ret = t380_charge_household_quiet_snapshot(&p->charger->dev, &owner);
 if (ret) goto fail;
 w->low_ms = owner.low_success_ms;
 if (!w->low_ms) { ret = -ENODATA; goto fail; }
 w->guard_before = t380_charge_session_collector_check(&p->charger->dev);
 source_before = t380_pp_startup_source(p, 0);
 w->source_error = source_before;
 ret = w->guard_before ? w->guard_before : source_before;
 if (ret) goto fail;
 w->post_started_ms = t380_source_now();
 value = i2c_smbus_read_byte_data(p->charger, 0x0a);
 w->post_ended_ms = t380_source_now();
 w->post_status3 = value;
 w->read_error = value < 0 ? value : value > 255 ? -EIO : 0;
 w->guard_after = t380_charge_session_collector_check(&p->charger->dev);
 source_after = t380_pp_startup_source(p, 1);
 w->source_error = source_after;
 ret = w->read_error ? w->read_error :
  w->guard_after ? w->guard_after : source_after;
 if (!ret && (w->post_started_ms < w->low_ms ||
     w->post_ended_ms < w->post_started_ms)) ret = -ERANGE;
 if (!ret && w->post_ended_ms - w->post_started_ms >
     T380_PP_ACQUISITION_LIMIT_MS) ret = -ETIME;
 if (!ret) return 0; /* CHGON/TOP/DONE bits are observation only here. */
fail:
 if (!w->error) w->error = ret;
 w->off_error = t380_charge_trial_revoke_off(&p->charger->dev);
 /* Preserve initiating observation/guard failure even if HIGH repairs it. */
 if (!p->state.latched_error) p->state.latched_error = ret;
 t380_control_fault_locked(p->charger, ret);
 return ret;
}
static void t380_pp_startup_first_locked(struct t380_periodic *p,
 const struct t380_pp_cycle *c, bool active_at_entry)
{
 struct t380_startup_witness *w = &p->startup;
 if (!active_at_entry || !c->trial_active || !w->low_ms || w->first_recorded)
  return;
 w->first_recorded = true;
 w->first_started_ms = c->started_ms;
 w->first_ended_ms = c->ended_ms;
 w->first_generation = c->source.generation;
 w->first_status3[0] = c->charger[0].raw[T380_CO_STATUS3];
 w->first_status3[1] = c->charger[1].raw[T380_CO_STATUS3];
 w->first_current_ua = c->battery.field[T380_PA_CURRENT].value;
 w->first_error = t380_pp_acquisition_error(c, p->state.evaluations,
                                           p->state.last_ms);
}
static int t380_pp_startup_show(const struct t380_periodic *p,
                               char *buf, int length)
{
 const struct t380_startup_witness *w = &p->startup;
 return scnprintf(buf + length, PAGE_SIZE - length,
  "startup_v1=1 start=%llu identity=%llu,%llu,%llu apply=%llu,%llu,%d,%d,%d prepare=%llu,%llu,%d,%d low=%llu post=%d,%llu,%llu,%d,%d,%d,%d,%d,%d,%d source0=%llu,%llu,%llu,%llu,%llu source1=%llu,%llu,%llu,%llu,%llu first=%d,%llu,%llu,%llu,%d,%d,%d,%d\n",
  (unsigned long long)w->started_ms,
  (unsigned long long)w->generation,(unsigned long long)w->fault_serial,
  (unsigned long long)w->pm_serial,
  (unsigned long long)w->apply_started_ms,(unsigned long long)w->apply_ended_ms,
  w->apply_before,w->apply_after,w->apply_error,
  (unsigned long long)w->prepare_started_ms,(unsigned long long)w->prepare_ended_ms,
  w->prepare_status3,w->prepare_error,(unsigned long long)w->low_ms,
  w->post_attempted,(unsigned long long)w->post_started_ms,
  (unsigned long long)w->post_ended_ms,w->post_status3,w->guard_before,
  w->read_error,w->guard_after,w->source_error,w->off_error,w->error,
  (unsigned long long)w->post_source[0].now_ms,
  (unsigned long long)w->post_source[0].oldest_ms,
  (unsigned long long)w->post_source[0].generation,
  (unsigned long long)w->post_source[0].fault_serial,
  (unsigned long long)w->post_source[0].pm_serial,
  (unsigned long long)w->post_source[1].now_ms,
  (unsigned long long)w->post_source[1].oldest_ms,
  (unsigned long long)w->post_source[1].generation,
  (unsigned long long)w->post_source[1].fault_serial,
  (unsigned long long)w->post_source[1].pm_serial,
  w->first_recorded,(unsigned long long)w->first_started_ms,
  (unsigned long long)w->first_ended_ms,(unsigned long long)w->first_generation,
  w->first_status3[0],w->first_status3[1],w->first_current_ua,w->first_error);
}
#endif
static int t380_pp_readback(struct t380_control *control,
			    const struct t380_co_observation *o)
{
	int raw[T380_CS_COUNT], i;
	struct t380_cs_verification verification;

	/* The observation order has STATUS entries before the seven settings. */
	for (i = 0; i < T380_CS_COUNT; ++i) raw[i] = o->raw[T380_CO_CNTL + i];
	return t380_cs_verify(&control->plan, raw, &verification);
}
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
static int t380_pp_session_input_ma(const struct t380_periodic *p)
{
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
 if (p->household_enabled)
  return t380_household_window_input_ma(p->household_window);
#endif
 return 500;
}
static int t380_pp_session_cv_mv(const struct t380_periodic *p)
{
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
 if (p->household_enabled)
  return t380_household_window_cv_mv(p->household_window);
#endif
 return 4190;
}
static int t380_pp_session_initial_mv(const struct t380_periodic *p)
{
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
 if (p->household_enabled)
  return t380_household_window_initial_mv(p->household_window);
#endif
 return 4170;
}
static bool t380_pp_input_contract(const struct t380_periodic *p,
                                  const struct t380_sl_snapshot *source)
{
 return t380_pp_session_input_ma(p) != 900 ||
        source->fixed5v_input_allowance_ma == 900;
}
/* Caller holds charger control->lock. Stop never waits for this lock before
 * revoking the GPIO owner's admission under its separate lock. */
static int t380_pp_prepare_locked(struct t380_periodic *p,
		struct t380_control *control, const struct t380_pp_cycle *c,
		bool fixed5v, bool belkin_session_500ma)
{
	struct i2c_client *client = p->charger;
	struct t380_sl_snapshot live;
	struct t380_cs_plan managed;
	struct t380_ic_result applied = { 0 };
	struct t380_ic_io io = {
		.context = client, .off = t380_control_off,
		.guard = t380_control_guard, .read = t380_control_read,
		.write = t380_control_write,
	};
	const struct t380_cs_limits limits = {
		belkin_session_500ma ? t380_pp_session_input_ma(p) : 100,
		belkin_session_500ma ? 500 : 100,
        belkin_session_500ma ? t380_pp_session_cv_mv(p) : 4190,
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
		p->household_rebind_pending &&
		p->phase_candidate.phase == T380_CP_SECOND ? 250 :
#endif
		350 };
	int ret;

	if (belkin_session_500ma && (!fixed5v || !t380_pp_input_contract(p, &c->source)))
		return -EPERM;
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
 if (t380_household_window_regular(p->household_window)) {
  ret = t380_rf_model_observe(client, p->dependencies->gauge);
  if (ret) return ret;
 }
#endif
	ret = t380_cs_encode_profile(&limits, T380_CS_MANAGED_ATTACHED, &managed);
 if (!ret) {
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
  if (p->household_quiet_pending) p->startup.apply_started_ms = t380_source_now();
#endif
  ret = t380_ic_apply(&io, &managed, &applied);
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
  if (p->household_quiet_pending) {
   p->startup.apply_ended_ms = t380_source_now();
   p->startup.apply_before = applied.status_before;
   p->startup.apply_after = applied.status_after;
   p->startup.apply_error = ret;
  }
#endif
 }
	if (!ret && !applied.verified) ret = -EIO;
	if (!ret) {
		control->plan = managed;
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
  if (p->household_quiet_pending) p->startup.prepare_started_ms = t380_source_now();
#endif
		ret = i2c_smbus_read_byte_data(client, 0x0a);
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
  if (p->household_quiet_pending) {
   p->startup.prepare_ended_ms = t380_source_now();
   p->startup.prepare_status3 = ret;
  }
#endif
		if (ret >= 0 && (ret & 1)) ret = -EIO;
	}
	if (ret >= 0) {
		t380_pp_source_copy(&live);
		ret = (fixed5v ? live.fixed5v_qualified :
		       live.dcp_candidate) &&
			(!belkin_session_500ma || t380_pp_input_contract(p, &live)) &&
			live.generation == c->source.generation &&
			live.now_ms >= live.oldest_ms &&
			live.now_ms - live.oldest_ms <= 2000 ? 0 : -ESTALE;
	}
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
 if (p->household_quiet_pending) p->startup.prepare_error = ret;
#endif
	return ret;
}
static int t380_pp_activate_locked(struct t380_periodic *p,
		struct t380_control *control, const struct t380_pp_cycle *c)
{
	int ret = t380_pp_prepare_locked(p, control, c,
		p->trial_fixed5v, false);

	return ret ? ret : t380_charge_trial_begin(&p->charger->dev,
		c->source.generation);
}
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
static int t380_pp_session_activate_locked(struct t380_periodic *p,
		struct t380_control *control, const struct t380_pp_cycle *c)
{
	int ret = t380_pp_prepare_locked(p, control, c,
		p->session_fixed5v, p->session_fixed5v);

#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
 p->household_quiet_retry = false;
 if (!ret && p->household_quiet_pending) {
  ret = t380_display_guard_enter(p->household_quiet_epoch);
  if (ret == -EAGAIN) {
   p->household_quiet_retry = true;
   p->household_quiet_sample_ms = 0;
   p->phase_shadow.policy.previous_would_charge = false;
   return ret;
  }
  if (!ret) {
   ret = t380_charge_household_quiet_begin(&p->charger->dev,
                                          c->source.generation, c->ended_ms);
   t380_display_guard_leave();
   if (!ret) ret = t380_pp_startup_post_locked(p, control);
  }
  if (ret) {
   struct t380_household_quiet_snapshot owner;
   if (!t380_charge_household_quiet_snapshot(&p->charger->dev, &owner))
    p->household_quiet_timeout = owner.expired && ret == -ETIME;
  }
  p->household_quiet_pending = false;
  return ret;
 }
#endif
	return ret ? ret : t380_charge_session_begin(&p->charger->dev,
		c->source.generation, c->ended_ms);
}
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
static int t380_pp_phase_high(void *context)
{
	struct i2c_client *client = context;

	return t380_charge_phase_paused_high(&client->dev);
}
static void t380_pp_transaction_attempt(struct t380_periodic *p,
 struct t380_phase_apply_witness *w, enum t380_transaction_kind kind,
 u64 sample_ms, enum t380_phase_apply_stage stage)
{
 int i;

 memset(w, 0, sizeof(*w));
 w->valid = true;
 if (p->transaction_sequence != U64_MAX) ++p->transaction_sequence;
 w->sequence = p->transaction_sequence;
 w->kind = kind;
 w->sample_ms = sample_ms;
 w->policy_sample_ms = p->result.input.sample_ms;
 w->started_ms = t380_source_now();
 w->stage = stage;
 w->owner_return = w->pause_return = w->resume_return = -ENODATA;
 w->phase_ack = w->policy_ack = -ENODATA;
 w->cleanup_error = -ENODATA;
 w->wait_first = w->wait_last = w->extra_status = -ENODATA;
 w->ic.error = w->ic.off_error = w->ic.verified = -ENODATA;
 w->ic.status_before = w->ic.status_after = -ENODATA;
 w->ic.verification.valid = -ENODATA;
 for (i = 0; i < T380_CS_COUNT; ++i)
  w->ic.before[i] = w->ic.after[i] = -ENODATA;
}
static void t380_pp_phase_attempt(struct t380_periodic *p, u64 sample_ms)
{
 t380_pp_transaction_attempt(p, &p->phase_apply, T380_TX_SECOND,
  sample_ms, T380_PHASE_PAUSE);
}
static int t380_pp_transaction_result(struct t380_phase_apply_witness *w, int error)
{
 /* The first returned boundary error is not an assertion about a hidden
  * owner's initiating predicate; attached_owner retains its latched cause. */
 if (error && !w->first_error) w->first_error = error;
 w->ended_ms = t380_source_now();
 return error;
}
static int t380_pp_phase_result(struct t380_periodic *p, int error)
{
 return t380_pp_transaction_result(&p->phase_apply, error);
}
static int t380_pp_phase_wait_guard(struct t380_periodic *p,
 const struct t380_ic_io *io, u64 deadline, u64 *previous)
{
 struct t380_phase_apply_witness *w = &p->phase_apply;
 u64 now;
 int ret = io->guard(io->context);

 if (ret) return ret;
 now = t380_source_now();
 w->wait_elapsed_ms = now >= w->wait_started_ms ? now - w->wait_started_ms : 0;
 if (now < *previous) return -ERANGE;
 *previous = now;
 if (!READ_ONCE(p->running)) return -ESHUTDOWN;
 if (!w->sample_ms || now < w->sample_ms ||
     now - w->sample_ms > T380_SS_SAMPLE_MAX_AGE_MS) return -ESTALE;
 if (now >= deadline) return -EIO;
 return 0;
}
static int t380_pp_phase_wait_off(struct t380_periodic *p,
 const struct t380_ic_io *io)
{
 struct t380_phase_apply_witness *w = &p->phase_apply;
 u64 previous, deadline;
 int ret, after, raw, i;

 w->wait_started_ms = t380_source_now();
 previous = w->wait_started_ms;
 if (previous < w->started_ms || previous > U64_MAX - T380_PP_PHASE_WAIT_MS)
  return -ERANGE;
 deadline = previous + T380_PP_PHASE_WAIT_MS;
 for (i = 0; i < T380_PP_PHASE_WAIT_READS; ++i) {
  ret = t380_pp_phase_wait_guard(p, io, deadline, &previous);
  if (ret) return ret;
  raw = io->read(io->context, 0x0a);
  ++w->wait_reads;
  if (w->wait_reads == 1) w->wait_first = raw;
  w->wait_last = raw;
  ret = raw < 0 ? raw : raw > 255 ? -EIO : 0;
  /* Keep a genuine read error even if the following guard also fails. */
  after = t380_pp_phase_wait_guard(p, io, deadline, &previous);
  if (ret) return ret;
  if (after) return after;
  if (!(raw & 1)) return 0;
  if (i + 1 == T380_PP_PHASE_WAIT_READS) return -EIO;
  usleep_range(5000, 10000);
  ret = t380_pp_phase_wait_guard(p, io, deadline, &previous);
  if (ret) return ret;
 }
 return -EIO;
}
static void t380_pp_phase_ack_result(struct t380_periodic *p, int error)
{
 if (!p->phase_apply.first_error) {
  p->phase_apply.phase_ack = error;
  p->phase_apply.stage = error ? T380_PHASE_ACK : T380_PHASE_DONE;
  t380_pp_phase_result(p, error);
 }
}
static void t380_pp_phase_cleanup(struct t380_periodic *p,
 const struct t380_pp_cycle *c)
{
 int ret = t380_charge_trial_revoke_off(&p->charger->dev);

 if (p->phase_apply.valid && p->phase_apply.first_error &&
     p->phase_apply.sample_ms == c->ended_ms)
  p->phase_apply.cleanup_error = ret;
}
static int t380_pp_transaction_show(const struct t380_phase_apply_witness *w,
 char *buf, int capacity)
{
 int target = w->kind == T380_TX_FULL ? T380_CP_FULL :
  w->kind == T380_TX_RECHARGE ? T380_CP_FIRST : T380_CP_SECOND;
 /* Formatting is cached-only.
  * ic_final_off is the IC primitive's final cleanup, not its first off. */
 return scnprintf(buf, capacity,
  "transaction_kind=%d valid=%d sequence=%llu target_phase=%d stage=%d first_error=%d cleanup_error=%d sample_ms=%llu policy_sample_ms=%llu started_ms=%llu ended_ms=%llu\n"
  "owner_return=%d pause_return=%d resume_return=%d phase_ack=%d policy_ack=%d\n"
  "wait_bound_ms=%d wait_started_ms=%llu wait_elapsed_ms=%llu wait_reads=%d wait_first=%d wait_last=%d extra_status=%d\n"
  "ic_ran=%d ic_error=%d ic_final_off=%d ic_verified=%d ic_reads=%d ic_writes=%d ic_status_before=%d ic_status_after=%d ic_verification_valid=%d ic_mismatch=%u\n"
  "ic_before=%d,%d,%d,%d,%d,%d,%d ic_after=%d,%d,%d,%d,%d,%d,%d\n",
  w->kind, w->valid, (unsigned long long)w->sequence, target, w->stage, w->first_error,
  w->cleanup_error, (unsigned long long)w->sample_ms,
  (unsigned long long)w->policy_sample_ms,
  (unsigned long long)w->started_ms, (unsigned long long)w->ended_ms,
  w->owner_return, w->pause_return, w->resume_return, w->phase_ack, w->policy_ack,
  w->kind == T380_TX_SECOND ? 100 : 0,
  (unsigned long long)w->wait_started_ms, (unsigned long long)w->wait_elapsed_ms,
  w->wait_reads, w->wait_first, w->wait_last, w->extra_status,
  w->ic_ran, w->ic.error, w->ic.off_error, w->ic.verified, w->ic.reads,
  w->ic.writes, w->ic.status_before, w->ic.status_after,
  w->ic.verification.valid, w->ic.verification.mismatch,
  w->ic.before[0], w->ic.before[1], w->ic.before[2], w->ic.before[3],
  w->ic.before[4], w->ic.before[5], w->ic.before[6],
  w->ic.after[0], w->ic.after[1], w->ic.after[2], w->ic.after[3],
  w->ic.after[4], w->ic.after[5], w->ic.after[6]);
}
static ssize_t phase_apply_show(struct device *dev,
 struct device_attribute *attr, char *buf)
{
 struct t380_control *control = i2c_get_clientdata(to_i2c_client(dev));
 const struct t380_periodic *p = control->periodic;
 const struct t380_rest_stop_witness *r = &p->rest_stop;
 int length;

 mutex_lock(&control->lock);
 length = scnprintf(buf, PAGE_SIZE, "cached_only=1 permission=0 phase_apply_v2=1\n");
 length += t380_pp_transaction_show(&p->phase_apply, buf + length, PAGE_SIZE - length);
 length += t380_pp_transaction_show(&p->full_apply, buf + length, PAGE_SIZE - length);
 length += t380_pp_transaction_show(&p->recharge_apply, buf + length, PAGE_SIZE - length);
 length += scnprintf(buf + length, PAGE_SIZE - length,
  "rest_stop_valid=%d sequence=%llu sample_ms=%llu policy_sample_ms=%llu reason=%d first_return_error=%d cleanup_error=%d\n",
  r->valid, (unsigned long long)r->sequence, (unsigned long long)r->sample_ms,
  (unsigned long long)r->policy_sample_ms, r->reason, r->error, r->cleanup_error);
 mutex_unlock(&control->lock);
 return length;
}
static DEVICE_ATTR_RO(phase_apply);
static int t380_pp_phase_transition_locked(struct t380_periodic *p,
		struct t380_control *control, const struct t380_pp_cycle *c)
{
	struct i2c_client *client = p->charger;
	const struct t380_cs_limits limits = { t380_pp_session_input_ma(p), 500, t380_pp_session_cv_mv(p), 250 };
	struct t380_cs_plan managed;
	struct t380_phase_apply_witness *w = &p->phase_apply;
	struct t380_ic_io io = {
		.context = client, .off = t380_pp_phase_high,
		.guard = t380_pp_phase_high, .read = t380_control_read,
		.write = t380_control_write,
	};
	int ret;

	t380_pp_phase_attempt(p, c->ended_ms);
	ret = t380_charge_phase_pause(&client->dev, c->ended_ms);
 w->pause_return = ret;
	if (ret) return t380_pp_phase_result(p, ret);
	w->stage = T380_PHASE_ENCODE;
	ret = t380_cs_encode_profile(&limits, T380_CS_MANAGED_ATTACHED,
		&managed);
	if (ret) return t380_pp_phase_result(p, ret);
 w->stage = T380_PHASE_WAIT;
 ret = t380_pp_phase_wait_off(p, &io);
 if (ret) return t380_pp_phase_result(p, ret);
 w->stage = T380_PHASE_APPLY;
 w->ic_ran = true;
 ret = t380_ic_apply(&io, &managed, &w->ic);
 if (!ret && !w->ic.verified) ret = -EIO;
	if (!ret) {
		/* The masked full readback succeeded while verified HIGH. */
		control->plan = managed;
		w->stage = T380_PHASE_EXTRA_STATUS;
		ret = i2c_smbus_read_byte_data(client, 0x0a);
		w->extra_status = ret;
		if (ret >= 0 && (ret & 1)) ret = -EIO;
	}
 if (ret < 0) return t380_pp_phase_result(p, ret);
 w->stage = T380_PHASE_RUNNING;
 if (!READ_ONCE(p->running)) return t380_pp_phase_result(p, -ESHUTDOWN);
 w->stage = T380_PHASE_RESUME;
 ret = t380_charge_phase_resume(&client->dev, c->ended_ms);
 w->resume_return = ret;
 if (!ret) w->stage = T380_PHASE_ACK;
 return t380_pp_phase_result(p, ret);
}
static int t380_pp_recharge_transition_locked(struct t380_periodic *p,
		struct t380_control *control, const struct t380_pp_cycle *c)
{
	struct i2c_client *client = p->charger;
	const struct t380_cs_limits limits = { t380_pp_session_input_ma(p), 500, t380_pp_session_cv_mv(p), 350 };
	struct t380_cs_plan managed;
	struct t380_phase_apply_witness *w = &p->recharge_apply;
	struct t380_ic_io io = {
		.context = client, .off = t380_pp_phase_high,
		.guard = t380_pp_phase_high, .read = t380_control_read,
		.write = t380_control_write,
	};
	int ret;

 t380_pp_transaction_attempt(p, w, T380_TX_RECHARGE, c->ended_ms, T380_PHASE_PAUSE);
	ret = t380_charge_recharge_pause(&client->dev, c->ended_ms);
 w->pause_return = ret;
	if (ret) return t380_pp_transaction_result(w, ret);
 w->stage = T380_PHASE_ENCODE;
	ret = t380_cs_encode_profile(&limits, T380_CS_MANAGED_ATTACHED,
		&managed);
 if (ret) return t380_pp_transaction_result(w, ret);
 w->stage = T380_PHASE_APPLY;
 w->ic_ran = true;
 ret = t380_ic_apply(&io, &managed, &w->ic);
 if (!ret && !w->ic.verified) ret = -EIO;
	if (!ret) {
		control->plan = managed;
  w->stage = T380_PHASE_EXTRA_STATUS;
		ret = i2c_smbus_read_byte_data(client, 0x0a);
  w->extra_status = ret;
		if (ret >= 0 && (ret & 1)) ret = -EIO;
	}
 if (ret < 0) return t380_pp_transaction_result(w, ret);
 w->stage = T380_PHASE_RUNNING;
 if (!READ_ONCE(p->running)) return t380_pp_transaction_result(w, -ESHUTDOWN);
 w->stage = T380_PHASE_RESUME;
 ret = t380_charge_recharge_resume(&client->dev, c->ended_ms);
 w->resume_return = ret;
 if (!ret) w->stage = T380_PHASE_ACK;
 return t380_pp_transaction_result(w, ret);
}
#endif
/* Caller holds control->lock. Preserve a real cycle/GPIO error over the
 * typed autonomous-stop diagnosis; the owner revokes LOW before HIGH. */
static void t380_pp_session_refuse_locked(struct t380_periodic *p)
{
	int ret;

	p->session_error = p->result.cycle_error ?
		p->result.cycle_error : -EPERM;
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
	if (!p->result.cycle_error &&
	    p->phase_candidate.status == T380_PS_TERMINAL) {
		if (p->phase_candidate.reason == T380_CP_HARDWARE_DONE_INCOMPLETE)
			p->session_error = -ENOMSG;
		else if (p->phase_candidate.reason == T380_CP_HARDWARE_STATE_MISMATCH)
			p->session_error = -EPROTO;
	}
#endif
	ret = t380_charge_trial_revoke_off(&p->charger->dev);
	if (ret) p->session_error = ret;
	WRITE_ONCE(p->session_active, false);
}
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
static void t380_pp_regular_sample(struct t380_periodic *p,
 const struct t380_pp_cycle *c)
{
 struct t380_battery_thermal *sensor = i2c_get_clientdata(p->dependencies->gauge);
 bool charging, candidate;
 if (!t380_household_window_regular(p->household_window)) return;
 mutex_lock(&sensor->acquisition_lock);
 sensor->regular_full_off = false;
 p->result.input.regular_profile = true;
 p->result.input.regular_model_matched = sensor->regular_model_matched;
 p->result.input.age_step = 0; /* stock default, measured age remains unknown */
 /* Source-only charging refusal must not erase fresh nonlearning capacity.
  * integrity_error retains physical -EAGAIN, epoch and freshness failures. */
 if (p->result.integrity_error || p->result.latched_error ||
     !sensor->regular_model_matched || c->ended_ms < c->started_ms ||
     c->ended_ms - c->started_ms > T380_PP_ACQUISITION_LIMIT_MS) {
  sensor->capacity.valid = false;
  mutex_unlock(&sensor->acquisition_lock);
  return;
 }
 charging = p->result.acquisition_valid && c->trial_active &&
  c->charger[0].chgon && c->charger[1].chgon;
 p->result.input.reported_capacity = sensor->capacity.capacity_old;
 candidate = p->household_enabled &&
  t380_ps_regular_full_candidate(&p->phase_shadow, c, &p->result);
 if (t380_rf_capacity_update(&sensor->capacity, c->battery.field[T380_PA_SOC].raw,
  c->ended_ms, c->battery.field[T380_PA_SOC].started_ms, charging, candidate))
  t380_ga_accept_conversion(sensor, &c->battery, c->ended_ms);
 p->result.input.reported_capacity = sensor->capacity.capacity_old;
 if (!sensor->capacity.valid) p->result.input.regular_model_matched = false;
 mutex_unlock(&sensor->acquisition_lock);
}
/* Publish only after this cycle's final rest/transaction/error dispatch.
 * This cached reporting decision performs no hardware read or control action. */
static void t380_pp_regular_finish(struct t380_periodic *p,
 const struct t380_pp_cycle *c)
{
 struct t380_battery_thermal *sensor;
 struct t380_control *control;
 bool full_off;
 if (!t380_household_window_regular(p->household_window)) return;
 sensor = i2c_get_clientdata(p->dependencies->gauge);
 control = i2c_get_clientdata(p->charger);
 mutex_lock(&sensor->acquisition_lock);
 full_off = p->result.input.now_ms == c->ended_ms &&
  p->result.acquisition_valid && !p->result.integrity_error &&
  !p->result.latched_error && !p->result.cycle_error && !control->fault &&
  p->phase_candidate.status == T380_PS_BOUND &&
  p->phase_candidate.phase == T380_CP_FULL &&
  p->phase_candidate.reason == T380_CP_FULL_STOP &&
  p->household_enabled && READ_ONCE(p->session_rest) &&
  !READ_ONCE(p->session_active) && p->phase_shadow.policy.phase == T380_CP_FULL &&
  !p->phase_shadow.policy.episode_open && !p->phase_shadow.policy.full_pending &&
  !p->phase_shadow.policy.fault_latched && !p->session_error &&
  c->source.fixed5v_qualified && c->source.fixed5v_input_allowance_ma == 900 &&
  !c->charger[0].chgon && !c->charger[1].chgon &&
  c->started_ms > p->phase_shadow.policy.last_full_sample_ms &&
  sensor->regular_model_matched && sensor->capacity.valid &&
  sensor->capacity.capacity_old == 100;
 sensor->regular_full_off = full_off;
 sensor->regular_full_off_ms = full_off ? c->ended_ms : 0;
 mutex_unlock(&sensor->acquisition_lock);
}
#endif
static bool t380_pp_session_decide(struct t380_periodic *p,
		const struct t380_pp_cycle *c)
{
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
	if (p->session_phase_selected) {
		t380_ps_observe(&p->phase_shadow, c, &p->result,
			&p->phase_candidate);
		p->session_decision = p->phase_shadow.decision;
		if (p->phase_candidate.status == T380_PS_TERMINAL &&
		    (p->phase_candidate.reason == T380_CP_HARDWARE_DONE_INCOMPLETE ||
		     p->phase_candidate.reason == T380_CP_HARDWARE_STATE_MISMATCH))
			return false;
		if (p->session_recharge_selected &&
		    READ_ONCE(p->session_rest) &&
		    p->phase_candidate.status == T380_PS_BOUND &&
		    p->phase_candidate.recharge_candidate &&
		    p->phase_candidate.candidate_plan &&
		    p->phase_candidate.phase == T380_CP_FIRST)
			return true;
		if (p->phase_candidate.status == T380_PS_BOUND &&
		    p->phase_candidate.phase == T380_CP_FULL &&
		    p->phase_candidate.reason == T380_CP_FULL_STOP)
			return false;
		if (p->phase_candidate.status != T380_PS_BOUND ||
		    !p->phase_candidate.candidate_plan ||
		    p->phase_candidate.recharge_candidate ||
		    (!READ_ONCE(p->session_active) &&
		     ((p->result.input.voltage_mv > t380_pp_session_initial_mv(p)
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
		       && !p->household_rebind_pending
#endif
		       ) ||
		      (p->phase_candidate.phase != T380_CP_FIRST
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
		       && !(p->household_rebind_pending &&
			    p->phase_candidate.phase == T380_CP_SECOND)
#endif
		      )))) {
			p->session_decision = (struct t380_cp_output) {
				.reason = T380_CP_INVALID_ARGUMENT };
			return false;
		}
		return true;
	}
#endif
	/* Only the explicitly selected nearby-voltage session admits 3800 mV.
	 * Older Belkin sessions retain 3900 and DCP retains 4050 mV. */
	if (p->result.input.voltage_mv <
	    (p->session_fixed5v ?
	     (p->session_window == T380_BELKIN_SESSION_WINDOW_3800 ?
	      3800 : 3900) : 4050)) {
		p->session_decision = (struct t380_cp_output) {
			.reason = T380_CP_INVALID_ARGUMENT };
		return false;
	}
	return t380_trial_decide(&p->session_policy, c, &p->result,
		true, p->session_fixed5v, p->session_fixed5v,
		p->session_window,
		&p->session_decision);
}
#endif
#endif
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
static bool t380_pp_household_window_valid(
		enum t380_household_voltage_window window)
{
	return t380_household_window_floor_mv(window) >= 0;
}

static const char *t380_pp_household_profile(
		enum t380_household_voltage_window window)
{
	return window == T380_HOUSEHOLD_WINDOW_REGULAR900_3900_V1 ?
        "stock_default_age_unknown4300_4230-regular900-3900-v1" :
        window == T380_HOUSEHOLD_WINDOW_REGULAR900_NORMAL_3600_V1 ?
        "stock_default_age_unknown4300_4230-regular900-normal3600-v1" :
        window == T380_HOUSEHOLD_WINDOW_OPERATOR900_3900_V1 ?
        "household-operator900-3900mv-input900-fast500-v1" :
        window == T380_HOUSEHOLD_WINDOW_OPERATOR900_NORMAL_3600_V1 ?
        "household-operator900-normal-3600mv-input900-fast500-v1" :
        window == T380_HOUSEHOLD_WINDOW_ATTENDED_3800 ?
		"household-attended-3800mv-500ma" :
		window == T380_HOUSEHOLD_WINDOW_NORMAL_3600_V1 ?
		"household-normal-3600mv-500ma-v1" :
		window == T380_HOUSEHOLD_WINDOW_3900 ?
		"household-3900mv-500ma" : "household-invalid";
}

/* Called only after the real tuple passes the full integrity check. */
static int t380_pp_household_voltage_screen(
		enum t380_household_voltage_window window, bool detached,
		int voltage_mv)
{
	if (!t380_pp_household_window_valid(window))
		return -EINVAL;
	if (voltage_mv < t380_household_window_floor_mv(window))
		return -ERANGE;
	if (voltage_mv > (detached ? t380_household_window_cv_mv(window) :
        t380_household_window_initial_mv(window)))
		return -EOVERFLOW;
	return 1;
}

static const char *t380_pp_household_admission_refusal(int refusal)
{
	return refusal == -ERANGE ? "voltage_low" :
		refusal == -EOVERFLOW ? "voltage_high" :
		refusal == -EINVAL ? "selector_invalid" : "none";
}

/* Qualification only: no owner reservation, register plan or LOW operation.
 * This consumes the actual fully checked inhibited tuple even when ordinary
 * acquisition validity is withheld solely for an unqualified suspended bus. */
static bool t380_pp_household_source_fresh(const struct t380_sl_snapshot *s)
{
	return s->fixed5v_candidate && s->reason == T380_SL_FIXED5V_CANDIDATE &&
		s->clean_absence && !s->host_blocked && !s->system_suspended &&
		s->now_ms >= s->oldest_ms && s->now_ms - s->oldest_ms <= 2000;
}

static void t380_pp_household_unqualify(struct t380_periodic *p)
{
	p->household_qualification_pending = false;
	p->household_qualification_generation = 0;
	p->household_qualification_ms = 0;
	t380_source_unqualify_fixed5v();
}

static int t380_pp_household_stop_locked(struct t380_periodic *p, int error)
{
 int off_error;
	p->session_error = error ? error : -EPERM;
	p->session_requested = false;
 p->household_quiet_pending = p->household_quiet_retry = false;
 p->household_quiet_sample_ms = 0;
	WRITE_ONCE(p->session_active, false);
	WRITE_ONCE(p->session_rest, false);
	p->household_detaching = false;
	p->household_detached = false;
	p->household_rebind_pending = false;
	off_error = t380_charge_trial_revoke_off(&p->charger->dev);
	t380_pp_household_unqualify(p);
 return off_error;
}

/* Physical errors take precedence over a simultaneous detach cancellation. */
static int t380_pp_battery_error(const struct t380_pa_tuple *battery)
{
	int i;
	for (i = 0; i < T380_PA_FIELDS; ++i)
		if (battery->field[i].error) return battery->field[i].error;
	if (battery->guard_before_error) return battery->guard_before_error;
	return battery->guard_after_error;
}

/* Error numbers alone never prove guard cancellation: I2C can return the
 * same errno. Preserve genuine read errors and their original diagnostics. */
static bool t380_pp_mark_detach_cancellation(struct t380_pp_cycle *c)
{
	bool cancelled = false;
	int i;
	for (i = 0; i < 2; ++i)
		if (c->charger_error[i] == -ECANCELED &&
		    c->charger[i].guard_cancelled) {
			c->charger_error[i] = -EAGAIN;
			cancelled = true;
		}
	if (c->muic_error == -ECANCELED && !c->muic.read_error &&
	    (c->muic.guard_before_error == -ECANCELED ||
	     c->muic.guard_after_error == -ECANCELED)) {
		c->muic_error = -EAGAIN;
		cancelled = true;
	}
	if (c->battery_error == -ECANCELED && c->battery.guard_cancelled &&
	    (c->battery.guard_before_error == -ECANCELED ||
	     c->battery.guard_after_error == -ECANCELED)) {
		c->battery_error = -EAGAIN;
		cancelled = true;
	}
	return cancelled;
}

/* A transitional partial-absence frame is discarded after immediate HIGH.
 * Only a later complete inhibited acquisition may pause an episode. */
static void t380_pp_household_detach_cycle_locked(struct t380_periodic *p,
				 const struct t380_pp_cycle *c)
{
	int ret = t380_charge_session_check(&p->charger->dev);

	if (ret != -EAGAIN) {
		t380_pp_household_stop_locked(p, ret);
		return;
	}
	if ((c->charger_error[0] && c->charger_error[0] != -EAGAIN) ||
	    (c->charger_error[1] && c->charger_error[1] != -EAGAIN) ||
	    (c->muic_error && c->muic_error != -EAGAIN) ||
	    (c->battery_error && c->battery_error != -EAGAIN) ||
	    (c->readback_error && c->readback_error != -EAGAIN)) {
		t380_pp_household_stop_locked(p, -EIO);
		return;
	}
	if (c->detach_cancelled) return;
	if (!c->source.fresh_absence) {
		if (!c->source.absence_pending &&
		    !c->source.ambiguous_disconnect)
			t380_pp_household_stop_locked(p, -ESTALE);
		return;
	}
	/* The transitional frame may contain an interrupted collector guard.
	 * Require the next wholly inhibited frame for weighted debit and proof. */
	if (c->off_before_error == -EAGAIN ||
	    c->off_after_error == -EAGAIN ||
	    c->charger_error[0] || c->charger_error[1] || c->muic_error ||
	    c->battery_error || c->readback_error)
		return;
	if (!c->battery.tuple_valid || !c->muic.valid || !c->muic.stable ||
	    !c->charger[0].valid || !c->charger[1].valid ||
	    c->charger[0].chgon || c->charger[1].chgon ||
	    c->charger[0].vbusovp || c->charger[1].vbusovp ||
	    c->charger[0].vbus_conflict || c->charger[1].vbus_conflict ||
	    c->charger[0].otgfail || c->charger[1].otgfail ||
	    (c->charger[0].vbusuvlo &&
	     !t380_co_expected_absent_uvlo(&c->charger[0], false)) ||
	    (c->charger[1].vbusuvlo &&
	     !t380_co_expected_absent_uvlo(&c->charger[1], false))) {
		t380_pp_household_stop_locked(p, -EIO);
		return;
	}
	ret = t380_charge_household_detach_finish(&p->charger->dev);
	if (!ret && !t380_charge_policy_household_detach(
	    &p->phase_shadow.policy, t380_source_now())) ret = -EPERM;
	if (ret) {
		t380_pp_household_stop_locked(p, ret);
		return;
	}
	p->household_detaching = false;
	p->household_detached = true;
	t380_ps_household_rebind(&p->phase_shadow);
	t380_source_unqualify_fixed5v();
}

static void t380_pp_household_admit_locked(struct t380_periodic *p,
		struct t380_control *control, const struct t380_pp_cycle *c)
{
	struct t380_sl_snapshot live;
	int ret, voltage_screen;

	if (!p->household_enabled || p->household_detaching ||
	    p->household_quiet_pending || p->session_requested || READ_ONCE(p->session_active) ||
	    READ_ONCE(p->session_rest) ||
	    (p->session_spent && !p->household_detached)) {
		if (p->household_qualification_pending)
			t380_pp_household_unqualify(p);
		return;
	}
	if (!p->household_absence_seen) {
		if (p->household_qualification_pending)
			t380_pp_household_unqualify(p);
		if (c->source.fresh_absence &&
		    c->source.absence_oldest_ms >= p->household_enable_ms)
			p->household_absence_seen = true;
		return;
	}
	/* Bus-only EAGAIN is allowed solely at qualification. integrity_error
	 * includes every physical check regardless of that earlier refusal. */
	if (!READ_ONCE(p->running) || control->fault || c->trial_active ||
	    p->result.integrity_error || p->result.latched_error ||
	    (p->result.cycle_error && p->result.cycle_error != -EAGAIN) ||
	    !t380_pp_household_source_fresh(&c->source) ||
	    c->battery.field[T380_PA_TEMP].value < 200 ||
	    c->battery.field[T380_PA_TEMP].value > 350) {
		t380_pp_household_unqualify(p);
		return;
	}
	/* The integrity check established this real tuple's validity. Inspect its
	 * selected voltage directly, without promoting copied policy inputs. */
	voltage_screen = t380_pp_household_voltage_screen(p->household_window,
		p->household_detached, c->battery.field[T380_PA_VOLTAGE].value / 1000);
	if (voltage_screen < 0) {
		p->household_admission_refusal = voltage_screen;
		t380_pp_household_unqualify(p);
		if (voltage_screen == -EINVAL)
			t380_pp_household_stop_locked(p, voltage_screen);
		return;
	}
	p->household_admission_refusal = 0;
	t380_pp_source_copy(&live);
	if (!t380_pp_household_source_fresh(&live) ||
	    live.generation != c->source.generation ||
	    live.now_ms < c->ended_ms || live.now_ms - c->ended_ms > 2000) {
		t380_pp_household_unqualify(p);
		return;
	}
	if (!p->household_qualification_pending) {
		if (!(t380_pp_session_input_ma(p) == 900 ?
		      t380_source_qualify_operator900() : t380_source_qualify_fixed5v())) {
			t380_pp_household_unqualify(p);
			return;
		}
		t380_pp_source_copy(&live);
		if (!READ_ONCE(p->running) ||
		    !t380_pp_household_source_fresh(&live) ||
		    !live.fixed5v_qualified || !t380_pp_input_contract(p, &live) ||
		    live.generation != c->source.generation ||
		    live.now_ms < c->ended_ms || live.now_ms - c->ended_ms > 2000) {
			t380_pp_household_unqualify(p);
			return;
		}
		p->household_qualification_pending = true;
		p->household_qualification_generation = live.generation;
		p->household_qualification_ms = c->ended_ms;
		/* Normal tail schedules the next genuine complete acquisition at the
		 * existing cadence; do not acquire another gauge current here. */
		return;
	}
	if (!p->result.acquisition_valid || p->result.cycle_error ||
	    !live.fixed5v_qualified || !c->source.fixed5v_qualified ||
	    !t380_pp_input_contract(p, &live) || !t380_pp_input_contract(p, &c->source) ||
	    live.generation != p->household_qualification_generation ||
	    c->started_ms <= p->household_qualification_ms ||
	    c->ended_ms - p->household_qualification_ms < T380_PA_HISTORY_MIN_MS ||
	    c->ended_ms - p->household_qualification_ms > T380_PA_HISTORY_MAX_MS) {
		t380_pp_household_unqualify(p);
		return;
	}
	/* Only this subsequent ordinary valid acquisition may reserve. */
	p->household_qualification_pending = false;
	if (p->household_detached) {
		ret = t380_charge_household_rebind(&p->charger->dev,
			live.generation);
		if (!ret && !t380_charge_policy_household_rebind(
		    &p->phase_shadow.policy, c->ended_ms)) ret = -EPERM;
		if (ret) {
			t380_pp_household_stop_locked(p, ret);
			return;
		}
		p->household_detached = false;
		if (p->household_detached_rest) {
			WRITE_ONCE(p->session_rest, true);
			p->household_detached_rest = false;
			return;
		}
		p->household_rebind_pending = true;
	} else {
		ret = t380_charge_household_reserve(&p->charger->dev,
			live.generation);
		if (!ret && !t380_charge_policy_household_mode(
		    &p->phase_shadow.policy)) ret = -EPERM;
		if (ret) {
			t380_pp_household_stop_locked(p, ret);
			return;
		}
		p->session_spent = true;
		p->session_fixed5v = true;
		p->session_phase_selected = true;
		p->session_recharge_selected = true;
		p->session_attached_selected = true;
		if (p->phase_shadow.household_window != p->household_window) {
			t380_pp_household_stop_locked(p, -EINVAL);
			return;
		}
		p->session_window = T380_TRIAL_WINDOW_3900;
		p->session_budget_ms = T380_SS_ATTACHED_INITIAL_MS;
	}
 if (!p->household_rebind_pending) {
  struct t380_household_quiet_snapshot owner;
  ret = t380_charge_household_quiet_snapshot(&p->charger->dev, &owner);
  if (ret || !owner.pending) {
   t380_pp_household_stop_locked(p, ret ? ret : -EPERM);
   return;
  }
  t380_pp_startup_reset(p, &c->source);
  p->household_quiet_pending = true;
  p->household_quiet_timeout = p->household_quiet_retry = false;
  p->household_quiet_deadline_ms = owner.deadline_ms;
  p->household_quiet_generation = c->source.generation;
  p->household_quiet_fault_serial = c->source.fault_serial;
  p->household_quiet_pm_serial = c->source.pm_serial;
  p->household_quiet_sample_ms = p->household_quiet_epoch = 0;
  p->session_requested = false;
 } else {
  p->session_requested = true;
  p->arm_not_before_ms = t380_source_now() + T380_PP_ARM_SAMPLE_DELAY_MS;
  mod_delayed_work(system_wq, &p->work,
   msecs_to_jiffies(T380_PP_ARM_SAMPLE_DELAY_MS));
 }
}
#endif
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
/* A logical quiet deadline is recoverable by ordinary fresh-absence ACK.
 * Acquisition OFF fields describe physical HIGH/verification only. Never
 * classify by errno alone: an actual HIGH failure may also be -ETIME. */
static int t380_pp_household_quiet_off(struct device *dev)
{
 struct t380_household_quiet_snapshot owner;
 int ret = t380_charge_household_quiet_check(dev);
 int snapshot_error;
 if (ret != -ETIME) return ret;
 snapshot_error = t380_charge_household_quiet_snapshot(dev, &owner);
 if (snapshot_error) return snapshot_error;
 if (owner.pending || !owner.expired || owner.error != -ETIME ||
     owner.off_error) return ret;
 /* Independently prove current HIGH, retaining any physical failure. */
 return t380_charge_force_off(dev);
}
static bool t380_pp_household_quiet_cycle_locked(struct t380_periodic *p,
 struct t380_control *control, const struct t380_pp_cycle *c,
 const struct t380_display_snapshot *before,
 const struct t380_display_snapshot *after)
{
 struct t380_household_quiet_snapshot owner;
 int ret = t380_charge_household_quiet_snapshot(&p->charger->dev, &owner);
 p->household_quiet_display = *after;
 if (ret || !owner.pending) {
  int error = ret ? ret : owner.error ? owner.error : -EPERM;
  p->household_quiet_timeout = !ret && owner.expired &&
   !owner.off_error && error == -ETIME;
  ret = t380_pp_household_stop_locked(p, error);
  if (ret) {
   /* A later HIGH failure must not become recoverable just because the
    * initiating stop was a successfully verified logical deadline. */
   int first_error = p->result.cycle_error &&
    p->result.cycle_error != -EAGAIN && p->result.cycle_error != -ESTALE ?
    p->result.cycle_error : ret;
   if (p->household_quiet_timeout) p->session_error = first_error;
   p->household_quiet_timeout = false;
   if (!p->state.latched_error) p->state.latched_error = first_error;
   t380_control_fault_locked(p->charger, first_error);
  }
  return false;
 }
 if (control->fault || p->result.integrity_error || p->result.latched_error ||
     p->result.cycle_error || c->trial_active ||
     !c->battery.tuple_valid || !c->battery.history_ready ||
     c->charger[0].chgon || c->charger[1].chgon ||
     c->charger[0].done != c->charger[1].done ||
     c->charger[0].topoff != c->charger[1].topoff ||
     !t380_pp_household_source_fresh(&c->source) ||
     !c->source.fixed5v_qualified || !t380_pp_input_contract(p, &c->source) ||
     c->source.generation != p->household_quiet_generation ||
     c->source.fault_serial != p->household_quiet_fault_serial ||
     c->source.pm_serial != p->household_quiet_pm_serial ||
     c->battery.field[T380_PA_TEMP].value < 200 ||
     c->battery.field[T380_PA_TEMP].value > 350 ||
     t380_pp_household_voltage_screen(p->household_window, false,
       c->battery.field[T380_PA_VOLTAGE].value / 1000) < 0) {
  int error = control->fault ? control->fault : p->result.cycle_error ?
   p->result.cycle_error : -ESTALE;
  t380_pp_household_stop_locked(p, error);
  return false;
 }
 if (!before->known_off || !after->known_off || !before->epoch ||
     before->epoch != after->epoch ||
     before->off_completed_ms != after->off_completed_ms ||
     !before->off_completed_ms || c->started_ms < before->off_completed_ms) {
  p->household_quiet_sample_ms = p->household_quiet_epoch = 0;
  return false;
 }
 if (p->household_quiet_sample_ms && p->household_quiet_epoch == after->epoch &&
     c->battery.field[T380_PA_CURRENT].started_ms > p->household_quiet_sample_ms &&
     c->battery.field[T380_PA_CURRENT].started_ms - p->household_quiet_sample_ms >= T380_PA_HISTORY_MIN_MS &&
     c->battery.field[T380_PA_CURRENT].started_ms - p->household_quiet_sample_ms <= T380_PA_HISTORY_MAX_MS)
  return true;
 p->household_quiet_epoch = after->epoch;
 p->household_quiet_sample_ms = c->battery.field[T380_PA_CURRENT].started_ms;
 return false;
}
#endif
/* Separate owned actor lifetime cancellation from independently observed
 * physical errors. Pure policy is unchanged; the discarded cycle cannot enter
 * any enable/LOW dispatch. Original diagnostics retain every typed outcome. */
static bool t380_pp_evaluate_cycle(struct t380_pp_state *state,
 const struct t380_pp_cycle *c, struct t380_pp_result *result)
{
 bool owned = c->battery.guard_cancelled && !c->battery.acquisition_physical_error &&
  (c->battery.acquisition_cancel_reason == T380_GA_CANCEL_DISABLE ||
   c->battery.acquisition_cancel_reason == T380_GA_CANCEL_PM ||
   c->battery.acquisition_cancel_reason == T380_GA_CANCEL_STOP ||
   c->battery.acquisition_cancel_reason == T380_GA_CANCEL_REMOVE);
 if (owned) {
  struct t380_pp_cycle physical = *c;
  physical.battery_error = 0;
  if (physical.charger[0].guard_cancelled) physical.charger_error[0] = 0;
  if (physical.charger[1].guard_cancelled) physical.charger_error[1] = 0;
  if (!physical.muic.read_error &&
      (physical.muic.guard_before_error == -ECANCELED ||
       physical.muic.guard_after_error == -ECANCELED)) physical.muic_error = 0;
  if (!physical.off_before_error && !physical.off_after_error &&
      !physical.readback_error && !physical.charger_error[0] &&
      !physical.charger_error[1] && !physical.muic_error) {
   *result = (struct t380_pp_result){ .cycle_error = -EAGAIN,
    .latched_error = state->latched_error };
   return true;
  }
  /* Only typed cancellation is omitted from this policy view. A coincident
   * hardware/OFF/readback error is still evaluated and latched normally. */
  t380_pp_evaluate(state, &physical, result);
 } else t380_pp_evaluate(state, c, result);
 return false;
}
/* Consumed under the existing control lock before any policy/LOW dispatch.
 * Error identity and original request survive even if the canceled caller left. */
static bool t380_pp_late_physical(struct t380_periodic *p, struct t380_pp_cycle *cycle)
{
 struct t380_battery_thermal *sensor = i2c_get_clientdata(p->dependencies->gauge);
 struct t380_ga_outcome physical;
 if (!t380_ga_physical_take(sensor->actor, &physical)) return false;
 cycle->battery.acquisition_request = physical.request;
 cycle->battery.acquisition_lifetime = physical.lifetime;
 cycle->battery.acquisition_cancel_reason = physical.cancel_reason;
 cycle->battery.acquisition_physical_error = physical.physical_error;
 cycle->battery.guard_cancelled = false;
 cycle->battery_error = physical.physical_error;
 return true;
}
static void t380_periodic_work(struct work_struct *work)
{
	struct t380_periodic *p = container_of(to_delayed_work(work), struct t380_periodic, work);
	struct i2c_client *client = p->charger;
	struct t380_control *control = i2c_get_clientdata(client);
	struct t380_pp_cycle *c = &p->last;
	const struct t380_pp_cycle empty = { 0 };
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
 struct t380_display_snapshot quiet_before = {0}, quiet_after = {0};
#endif
	int ret, i;
 u64 off_started = t380_source_now(), off_ended;
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
	bool active = READ_ONCE(p->trial_active);
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
	bool session_active = READ_ONCE(p->session_active);
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
	bool session_rest = READ_ONCE(p->session_rest);
#endif
#endif
#endif

	/* HIGH before waiting on any I2C/sampling lock. This is not a watchdog. */
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
	ret =
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
		p->household_detaching ?
		t380_charge_household_high(&client->dev) :
#endif
		active ? t380_charge_trial_check(&client->dev) :
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
		session_active ? t380_charge_session_check(&client->dev) :
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
		session_rest ? t380_charge_recharge_rest_check(&client->dev) :
#endif
#endif
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
  READ_ONCE(p->household_quiet_pending) ? t380_pp_household_quiet_off(&client->dev) :
#endif
		t380_charge_force_off(&client->dev);

#ifdef CONFIG_T380_CHARGE_SESSION_DEV
	#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
	if (session_active && ret == -EAGAIN && p->household_enabled &&
	    p->session_attached_selected) {
		WRITE_ONCE(p->session_active, false);
		session_active = false;
		p->household_detaching = true;
		ret = 0;
	}
	#endif
	if (session_active && ret) {
		t380_charge_force_off(&client->dev);
		WRITE_ONCE(p->session_active, false);
		session_active = false;
	}
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
	#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
	if (session_rest && ret == -EAGAIN && p->household_enabled &&
	    p->session_attached_selected) {
		WRITE_ONCE(p->session_rest, false);
		session_rest = false;
		p->household_detached_rest = true;
		p->household_detaching = true;
		ret = 0;
	}
	#endif
	if (session_rest && ret) {
		t380_charge_trial_revoke_off(&client->dev);
		WRITE_ONCE(p->session_rest, false);
		session_rest = false;
	}
#endif
#endif
	if (active && ret) {
		t380_charge_force_off(&client->dev);
		WRITE_ONCE(p->trial_active, false);
		#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
		if (p->trial_fixed5v) t380_source_unqualify_fixed5v();
		#endif
		active = false;
	}
#else
	ret = t380_charge_force_off(&client->dev);
#endif
 off_ended = t380_source_now();
	mutex_lock(&control->lock);
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
	/* Arm can race a worker already waiting on this lock. Do not acquire a
	 * second gauge current within the real 1000 ms history interval: that
	 * would reset average validity and spend the one-shot without admission.
	 * The reserve-time session budget continues during this short wait. */
	if (!READ_ONCE(p->running)) {
		mutex_unlock(&control->lock);
		return;
	}
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
	if (p->household_pm_detached) {
		t380_pp_household_unqualify(p);
		ret = t380_charge_household_detached_ready(&client->dev, false);
		if (ret == -EAGAIN) {
			mutex_unlock(&control->lock);
			schedule_delayed_work(&p->work,
				msecs_to_jiffies(T380_PP_INTERVAL_MS));
			return;
		}
		p->household_pm_detached = false;
		if (ret) {
			t380_pp_household_stop_locked(p, ret);
			mutex_unlock(&control->lock);
			schedule_delayed_work(&p->work,
				msecs_to_jiffies(T380_PP_INTERVAL_MS));
			return;
		}
		/* Sleep never carries an old absence ticket or gauge average into
		 * the next exact-source admission on this physical attachment. */
		p->household_absence_seen = false;
		p->household_enable_ms = t380_source_now();
		p->history = (struct t380_pa_history){ 0 };
	}
#endif
	{
		u64 arm_now = t380_source_now();
		bool pending = p->trial_requested
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
			|| p->session_requested
#endif
			;

		if (pending && !ret && arm_now < p->arm_not_before_ms) {
			u64 remaining = p->arm_not_before_ms - arm_now;

			mod_delayed_work(system_wq, &p->work,
				msecs_to_jiffies((unsigned int)remaining + 1));
			mutex_unlock(&control->lock);
			return;
		}
		/* An older callback may have waited on control->lock past the arm
		 * delay, leaving arm's rerun queued. Consume that pending rerun now;
		 * otherwise it can follow this sample immediately and reset the
		 * gauge average. The worker's normal tail schedules the next cycle. */
		if (pending)
			cancel_delayed_work(&p->work);
	}
	p->arm_not_before_ms = 0;
#endif
	*c = empty;
	c->trial_active =
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
		active
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
		|| session_active
#endif
		;
#else
		false;
#endif
	c->off_before_error = ret;
 c->off_before_start_ms = off_started; c->off_before_end_ms = off_ended;
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
 if (p->household_quiet_pending) t380_display_snapshot(&quiet_before);
#endif
	c->started_ms = t380_source_now();
 t380_ga_epoch_copy(&c->epoch_start);
 c->epoch_started = c->epoch_start.generation;
	c->charger_error[0] = t380_co_collect_locked_mode(client, &c->charger[0],
		&c->charger_started[0], &c->charger_ended[0], c->trial_active);
	c->muic_error = t380_mo_collect_mode(p->dependencies->muic, &c->muic,
		&c->muic_started_ms, &c->muic_ended_ms, c->trial_active);
	c->battery_error = t380_pa_collect_mode(p->dependencies->gauge,
		&p->history, &c->battery, c->trial_active);
	if (!c->battery_error) c->battery_error = t380_pp_battery_error(&c->battery);
	c->charger_error[1] = t380_co_collect_locked_mode(client, &c->charger[1],
		&c->charger_started[1], &c->charger_ended[1], c->trial_active);
	if (!c->charger_error[1] && c->charger[1].valid)
		c->readback_error = t380_pp_readback(control, &c->charger[1]);
 c->off_after_start_ms = t380_source_now();
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
	c->off_after_error =
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
		p->household_detaching ?
		t380_charge_household_high(&client->dev) :
#endif
		active ? t380_charge_trial_check(&client->dev) :
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
		session_active ? t380_charge_session_check(&client->dev) :
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
		session_rest ? t380_charge_recharge_rest_check(&client->dev) :
#endif
#endif
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
  p->household_quiet_pending ? t380_pp_household_quiet_off(&client->dev) :
#endif
			t380_charge_force_off(&client->dev);
#else
	c->off_after_error = t380_charge_force_off(&client->dev);
#endif
 c->off_after_end_ms = t380_source_now();
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
	c->detach_cancelled = t380_pp_mark_detach_cancellation(c);
#endif
	t380_pp_source_copy(&c->source);
 c->epoch_final = (struct t380_sl_epoch_record){
  .generation = c->source.generation, .pm_serial = c->source.pm_serial,
  .fault_serial = c->source.fault_serial, .invalidated_ms = c->source.invalidated_ms,
  .sampled_ms = c->source.now_ms, .last_invalidation = c->source.last_invalidation,
  .system_suspended = c->source.system_suspended, .bus_suspended = c->source.bus_suspended};
 c->epoch_mismatch_flags = t380_pp_epoch_flags(c);
	c->ended_ms = t380_source_now();
 /* A consumed real completion must reach unchanged pure policy, including
  * during household startup/detach bookkeeping; it cannot take a skip path. */
 if (t380_pp_late_physical(p, c)) goto t380_cycle_evaluate;
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
 t380_pp_startup_first_locked(p, c, session_active);
	if (p->household_enabled &&
	    (p->household_detaching ||
	     ((session_active || session_rest) &&
	      c->off_after_error == -EAGAIN))) {
		if (!p->household_detaching) {
			p->household_detached_rest = session_rest;
			WRITE_ONCE(p->session_active, false);
			WRITE_ONCE(p->session_rest, false);
			p->household_detaching = true;
		}
		t380_pp_household_detach_cycle_locked(p, c);
		goto t380_household_cycle_done;
	}
#endif
t380_cycle_evaluate:
 if (t380_pp_evaluate_cycle(&p->state, c, &p->result)) {
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
  goto t380_household_cycle_done;
#else
  goto t380_cycle_publish;
#endif
 }

#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
 t380_pp_regular_sample(p, c);
#endif
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
 if (p->household_quiet_pending) {
  t380_display_snapshot(&quiet_after);
  if (t380_pp_household_quiet_cycle_locked(p, control, c, &quiet_before, &quiet_after))
   p->session_requested = true;
 }
#endif
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
	if (p->trial_requested || active) {
		bool allow = !control->fault &&
				t380_trial_decide(&p->trial_policy, c, &p->result,
				true, p->trial_fixed5v, false,
				T380_TRIAL_WINDOW_3900,
				&p->trial_decision);
		p->trial_requested = false;
		if (active) {
			if (!allow) {
				p->trial_error = p->result.cycle_error ?
					p->result.cycle_error : -EPERM;
				t380_charge_force_off(&client->dev);
				WRITE_ONCE(p->trial_active, false);
				#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
				if (p->trial_fixed5v) t380_source_unqualify_fixed5v();
				#endif
			}
		} else if (allow) {
			ret = t380_pp_activate_locked(p, control, c);
			if (ret) {
				p->trial_error = ret;
				t380_charge_force_off(&client->dev);
				#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
				if (p->trial_fixed5v) t380_source_unqualify_fixed5v();
				#endif
			} else {
				WRITE_ONCE(p->trial_active, true);
			}
		} else {
			p->trial_error = p->result.cycle_error ?
				p->result.cycle_error : -EPERM;
			t380_charge_force_off(&client->dev);
				#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
				if (p->trial_fixed5v) t380_source_unqualify_fixed5v();
				#endif
		}
	}
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
	if (p->session_requested || session_active
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
	    || session_rest
#endif
	    ) {
		bool allow = !control->fault &&
			t380_pp_session_decide(p, c);
		p->session_requested = false;
		if (session_active) {
			if (!allow) {
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
				if (p->session_recharge_selected &&
				    p->phase_candidate.status == T380_PS_BOUND &&
				    p->phase_candidate.phase == T380_CP_FULL &&
				    p->phase_candidate.reason == T380_CP_FULL_STOP &&
				    !p->result.cycle_error) {
 t380_pp_transaction_attempt(p, &p->full_apply, T380_TX_FULL, c->ended_ms, T380_PHASE_FULL_OWNER);
					ret = t380_charge_recharge_full_rest(
						&client->dev, c->ended_ms);
 p->full_apply.owner_return = ret;
					if (!ret) {
						bool full_ack = true;
#ifdef CONFIG_T380_ATTACHED_MAINTENANCE_DEV
 if (p->session_attached_selected) {
  p->full_apply.stage = T380_PHASE_POLICY_ACK;
  full_ack = t380_charge_policy_ack_full(&p->phase_shadow.policy,
   p->result.input.sample_ms);
  p->full_apply.policy_ack = full_ack ? 0 : -EPERM;
 }
#endif
						if (!full_ack) {
							p->session_error = -EPERM;
 t380_pp_transaction_result(&p->full_apply, -EPERM);
							p->full_apply.cleanup_error = t380_charge_trial_revoke_off(&client->dev);
							WRITE_ONCE(p->session_active, false);
						} else {
 p->full_apply.stage = T380_PHASE_DONE;
 t380_pp_transaction_result(&p->full_apply, 0);
						WRITE_ONCE(p->session_active, false);
						WRITE_ONCE(p->session_rest, true);
						}
					} else {
						p->session_error = ret;
 t380_pp_transaction_result(&p->full_apply, ret);
						p->full_apply.cleanup_error = t380_charge_trial_revoke_off(&client->dev);
						WRITE_ONCE(p->session_active, false);
					}
				} else
#endif
				{
				t380_pp_session_refuse_locked(p);
				}
			} else {
				ret = 0;
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
				if (p->session_phase_selected &&
				    p->phase_candidate.phase == T380_CP_SECOND &&
				    p->phase_shadow.applied_valid &&
				    p->phase_shadow.applied_phase == T380_CP_FIRST) {
					ret = t380_pp_phase_transition_locked(p,
						control, c);
					if (!ret && !t380_ps_ack_applied(
					    &p->phase_shadow, T380_CP_SECOND,
					    t380_source_now()))
						ret = -EPERM;
					t380_pp_phase_ack_result(p, ret);
				} else
#endif
					ret = t380_charge_session_renew(&client->dev,
						c->ended_ms);
				if (ret) {
					p->session_error = ret;
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
					t380_pp_phase_cleanup(p, c);
#else
					t380_charge_trial_revoke_off(&client->dev);
#endif
					WRITE_ONCE(p->session_active, false);
				}
			}
		}
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
		else if (session_rest) {
			if (allow && p->session_recharge_selected &&
			    p->phase_candidate.recharge_candidate) {
				ret = t380_pp_recharge_transition_locked(p,
					control, c);
 if (!ret) {
  bool phase_ack;
  p->recharge_apply.stage = T380_PHASE_ACK;
  phase_ack = t380_ps_ack_applied(&p->phase_shadow,
   T380_CP_FIRST, t380_source_now());
  p->recharge_apply.phase_ack = phase_ack ? 0 : -EPERM;
  if (!phase_ack) ret = -EPERM;
 }
#ifdef CONFIG_T380_ATTACHED_MAINTENANCE_DEV
 if (!ret && p->session_attached_selected) {
  bool recharge_ack;
  p->recharge_apply.stage = T380_PHASE_POLICY_ACK;
  recharge_ack = t380_charge_policy_ack_recharge(
   &p->phase_shadow.policy, p->result.input.sample_ms);
  p->recharge_apply.policy_ack = recharge_ack ? 0 : -EPERM;
  if (!recharge_ack) ret = -EPERM;
 }
#endif
				if (ret) {
					p->session_error = ret;
 t380_pp_transaction_result(&p->recharge_apply, ret);
					p->recharge_apply.cleanup_error = t380_charge_trial_revoke_off(&client->dev);
					WRITE_ONCE(p->session_rest, false);
				} else {
 p->recharge_apply.stage = T380_PHASE_DONE;
 t380_pp_transaction_result(&p->recharge_apply, 0);
					WRITE_ONCE(p->session_rest, false);
					WRITE_ONCE(p->session_active, true);
				}
			} else if (!(p->phase_candidate.status == T380_PS_BOUND &&
				     p->phase_candidate.phase == T380_CP_FULL &&
				     p->phase_candidate.reason == T380_CP_FULL_STOP &&
				     !p->result.cycle_error && !control->fault)) {
				p->session_error = p->result.cycle_error ?
					p->result.cycle_error : -EPERM;
 p->rest_stop.valid = true;
 if (p->transaction_sequence != U64_MAX) ++p->transaction_sequence;
 p->rest_stop.sequence = p->transaction_sequence;
 p->rest_stop.sample_ms = c->ended_ms;
 p->rest_stop.policy_sample_ms = p->result.input.sample_ms;
 p->rest_stop.reason = p->phase_candidate.reason;
 p->rest_stop.error = p->session_error;
 p->rest_stop.cleanup_error = t380_charge_trial_revoke_off(&client->dev);
				WRITE_ONCE(p->session_rest, false);
			}
		}
#endif
		else if (allow) {
			ret = t380_pp_session_activate_locked(p, control, c);
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
   if (ret && p->household_quiet_retry) {
    p->household_quiet_retry = false;
    /* Display changed before LOW: preserve pending cap, obtain a new pair. */
   } else
#endif
			if (ret) {
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
    p->household_quiet_pending = false;
#endif
				p->session_error = ret;
				t380_charge_trial_revoke_off(&client->dev);
			} else {
				WRITE_ONCE(p->session_active, true);
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
				if (p->session_phase_selected &&
				    !t380_ps_ack_applied(&p->phase_shadow,
					#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
					p->household_rebind_pending ?
					p->phase_candidate.phase :
					#endif
					T380_CP_FIRST, t380_source_now())) {
					p->session_error = -EPERM;
					t380_charge_trial_revoke_off(&client->dev);
					WRITE_ONCE(p->session_active, false);
				}
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
				else p->household_rebind_pending = false;
#endif
#endif
			}
		} else {
			p->session_error = p->result.cycle_error ?
				p->result.cycle_error : -EPERM;
			t380_charge_trial_revoke_off(&client->dev);
		}
	}
#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
	/* A failed begin, renewal, prework owner check or policy cycle is
	 * terminal for the one-shot session. Never leave its qualification live. */
	if (p->session_fixed5v && p->session_spent &&
	    !p->session_requested
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
	    && !p->household_quiet_pending && !p->household_detached && !p->household_detaching
#endif
	    && !READ_ONCE(p->session_active)
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
	    && !READ_ONCE(p->session_rest)
#endif
	    )
		t380_source_unqualify_fixed5v();
#endif
#endif
#endif
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
	t380_pp_household_admit_locked(p, control, c);
#endif
	if (p->result.cycle_error && p->result.cycle_error != -EAGAIN &&
	    p->result.cycle_error != -ESTALE) {
		t380_pa_kernel_revoke(client);
		if (!control->fault) control->fault = p->result.cycle_error;
	}
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
t380_household_cycle_done:
 t380_pp_regular_finish(p, c);
#endif
#ifndef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
t380_cycle_publish:
#endif
 {
  struct t380_battery_thermal *sensor = i2c_get_clientdata(p->dependencies->gauge);
  const struct t380_sl_epoch_record phases[4] = {c->epoch_start,
   c->battery.epoch_record[0], c->battery.epoch_record[1], c->epoch_final};
  const u64 times[4] = {c->off_before_start_ms, c->off_before_end_ms,
   c->off_after_start_ms, c->off_after_end_ms};
  mutex_lock(&sensor->acquisition_lock);
  t380_ga_cache_publish_full(sensor, &c->battery,
   c->charger[1].chgon ? POWER_SUPPLY_STATUS_CHARGING : POWER_SUPPLY_STATUS_NOT_CHARGING,
   !c->battery.guard_cancelled && !p->result.integrity_error && !p->result.latched_error && !c->readback_error &&
   !c->charger_error[1] && !control->fault && !c->epoch_mismatch_flags,
   phases, times, c->epoch_mismatch_flags);
  mutex_unlock(&sensor->acquisition_lock);
 }
	mutex_unlock(&control->lock);
	if (READ_ONCE(p->running))
		schedule_delayed_work(&p->work, msecs_to_jiffies(T380_PP_INTERVAL_MS));
}
static int t380_pp_stop_mode(void *context, bool bounded, bool *pm_owned)
{
	struct t380_periodic *p = context;
	struct t380_control *control = i2c_get_clientdata(p->charger);
	int ret, drain_error = 0;

	WRITE_ONCE(p->running, false);
 {bool owned = t380_ga_pm_begin(((struct t380_battery_thermal *)i2c_get_clientdata(p->dependencies->gauge))->actor, T380_GA_CANCEL_STOP);
  if (pm_owned) *pm_owned = owned;}
	/* Physical HIGH precedes waiting for a potentially blocked I2C worker. */
	/* Revoke under the GPIO owner lock before draining an I2C-blocked worker.
	 * Its final begin decision takes that same lock, so no late LOW can win. */
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
	ret = t380_charge_trial_revoke_off(&p->charger->dev);
#else
	ret = t380_charge_force_off(&p->charger->dev);
#endif
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
	/* An arm holds this lock through its enqueue. Drain that critical section
	 * after HIGH, then drop the lock before waiting for work completion. */
	mutex_lock(&control->lock);
	mutex_unlock(&control->lock);
#endif
	cancel_delayed_work_sync(&p->work);
 /* Stop/teardown cannot release dependencies while physical actor uses them. */
 if (bounded) drain_error = t380_ga_quiesce(((struct t380_battery_thermal *)i2c_get_clientdata(p->dependencies->gauge))->actor);
 else flush_work(&((struct t380_battery_thermal *)i2c_get_clientdata(p->dependencies->gauge))->actor->work);
	t380_pa_kernel_revoke(p->charger);
	mutex_lock(&control->lock);
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
	p->trial_active = false;
	p->trial_requested = false;
	p->arm_not_before_ms = 0;
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
	p->session_active = false;
	p->session_requested = false;
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
 p->household_quiet_pending = p->household_quiet_retry = false;
 p->household_quiet_sample_ms = 0;
#endif
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
	if (p->household_enabled) {
		t380_pp_household_unqualify(p);
		p->household_enabled = false;
		p->household_pm_detached = false;
		p->session_error = -ESHUTDOWN;
	}
#endif
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
	p->session_rest = false;
#endif
	#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
	if (p->session_fixed5v) t380_source_unqualify_fixed5v();
	#endif
#endif
	#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
	if (p->trial_fixed5v) t380_source_unqualify_fixed5v();
	#endif
#endif
	p->history = (struct t380_pa_history){ 0 };
	p->result.acquisition_valid = false;
	p->result.permission = false;
	p->result.cycle_error = -ESHUTDOWN;
	if (ret && !p->state.latched_error) p->state.latched_error = ret;
	mutex_unlock(&control->lock);
 return ret ? ret : drain_error;
}
static void t380_pp_stop(void *context)
{
 /* Devres/teardown must physically drain before dependency resources release. */
 (void)t380_pp_stop_mode(context, false, NULL);
}
static int t380_ga_append(char *buf, size_t size, int *length, const char *fmt, ...)
{
 va_list args;
 int n;
 va_start(args, fmt); n = vsnprintf(buf + *length, size - *length, fmt, args); va_end(args);
 if (n < 0 || (size_t)n >= size - *length) return -EOVERFLOW;
 *length += n;
 return 0;
}
static int t380_ga_epoch_text(char *buf, size_t size, int *length,
 const char *name, const struct t380_sl_epoch_record *e)
{
 return t380_ga_append(buf, size, length,
  "%s_generation=%llu %s_pm=%llu %s_fault=%llu %s_invalidated_ms=%llu %s_invalidation=%d %s_sampled_ms=%llu %s_system_pm=%d %s_bus_pm=%d\n",
  name, (unsigned long long)e->generation, name, (unsigned long long)e->pm_serial,
  name, (unsigned long long)e->fault_serial, name, (unsigned long long)e->invalidated_ms,
  name, e->last_invalidation, name, (unsigned long long)e->sampled_ms,
  name, e->system_suspended, name, e->bus_suspended);
}
static ssize_t policy_shadow_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct t380_control *control = i2c_get_clientdata(to_i2c_client(dev));
	struct t380_periodic *p = control->periodic;
	struct t380_sl_snapshot live;
	bool current_record;
	int length, i;

	mutex_lock(&control->lock);
	if (!p->state.evaluations) {
		mutex_unlock(&control->lock);
		return scnprintf(buf, PAGE_SIZE,
			"cached_only=1 evaluations=0 current_record=0 not_sampled=1 permission=0 telemetry_validated=0 average_validated=0 source_validated=0 normal_charge_region_validated=0\n");
	}
	t380_pp_source_copy(&live);
	current_record = p->result.acquisition_valid && READ_ONCE(p->running) &&
		live.generation == p->last.source.generation && live.now_ms >= p->last.ended_ms &&
		live.now_ms - p->last.ended_ms <= 2 * T380_PP_INTERVAL_MS &&
		(!live.bus_suspended || live.fixed5v_qualified) &&
		!live.system_suspended;
	length = scnprintf(buf, PAGE_SIZE,
		"cached_only=1 permission=0 telemetry_validated=0 average_validated=0 source_validated=0 normal_charge_region_validated=0 fault_coverage_complete=0 hardware_sample_age_known=0\n"
		"running=%d evaluations=%llu current_record=%d cycle_error=%d latched_error=%d started_ms=%llu ended_ms=%llu live_now_ms=%llu sampled_generation=%llu live_generation=%llu source_reason=%d sdp_candidate=%d candidate_request_ma=%u request_callback_ms=%llu\n"
		"policy_reason=%s would_charge=0 input_ma=0 charge_ma=0 float_mv=0 topoff_ma=0 profile=conservative_oldest_row_4 history_ready=%d average_ma=%d\n"
		"charger_error_before=%d charger_error_after=%d muic_error=%d battery_error=%d readback_error=%d off_before_error=%d off_after_error=%d status2_raw=%d chgon=%d input_stock_ma=%d fast_stock_ma=%d float_stock_mv=%d\n",
		READ_ONCE(p->running), (unsigned long long)p->state.evaluations, current_record,
		p->result.cycle_error, p->state.latched_error,
		(unsigned long long)p->last.started_ms, (unsigned long long)p->last.ended_ms,
		(unsigned long long)live.now_ms, (unsigned long long)p->last.source.generation,
		(unsigned long long)live.generation, live.reason, live.sdp_candidate,
		live.candidate_request_ma, (unsigned long long)live.request_ms,
		t380_charge_policy_reason(p->result.decision.reason), p->last.battery.history_ready,
		p->last.battery.average_ma, p->last.charger_error[0], p->last.charger_error[1],
		p->last.muic_error, p->last.battery_error, p->last.readback_error,
		p->last.off_before_error, p->last.off_after_error,
		p->last.charger[1].raw[T380_CO_STATUS2], p->last.charger[1].chgon,
		p->last.charger[1].input_limit_stock_ma, p->last.charger[1].fast_current_stock_ma,
		p->last.charger[1].float_voltage_mv);
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
 length += t380_pp_startup_show(p, buf, length);
#endif
	/* Both records are cached source metadata; no hardware acquisition here. */
	length += scnprintf(buf + length, PAGE_SIZE - length,
		"sampled_reason=%d sampled_bus_suspended=%d sampled_system_suspended=%d sampled_fixed5v_candidate=%d sampled_fixed5v_qualified=%d sampled_clean_absence=%d sampled_host_blocked=%d sampled_fresh_absence=%d sampled_absence_oldest_ms=%llu sampled_oldest_ms=%llu\n"
		"live_reason=%d live_bus_suspended=%d live_system_suspended=%d live_fixed5v_candidate=%d live_fixed5v_qualified=%d live_clean_absence=%d live_host_blocked=%d live_fresh_absence=%d live_absence_oldest_ms=%llu live_oldest_ms=%llu\n",
		p->last.source.reason, p->last.source.bus_suspended, p->last.source.system_suspended, p->last.source.fixed5v_candidate, p->last.source.fixed5v_qualified, p->last.source.clean_absence, p->last.source.host_blocked, p->last.source.fresh_absence,
		(unsigned long long)p->last.source.absence_oldest_ms,
		(unsigned long long)p->last.source.oldest_ms,
		live.reason, live.bus_suspended, live.system_suspended, live.fixed5v_candidate, live.fixed5v_qualified, live.clean_absence, live.host_blocked, live.fresh_absence,
		(unsigned long long)live.absence_oldest_ms,
		(unsigned long long)live.oldest_ms);
	/* Version1: cached actual acquisition only, from this same stored cycle.
	 * state=pattern,valid,stable; errors=guard_before,guard_after,read,error;
	 * reads=count,failed_pass,failed_index; ms=started,ended. Raw pass order:
	 * DEVID,ADC,DEVT1,DEVT2,DEVT3,RSVDID1,CHGTYPE (01,07,0a,0b,15,1d,24).
	 * The sampled serials belong to p->last.source, never the live snapshot. */
	length += scnprintf(buf + length, PAGE_SIZE - length,
		"muic_diag_v1=1 muic_state=%d,%d,%d muic_errors=%d,%d,%d,%d muic_reads=%d,%d,%d muic_ms=%llu,%llu sampled_fault_serial=%llu sampled_pm_serial=%llu muic_raw0=%d,%d,%d,%d,%d,%d,%d muic_raw1=%d,%d,%d,%d,%d,%d,%d\n",
		p->last.muic.pattern, p->last.muic.valid, p->last.muic.stable,
		p->last.muic.guard_before_error, p->last.muic.guard_after_error,
		p->last.muic.read_error, p->last.muic.error,
		p->last.muic.read_count, p->last.muic.failed_pass, p->last.muic.failed_index,
		(unsigned long long)p->last.muic_started_ms,
		(unsigned long long)p->last.muic_ended_ms,
		(unsigned long long)p->last.source.fault_serial,
		(unsigned long long)p->last.source.pm_serial,
		p->last.muic.raw[0][0], p->last.muic.raw[0][1], p->last.muic.raw[0][2],
		p->last.muic.raw[0][3], p->last.muic.raw[0][4], p->last.muic.raw[0][5],
		p->last.muic.raw[0][6], p->last.muic.raw[1][0], p->last.muic.raw[1][1],
		p->last.muic.raw[1][2], p->last.muic.raw[1][3], p->last.muic.raw[1][4],
		p->last.muic.raw[1][5], p->last.muic.raw[1][6]);
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
	length += scnprintf(buf + length, PAGE_SIZE - length,
		"household_enable_ms=%llu household_absence_seen=%d household_qualification_pending=%d integrity_error=%d\n",
		(unsigned long long)p->household_enable_ms, p->household_absence_seen,
		p->household_qualification_pending, p->result.integrity_error);
#endif
	/* Same cached cycle, raw order is T380_CO_* enum; no extra acquisition. */
	length += scnprintf(buf + length, PAGE_SIZE - length,
		"co0_v1=%d,%d,%d raw=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
		p->last.charger[0].valid, p->last.charger_error[0], p->last.detach_cancelled,
		p->last.charger[0].raw[0], p->last.charger[0].raw[1], p->last.charger[0].raw[2],
		p->last.charger[0].raw[3], p->last.charger[0].raw[4], p->last.charger[0].raw[5],
		p->last.charger[0].raw[6], p->last.charger[0].raw[7], p->last.charger[0].raw[8],
		p->last.charger[0].raw[9], p->last.charger[0].raw[10]);
	for (i = 0; i < T380_PA_FIELDS; ++i) {
		const struct t380_pa_field_sample *f = &p->last.battery.field[i];
		length += scnprintf(buf + length, PAGE_SIZE - length,
			"field=%d raw=%d value=%d error=%d started_ms=%llu ended_ms=%llu\n",
			i, f->raw, f->value, f->error,
			(unsigned long long)f->started_ms, (unsigned long long)f->ended_ms);
	}
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
	length += scnprintf(buf + length, PAGE_SIZE - length,
		"phase_shadow_compiled=1 phase_shadow_authority=0 phase_shadow_status=%d phase_shadow_generation=%llu phase_shadow_reason=%s phase_shadow_phase=%d phase_shadow_candidate_plan=%d phase_shadow_recharge_candidate=%d phase_shadow_input_ma=%d phase_shadow_fast_ma=%d phase_shadow_float_mv=%d phase_shadow_topoff_ma=%d\n",
		p->phase_candidate.status,
		(unsigned long long)p->phase_candidate.generation,
		t380_charge_policy_reason(p->phase_candidate.reason),
		p->phase_candidate.phase, p->phase_candidate.candidate_plan,
		p->phase_candidate.recharge_candidate,
		p->phase_candidate.input_ma, p->phase_candidate.fast_ma,
		p->phase_candidate.float_mv, p->phase_candidate.topoff_ma);
	length += scnprintf(buf + length, PAGE_SIZE - length,
		"phase_profile_selected=%d phase_applied_valid=%d phase_applied_phase=%d\n",
		p->session_phase_selected, p->phase_shadow.applied_valid,
		p->phase_shadow.applied_phase);
	length += scnprintf(buf + length, PAGE_SIZE - length,
		"recharge_profile_selected=%d session_full_rest_cached=%d phase_full_sample_ms=%llu phase_recharge_candidate=%d\n",
		p->session_recharge_selected, READ_ONCE(p->session_rest),
		(unsigned long long)p->phase_shadow.policy.last_full_sample_ms,
		p->phase_candidate.recharge_candidate);
#ifdef CONFIG_T380_ATTACHED_MAINTENANCE_DEV
	length += scnprintf(buf + length, PAGE_SIZE - length,
		"attached_profile_selected=%d attached_wake_held=%d attached_episode_open=%d attached_episode_number=%u attached_episode_kind=%s attached_episode_start_ms=%llu attached_weighted_remaining_ma_ms=%llu attached_full_sample_ms=%llu attached_terminal_reason=%d\n",
		p->session_attached_selected,
		t380_charge_attached_wake_held(dev),
		p->phase_shadow.policy.episode_open,
		p->phase_shadow.policy.episode_number,
		p->phase_shadow.policy.episode_number > 1 ? "recharge" : "initial",
		(unsigned long long)p->phase_shadow.policy.episode_start_ms,
		(unsigned long long)p->phase_shadow.policy.remaining_budget_ma_ms,
		(unsigned long long)p->phase_shadow.policy.last_full_sample_ms,
		p->session_error);
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
	length += scnprintf(buf + length, PAGE_SIZE - length,
		"household_profile=%s household_floor_mv=%d household_initial_upper_mv=%d household_phase_upper_mv=%d\n",
		t380_pp_household_profile(p->household_window),
		t380_household_window_floor_mv(p->household_window),
        t380_household_window_initial_mv(p->household_window),
        t380_household_window_cv_mv(p->household_window));
#endif
#endif
#endif
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
	length += scnprintf(buf + length, PAGE_SIZE - length,
		"trial_compiled=1 trial_spent=%d trial_requested=%d trial_active_cached=%d trial_error=%d trial_policy_reason=%s shadow_permission=0\n",
		p->trial_spent, p->trial_requested, READ_ONCE(p->trial_active),
		p->trial_error, t380_charge_policy_reason(p->trial_decision.reason));
#endif
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
	length += scnprintf(buf + length, PAGE_SIZE - length,
		"session_compiled=1 session_spent_cached=%d session_requested=%d session_active_cached=%d session_fixed5v_selected=%d session_floor_mv=%d session_selected_input_ma=%d session_selected_fast_ma=%d session_selected_budget_ms=%llu session_error=%d session_policy_reason=%s session_profile=%s shadow_permission=0\n",
		p->session_spent, p->session_requested, READ_ONCE(p->session_active),
		p->session_fixed5v,
		p->session_fixed5v ?
		(p->session_window == T380_BELKIN_SESSION_WINDOW_3800 ?
		 3800 : 3900) : 4050,
		p->session_fixed5v ? t380_pp_session_input_ma(p) : 100, p->session_fixed5v ? 500 : 100,
		(unsigned long long)p->session_budget_ms,
		p->session_error,
		t380_charge_policy_reason(p->session_decision.reason),
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
		t380_pp_session_input_ma(p) == 900 ?
		t380_pp_household_profile(p->household_window) :
#endif
		p->session_fixed5v ?
#ifdef CONFIG_T380_ATTACHED_MAINTENANCE_DEV
		(p->session_attached_selected ? "attached_maintenance_500mA_4190mV" :
#endif
		(p->session_budget_ms == T380_SS_MAX_BUDGET_MS ?
		 (p->session_window == T380_BELKIN_SESSION_WINDOW_3800 ?
		  "development_1800s_3800mV_500mA_4190mV" :
		  "development_1800s_500mA_4190mV") :
			"development_60s_500mA_4190mV")
#ifdef CONFIG_T380_ATTACHED_MAINTENANCE_DEV
		)
#endif
		:
		"development_60s_100mA_4190mV");
#endif
	mutex_unlock(&control->lock);
	return length;
}
static DEVICE_ATTR_RO(policy_shadow);
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
static ssize_t trial_arm_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct t380_control *control = i2c_get_clientdata(client);
	struct t380_periodic *p = control->periodic;
	bool fixed5v = false;
	int ret = 0;

	if (!capable(CAP_SYS_RAWIO)) return -EPERM;
	if (!sysfs_streq(buf, "fixed5v-dcp-1a-thermal-checked")) {
#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
		if (sysfs_streq(buf,
		    "belkin-wcb002-dqv2-fixed5v2.4a-thermal-checked"))
			fixed5v = true;
		else
#endif
			return -EPERM;
	}
	mutex_lock(&control->lock);
	if (!p || !READ_ONCE(p->running) || !control->inhibited_programming ||
	    !control->initial.verified || control->fault || p->trial_spent)
		ret = -EPERM;
	#ifdef CONFIG_T380_CHARGE_SESSION_DEV
	else if (p->session_spent)
		ret = -EPERM; /* Never requalify a live session from a denied trial arm. */
	#endif
	else {
		#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
		if (fixed5v && !t380_source_qualify_fixed5v()) ret = -ESTALE;
		#endif
		if (!ret) ret = t380_charge_trial_reserve(dev, fixed5v);
		#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
		if (ret && fixed5v) t380_source_unqualify_fixed5v();
		#endif
		/* Stop marks running=false before it revokes under the owner lock.
		 * If it beat this reservation, revoke again and consume the token. */
		if (!ret && !READ_ONCE(p->running)) {
			t380_charge_trial_revoke_off(dev);
			#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
			if (fixed5v) t380_source_unqualify_fixed5v();
			#endif
			ret = -ESHUTDOWN;
		}
		if (!ret) {
			p->trial_spent = true;
			p->trial_requested = true;
			p->trial_fixed5v = fixed5v;
			p->arm_not_before_ms = t380_source_now() +
				T380_PP_ARM_SAMPLE_DELAY_MS;
			/* Stop synchronizes this locked enqueue before cancel_sync. */
			mod_delayed_work(system_wq, &p->work,
				msecs_to_jiffies(T380_PP_ARM_SAMPLE_DELAY_MS));
		}
	}
	mutex_unlock(&control->lock);
	if (ret) return ret;
	return count;
}
static DEVICE_ATTR_WO(trial_arm);
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
static ssize_t session_arm_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct t380_control *control = i2c_get_clientdata(client);
	struct t380_periodic *p = control->periodic;
	bool fixed5v = false;
	enum t380_trial_voltage_window window = T380_TRIAL_WINDOW_3900;
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
	bool phase_profile = false;
	bool recharge_profile = false;
#ifdef CONFIG_T380_ATTACHED_MAINTENANCE_DEV
	bool attached_profile = false;
#endif
#endif
	u64 budget_ms = T380_SS_BUDGET_MS;
	int ret = 0;

	if (!capable(CAP_SYS_RAWIO)) return -EPERM;
	if (!sysfs_streq(buf, "fixed5v-dcp-1a-thermal-checked-session60")) {
#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
		if (sysfs_streq(buf,
		    "belkin-wcb002-dqv2-fixed5v2.4a-thermal-checked-session60-3900mv-500ma"))
			fixed5v = true;
		else if (sysfs_streq(buf,
		    "belkin-wcb002-dqv2-fixed5v2.4a-thermal-checked-session1800-3900mv-500ma")) {
			fixed5v = true;
			budget_ms = T380_SS_MAX_BUDGET_MS;
		}
		else if (sysfs_streq(buf,
		    "belkin-wcb002-dqv2-fixed5v2.4a-thermal-checked-session1800-3800mv-500ma")) {
			fixed5v = true;
			window = T380_BELKIN_SESSION_WINDOW_3800;
			budget_ms = T380_SS_MAX_BUDGET_MS;
		}
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
		else if (sysfs_streq(buf,
		    "belkin-wcb002-dqv2-fixed5v2.4a-thermal-checked-phase1800-3900mv-500ma")) {
			fixed5v = true;
			phase_profile = true;
			budget_ms = T380_SS_MAX_BUDGET_MS;
		}
		else if (sysfs_streq(buf,
		    "belkin-wcb002-dqv2-fixed5v2.4a-thermal-checked-recharge1800-3900mv-500ma")) {
			fixed5v = true;
			phase_profile = true;
			recharge_profile = true;
			budget_ms = T380_SS_MAX_BUDGET_MS;
		}
#ifdef CONFIG_T380_ATTACHED_MAINTENANCE_DEV
		else if (sysfs_streq(buf,
		    "belkin-wcb002-dqv2-fixed5v2.4a-thermal-checked-attached-maintenance-3900mv-500ma")) {
			fixed5v = true;
			phase_profile = true;
			recharge_profile = true;
			attached_profile = true;
			budget_ms = T380_SS_ATTACHED_INITIAL_MS;
		}
#endif
#endif
		else
#endif
			return -EPERM;
	}
	mutex_lock(&control->lock);
	if (!p || !READ_ONCE(p->running) || !control->inhibited_programming ||
	    !control->initial.verified || control->fault || p->trial_spent ||
	    p->session_spent) ret = -EPERM;
	else {
		#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
		if (fixed5v && !t380_source_qualify_fixed5v()) ret = -ESTALE;
		#endif
		if (!ret) {
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
			ret =
#ifdef CONFIG_T380_ATTACHED_MAINTENANCE_DEV
				attached_profile ? t380_charge_attached_reserve(dev) :
#endif
				recharge_profile ? t380_charge_recharge_reserve(dev) :
				phase_profile ? t380_charge_phase_reserve(dev) :
#endif
				t380_charge_session_reserve(dev, fixed5v,
					budget_ms);
		}
		#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
		if (ret && fixed5v) t380_source_unqualify_fixed5v();
		#endif
		if (!ret && !READ_ONCE(p->running)) {
			t380_charge_trial_revoke_off(dev);
			#ifdef CONFIG_T380_FIXED5V_TIMEOUT_TRIAL
			if (fixed5v) t380_source_unqualify_fixed5v();
			#endif
			ret = -ESHUTDOWN;
		}
		if (!ret) {
#ifdef CONFIG_T380_ATTACHED_MAINTENANCE_DEV
			if (attached_profile &&
			    !t380_charge_policy_attached_mode(
			    &p->phase_shadow.policy)) {
				t380_charge_trial_revoke_off(dev);
				ret = -EPERM;
			}
#endif
		}
		if (!ret) {
			p->session_spent = true;
			p->session_requested = true;
			p->session_fixed5v = fixed5v;
			p->session_window = window;
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
			p->session_phase_selected = phase_profile;
			p->session_recharge_selected = recharge_profile;
#ifdef CONFIG_T380_ATTACHED_MAINTENANCE_DEV
			p->session_attached_selected = attached_profile;
#endif
#endif
			p->session_budget_ms = budget_ms;
			p->arm_not_before_ms = t380_source_now() +
				T380_PP_ARM_SAMPLE_DELAY_MS;
			/* Stop waits for this control-lock enqueue after owner HIGH. */
			mod_delayed_work(system_wq, &p->work,
				msecs_to_jiffies(T380_PP_ARM_SAMPLE_DELAY_MS));
		}
	}
	mutex_unlock(&control->lock);
	return ret ? (ssize_t)ret : (ssize_t)count;
}
static DEVICE_ATTR_WO(session_arm);
#endif
#endif
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
static const char *t380_household_stop_reason(int error)
{
	switch (error) {
	case -ENOMSG: return "hardware_done_incomplete";
	case -EPROTO: return "hardware_state_mismatch";
	case -ETIME: return "deadline";
	case -ESHUTDOWN: return "suspend";
	case -ECANCELED: return "disabled";
	case -ESTALE: return "source";
	case -EIO: case -ENODATA: return "measurement";
	default: return "safety";
	}
}
static ssize_t household_status_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct t380_control *control = i2c_get_clientdata(to_i2c_client(dev));
	struct t380_periodic *p = control->periodic;
	const char *state, *reason;
	ssize_t n;
 u64 status_now = t380_source_now();

	mutex_lock(&control->lock);
	if (p && p->session_error && p->session_error != -ECANCELED)
		state = "stopped";
	else if (!p || !p->household_enabled)
		state = p && p->session_error == -ECANCELED ? "disabled" :
			"boot_disabled";
	else if (p->session_error || control->fault)
		state = "stopped";
	else if (p->household_detaching || p->household_detached)
		state = "detached";
	else if (READ_ONCE(p->session_active))
		state = "charging";
	else if (READ_ONCE(p->session_rest))
		state = "full_rest";
	else if (p->household_quiet_pending)
		state = "waiting_quiet";
	else if (!p->household_absence_seen)
		state = "waiting_absence";
	else
		state = "waiting_source";
	reason = p ? t380_household_stop_reason(
		p->session_error ? p->session_error : control->fault) : "safety";
 if (p && p->household_quiet_timeout && p->session_error == -ETIME)
  reason = "quiet_timeout";
 else if (!strcmp(state, "waiting_quiet")) reason = "display_off_required";
	n = scnprintf(buf, PAGE_SIZE,
		"state=%s reason=%s profile=%s floor_mv=%d initial_upper_mv=%d phase_upper_mv=%d admission_refusal=%s cached_only=1 sample_ms=%llu "
		"episode=%u remaining_weighted_ma_ms=%llu quiet_status=%s quiet_deadline_ms=%llu quiet_remaining_ms=%llu input_limit_ma=%d fast_limit_ma=500 source_contract=%s\n",
		state, reason,
		p ? t380_pp_household_profile(p->household_window) :
		"household-3900mv-500ma",
		p ? t380_household_window_floor_mv(p->household_window) : 3900,
        p ? t380_household_window_initial_mv(p->household_window) : 4170,
        p ? t380_household_window_cv_mv(p->household_window) : 4190,
		p ? t380_pp_household_admission_refusal(
			p->household_admission_refusal) : "none",
		(unsigned long long)(p ? p->last.ended_ms : 0),
		p ? p->phase_shadow.policy.episode_number : 0,
		(unsigned long long)(p ?
		 p->phase_shadow.policy.remaining_budget_ma_ms : 0),
  p && p->household_quiet_display.known_off ? "sampling" :
   "display_unknown",
  (unsigned long long)(p ? p->household_quiet_deadline_ms : 0),
  (unsigned long long)(p && p->household_quiet_pending &&
   status_now < p->household_quiet_deadline_ms ?
   p->household_quiet_deadline_ms - status_now : 0),
  p ? t380_household_window_input_ma(p->household_window) : 500,
  p && t380_household_window_input_ma(p->household_window) == 900 ? "belkin-operator900-v1" : "belkin-fixed5v-500");
	mutex_unlock(&control->lock);
	return n;
}
static ssize_t regular_full_show(struct device *dev,
 struct device_attribute *attr, char *buf)
{
 struct t380_control *control = i2c_get_clientdata(to_i2c_client(dev));
 struct t380_periodic *p;
 struct t380_battery_thermal *sensor;
 struct t380_rf_model *w;
 ssize_t length;
 int i;
 mutex_lock(&control->lock);
 p = control->periodic;
 if (!p) {mutex_unlock(&control->lock); return -ENODEV;}
 sensor = i2c_get_clientdata(p->dependencies->gauge);
 mutex_lock(&sensor->acquisition_lock);
 w = &sensor->model;
 length = scnprintf(buf, PAGE_SIZE,
  "regular_full_v1=1 cached_only=1 profile_selected=%d profile=stock_default_age_unknown4300_4230 measured_age_known=0 float_mv=4300 recharge_mv=4230 full_min_mv=4200 full_scaled_soc=93 initial_upper_mv=4280 "
  "model_partial_compatibility=%d model_error=%d model_failed_index=%d model_reads=%d model_ms=%llu,%llu model_epoch=%llu model_status=%d,%d "
  "capacity_valid=%d raw_tenths=%d scaled_tenths=%d reported_capacity=%d capacity_max=%d capacity_ms=%llu capacity_sample_ms=%llu capacity_updates=%u dynamic_updates=%u cadence_ms=30000 ram_only=1 legitimate_full_off=%d full_off_ms=%llu\n",
  sensor->regular_selected, sensor->regular_model_matched, w->error,
  w->failed_index, w->reads, (unsigned long long)w->started_ms,
  (unsigned long long)w->ended_ms, (unsigned long long)w->epoch,
  w->status_before, w->status_after, sensor->capacity.valid,
  sensor->capacity.raw_tenths, sensor->capacity.scaled_tenths,
  sensor->capacity.capacity_old, sensor->capacity.capacity_max,
  (unsigned long long)sensor->capacity.updated_ms,
  (unsigned long long)sensor->capacity.sample_ms, sensor->capacity.updates,
  sensor->capacity.dynamic_updates, sensor->regular_full_off,
  (unsigned long long)sensor->regular_full_off_ms);
 length += scnprintf(buf + length, PAGE_SIZE - length,
  "model_primer_reg=ac attempted=%d raw=%d error=%d stage=%d reads=%d strict_reads=%d total_word_reads=%lld\n",
  w->primer_attempted, w->primer_raw, w->primer_error, w->primer_stage,
  w->primer_reads, w->reads, (long long)w->reads + w->aux_reads);
 length += scnprintf(buf + length, PAGE_SIZE - length,
  "model_aux_reads=%d model_aux_failed_index=%d\n", w->aux_reads, w->aux_failed_index);
 for (i = 0; i < T380_RF_MODEL_AUX_REGS; ++i)
  length += scnprintf(buf + length, PAGE_SIZE - length,
   "model_aux_reg=%02x raw=%d error=%d stage=%d\n",
   t380_rf_model_aux_registers[i], w->aux_raw[i], w->aux_error[i], w->aux_stage[i]);
 for (i = 0; i < T380_RF_MODEL_REGS; ++i)
  length += scnprintf(buf + length, PAGE_SIZE - length, "model_reg=%02x raw=%d\n",
   t380_rf_model_registers[i], w->raw[i]);
 mutex_unlock(&sensor->acquisition_lock);
 mutex_unlock(&control->lock);
 return length;
}
/* Same existing read-only SELinux path; finite cached text, no acquisition.
 * Blocks carry run IDs so a caller can reject a capture change across reads. */
#define T380_GA_HEADER_BYTES 4096
#define T380_GA_ROW_BYTES 1024
#define T380_GA_GUARD_BYTES 1024
#define T380_GA_EXPORT_BYTES (T380_GA_HEADER_BYTES + T380_GA_TRACE_RECORDS * T380_GA_ROW_BYTES + T380_GA_GUARD_RECORDS * T380_GA_GUARD_BYTES)
static int t380_ga_export_block(struct t380_ga_actor *a, unsigned int block,
 char *out, size_t size)
{
 int length = 0, ret = 0, i;
 unsigned long flags;
 /* Format a bounded coherent block under trace spin; no allocation/I2C/wait. */
 spin_lock_irqsave(&a->trace_lock, flags);
 if (!a->trace_ready || a->tracing) {ret = -EAGAIN; goto done;}
 if (!block) {
  const struct t380_rf_model *m = &a->trace_model;
  const struct t380_rf_diagnostic *d = &a->trace_diagnostic;
  ret = t380_ga_append(out, size, &length,
   "%s=1 cached_only=1 ram_only=1 diagnostic_not_admission=1 timestamps_clock=BOOTTIME current_permission=0 source_validated=0 normal_charge_region_validated=0 run_sequence=%llu trace_ready=1 trace_complete=%d trace_overflow=%d trace_foreign=%d trace_error=%d trace_cancel_reason=%d gauge_rows=%u guard_rows=%u started_ms=%llu ended_ms=%llu\n",
   a->trace_kind == T380_GA_SCRATCH ? "strict_scratch_v1" : "stock_diagnostic_v1",
   (unsigned long long)a->trace_run, a->trace_complete, a->trace_overflow,
   a->trace_foreign, a->trace_result, a->trace_cancel_reason, a->trace_count, a->guard_count,
   (unsigned long long)a->trace_started_ms, (unsigned long long)a->trace_ended_ms);
  if (!ret) ret = t380_ga_epoch_text(out, size, &length, "request", &a->trace_requested);
  if (!ret) ret = t380_ga_epoch_text(out, size, &length, "final", &a->trace_final);
  if (a->trace_kind == T380_GA_SCRATCH) {
   if (!ret) ret = t380_ga_append(out, size, &length,
    "algorithm_matched=%d error=%d failed_index=%d reads=%d aux_reads=%d aux_failed_index=%d primer_stage=%d primer_raw=%d primer_error=%d status_before=%d status_after=%d strict_started_ms=%llu strict_ended_ms=%llu strict_epoch=%llu\n",
    m->matched, m->error, m->failed_index, m->reads, m->aux_reads, m->aux_failed_index,
    m->primer_stage, m->primer_raw, m->primer_error, m->status_before, m->status_after,
    (unsigned long long)m->started_ms, (unsigned long long)m->ended_ms, (unsigned long long)m->epoch);
   for (i = 0; !ret && i < T380_RF_MODEL_REGS; ++i)
    ret = t380_ga_append(out, size, &length, "strict_reg=%02x raw=%d\n", t380_rf_model_registers[i], m->raw[i]);
   for (i = 0; !ret && i < T380_RF_MODEL_AUX_REGS; ++i)
    ret = t380_ga_append(out, size, &length, "aux_reg=%02x raw=%d error=%d stage=%d\n", t380_rf_model_aux_registers[i], m->aux_raw[i], m->aux_error[i], m->aux_stage[i]);
  } else {
   if (!ret) ret = t380_ga_append(out, size, &length,
    "complete=%d error=%d failed_read=%d stage=%d attempted=%d status_before=%d status_before_error=%d status_after=%d status_after_error=%d\n",
    d->complete, d->error, d->failed_read, d->stage, d->attempted,
    d->status_before, d->status_before_error, d->status_after, d->status_after_error);
   for (i = 0; !ret && i < T380_RF_DIAGNOSTIC_REGS; ++i)
    ret = t380_ga_append(out, size, &length, "diagnostic_read=%d reg=%02x raw=%d error=%d start_ms=%llu end_ms=%llu\n",
     i, d->address[i], d->raw[i], d->read_error[i],
     (unsigned long long)d->read_start_ms[i], (unsigned long long)d->read_end_ms[i]);
  }
 } else if (block <= a->trace_count) {
  const struct t380_ga_trace_record *r = &a->trace[block - 1];
  ret = t380_ga_append(out, size, &length,
   "run_sequence=%llu gauge_row=%u token=%llu pid=%d caller=%d messages=%d adapter_attempt=%d reg=%02x length0=%u flags0=%u length1=%u flags1=%u result=%d byte0=%u byte1=%u valid_bytes=%u foreign=%d data_write=%d finished=%d geometry_overflow=%d begin_ns=%llu end_ns=%llu\n",
   (unsigned long long)a->trace_run, block - 1, (unsigned long long)r->token,
   r->task_pid, r->caller, r->messages, r->attempt, r->command,
   r->length[0], r->flags[0], r->length[1], r->flags[1], r->result,
   r->bytes[0], r->bytes[1], r->valid_bytes, r->foreign, r->data_write, r->finished, r->geometry_overflow,
   (unsigned long long)r->begin_ns, (unsigned long long)r->end_ns);
  if (!ret) ret = t380_ga_epoch_text(out, size, &length, "before", &r->before);
  if (!ret) ret = t380_ga_epoch_text(out, size, &length, "after", &r->after);
 } else if (block <= a->trace_count + a->guard_count) {
  const struct t380_ga_guard_record *r = &a->guards[block - a->trace_count - 1];
  ret = t380_ga_append(out, size, &length,
   "run_sequence=%llu guard_row=%u kind=%d result=%d begin_ns=%llu end_ns=%llu\n",
   (unsigned long long)a->trace_run, block - a->trace_count - 1, r->kind, r->result,
   (unsigned long long)r->begin_ns, (unsigned long long)r->end_ns);
  if (!ret) ret = t380_ga_epoch_text(out, size, &length, "before", &r->before);
  if (!ret) ret = t380_ga_epoch_text(out, size, &length, "after", &r->after);
 } else ret = 1; /* finite EOF */
 if (!ret) {out[length] = '\n'; memset(out + length + 1, ' ', size - length - 1);}
done:
 spin_unlock_irqrestore(&a->trace_lock, flags);
 return ret;
}
static ssize_t regular_stock_diagnostic_read(struct file *file, struct kobject *kobj,
 struct bin_attribute *attr, char *buf, loff_t offset, size_t count)
{
 struct device *dev = kobj_to_dev(kobj);
 struct t380_control *control = i2c_get_clientdata(to_i2c_client(dev));
 struct t380_ga_actor *a;
 char *blockbuf;
 size_t copied = 0, size, pos;
 unsigned int block, rows;
 unsigned long flags;
 u64 run;
 int ret = 0;
 if (offset < 0) return -EINVAL;
 mutex_lock(&control->lock);
 if (!control->periodic) {ret = -ENODEV; goto unlock;}
 a = ((struct t380_battery_thermal *)i2c_get_clientdata(control->periodic->dependencies->gauge))->actor;
 blockbuf = kmalloc(T380_GA_HEADER_BYTES, GFP_KERNEL);
 if (!blockbuf) {ret = -ENOMEM; goto unlock;}
 spin_lock_irqsave(&a->trace_lock, flags);
 run = a->trace_run; rows = a->trace_count;
 spin_unlock_irqrestore(&a->trace_lock, flags);
 while (copied < count) {
  if (offset >= T380_GA_EXPORT_BYTES) break;
  if (offset < T380_GA_HEADER_BYTES) {block = 0; size = T380_GA_HEADER_BYTES; pos = offset;}
  else if (offset < T380_GA_HEADER_BYTES + rows * T380_GA_ROW_BYTES) {
   pos = (offset - T380_GA_HEADER_BYTES) % T380_GA_ROW_BYTES;
   block = 1 + (offset - T380_GA_HEADER_BYTES) / T380_GA_ROW_BYTES; size = T380_GA_ROW_BYTES;
  } else {
   pos = (offset - T380_GA_HEADER_BYTES - rows * T380_GA_ROW_BYTES) % T380_GA_GUARD_BYTES;
   block = 1 + rows + (offset - T380_GA_HEADER_BYTES - rows * T380_GA_ROW_BYTES) / T380_GA_GUARD_BYTES;
   size = T380_GA_GUARD_BYTES;
  }
  if (offset >= T380_GA_EXPORT_BYTES) break;
  ret = t380_ga_export_block(a, block, blockbuf, size);
  if (ret) {if (ret == 1) ret = 0; break;}
  size = min(size - pos, count - copied);
  memcpy(buf + copied, blockbuf + pos, size); copied += size; offset += size;
 }
 spin_lock_irqsave(&a->trace_lock, flags);
 if (run != a->trace_run || !a->trace_ready || a->tracing) ret = -EAGAIN;
 spin_unlock_irqrestore(&a->trace_lock, flags);
 kfree(blockbuf);
unlock:
 mutex_unlock(&control->lock);
 return ret ? ret : copied;
}
static struct bin_attribute bin_attr_regular_stock_diagnostic = {
 .attr = {.name = "regular_stock_diagnostic", .mode = 0444},
 .size = T380_GA_EXPORT_BYTES, .read = regular_stock_diagnostic_read,
};
static DEVICE_ATTR_RO(regular_full);
static DEVICE_ATTR_RO(household_status);
static ssize_t household_control_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct t380_control *control = i2c_get_clientdata(to_i2c_client(dev));
	struct t380_periodic *p = control->periodic;
	enum t380_household_voltage_window requested, previous;
	bool enable_command, was_enabled;
	int ret = -EPERM;
 struct t380_ga_actor *disable_actor = NULL;
 int disable_error = 0, high_error;
 struct t380_ga_actor *command_actor = p ?
  ((struct t380_battery_thermal *)i2c_get_clientdata(p->dependencies->gauge))->actor : NULL;
 u64 command_ticket = command_actor ? t380_ga_command_ticket(command_actor) : 0;

 /* Invalidate without waiting for the actor/control transaction. Revoke HIGH
  * independently, then the original locked Disable closes a racing enable. */
 if (sysfs_streq(buf, "disable") && p) {
  disable_actor = ((struct t380_battery_thermal *)i2c_get_clientdata(p->dependencies->gauge))->actor;
  t380_ga_disable_begin(disable_actor);
  disable_error = t380_charge_household_disable(dev);
  high_error = t380_charge_trial_revoke_off(dev);
  if (!disable_error) disable_error = high_error;
 }
	mutex_lock(&control->lock);
	if (!p || !READ_ONCE(p->running) || !control->inhibited_programming ||
	    !control->initial.verified) goto done;
 if (sysfs_streq(buf, "diagnose-stock-sequence-v1") ||
     sysfs_streq(buf, "diagnose-strict-scratch-v1")) {
  if (control->fault || p->session_error != -ECANCELED ||
      p->household_enabled || p->trial_requested || p->session_requested ||
      READ_ONCE(p->session_active) || READ_ONCE(p->session_rest) ||
      p->session_phase_selected || p->session_recharge_selected ||
      p->session_attached_selected || p->household_quiet_pending ||
      p->household_qualification_pending || p->household_detaching ||
      p->household_detached || p->household_rebind_pending ||
      p->phase_shadow.policy.episode_open) goto done;
  ret = sysfs_streq(buf, "diagnose-strict-scratch-v1") ?
   t380_rf_strict_scratch(p->charger, p->dependencies->gauge) :
   t380_rf_stock_diagnostic(p->charger, p->dependencies->gauge);
  goto done;
 }
	enable_command = sysfs_streq(buf, "enable") ||
		sysfs_streq(buf, "enable-attended-3800") ||
		sysfs_streq(buf, "enable-attended-normal-3600-v1") ||
		sysfs_streq(buf, "enable-operator900-3900-v1") ||
		sysfs_streq(buf, "enable-operator900-attended-normal-3600-v1") ||
        sysfs_streq(buf, "enable-regular900-stock4300-3900-v1") ||
        sysfs_streq(buf, "enable-regular900-stock4300-attended-normal-3600-v1");
	if (enable_command) {
  if (!t380_ga_command_current(command_actor, command_ticket)) {ret = -ECANCELED; goto done;}
		requested = sysfs_streq(buf, "enable-regular900-stock4300-3900-v1") ?
            T380_HOUSEHOLD_WINDOW_REGULAR900_3900_V1 :
            sysfs_streq(buf, "enable-regular900-stock4300-attended-normal-3600-v1") ?
            T380_HOUSEHOLD_WINDOW_REGULAR900_NORMAL_3600_V1 :
            sysfs_streq(buf, "enable-operator900-3900-v1") ?
			T380_HOUSEHOLD_WINDOW_OPERATOR900_3900_V1 :
			sysfs_streq(buf, "enable-operator900-attended-normal-3600-v1") ?
			T380_HOUSEHOLD_WINDOW_OPERATOR900_NORMAL_3600_V1 :
			sysfs_streq(buf, "enable-attended-normal-3600-v1") ?
			T380_HOUSEHOLD_WINDOW_NORMAL_3600_V1 :
			sysfs_streq(buf, "enable-attended-3800") ?
			T380_HOUSEHOLD_WINDOW_ATTENDED_3800 :
			T380_HOUSEHOLD_WINDOW_3900;
		if (control->fault || (p->session_error &&
		    !(p->session_error == -ECANCELED && !p->session_spent)))
			goto done;
        if (t380_household_window_regular(requested) && !p->household_enabled) {
            ret = t380_rf_model_observe(p->charger, p->dependencies->gauge);
            if (ret) goto done;
        }
		was_enabled = p->household_enabled;
		previous = p->phase_shadow.household_window;
		if (!was_enabled &&
		    !t380_ps_set_household_window(&p->phase_shadow, requested))
			goto done;
  if (!t380_ga_command_current(command_actor, command_ticket)) {ret = -ECANCELED; goto done;}
		ret = t380_charge_household_enable(dev, requested);
		if (ret && !was_enabled)
			t380_ps_set_household_window(&p->phase_shadow, previous);
		if (!ret && !was_enabled) {
			p->session_error = 0;
			p->household_quiet_pending = p->household_quiet_timeout = p->household_quiet_retry = false;
			p->household_enabled = true;
			p->household_window = requested;
            if (t380_household_window_regular(requested) ||
                t380_household_window_regular(previous))
                t380_rf_select(p->dependencies->gauge,
                    t380_household_window_regular(requested));
			p->household_admission_refusal = 0;
			p->household_enable_ms = t380_source_now();
			p->household_absence_seen = false;
			t380_pp_household_unqualify(p);
			mod_delayed_work(system_wq, &p->work, 0);
		}
	} else if (sysfs_streq(buf, "disable")) {
		ret = t380_charge_household_disable(dev);
		if (!ret) {
			p->household_enabled = false;
            if (t380_household_window_regular(p->household_window))
                t380_rf_clear_full(p->dependencies->gauge);
			p->household_quiet_pending = p->household_quiet_retry = false;
			p->session_requested = false;
			WRITE_ONCE(p->session_active, false);
			WRITE_ONCE(p->session_rest, false);
			p->household_detaching = false;
			p->household_detached = false;
			p->session_error = -ECANCELED;
			t380_pp_household_unqualify(p);
		}
	} else if (sysfs_streq(buf, "acknowledge")) {
		if (control->fault ||
		    !p->session_error) goto done;
		ret = t380_charge_household_ack(dev);
		if (!ret) {
			t380_ps_init(&p->phase_shadow);
			if (!t380_ps_set_household_window(&p->phase_shadow,
			    p->household_window)) {
				ret = -EINVAL;
				goto done;
			}
			p->session_spent = false;
			p->session_requested = false;
			p->session_attached_selected = false;
			p->session_error = 0;
			p->household_quiet_pending = p->household_quiet_timeout = p->household_quiet_retry = false;
			p->household_enabled = true;
			p->household_absence_seen = true;
			p->household_detaching = false;
			p->household_detached = false;
			p->household_rebind_pending = false;
			t380_pp_household_unqualify(p);
			mod_delayed_work(system_wq, &p->work, 0);
		}
	}
done:
 if (disable_actor) {
  if (!ret) ret = disable_error;
  t380_ga_disable_end(disable_actor);
 }
	mutex_unlock(&control->lock);
	return ret ? (ssize_t)ret : (ssize_t)count;
}
static DEVICE_ATTR_WO(household_control);
#endif
static void t380_pp_attr_remove(void *context)
{
	struct i2c_client *client = context;

	device_remove_file(&client->dev, &dev_attr_policy_shadow);
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
 device_remove_file(&client->dev, &dev_attr_phase_apply);
#endif
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
	device_remove_file(&client->dev, &dev_attr_trial_arm);
#endif
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
	device_remove_file(&client->dev, &dev_attr_session_arm);
#endif
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
	device_remove_file(&client->dev, &dev_attr_household_control);
	device_remove_file(&client->dev, &dev_attr_household_status);
 device_remove_file(&client->dev, &dev_attr_regular_full);
 device_remove_bin_file(&client->dev, &bin_attr_regular_stock_diagnostic);
#endif
}
static int t380_pp_start(struct i2c_client *client, struct t380_pp_dependencies *d)
{
	struct t380_control *control = i2c_get_clientdata(client);
	struct t380_periodic *p;
	int ret;

	if (!control->inhibited_programming) return -EOPNOTSUPP;
	p = devm_kzalloc(&client->dev, sizeof(*p), GFP_KERNEL);
	if (!p) return -ENOMEM;
	p->charger = client; p->dependencies = d;
	INIT_DELAYED_WORK(&p->work, t380_periodic_work);
	t380_pp_init(&p->state);
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
	t380_ps_init(&p->phase_shadow);
#endif
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
	t380_charge_policy_init(&p->trial_policy);
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
	t380_charge_policy_init(&p->session_policy);
#endif
#endif
	control->periodic = p;
	ret = devm_add_action_or_reset(&client->dev, t380_pp_stop, p);
	if (ret) return ret;
	ret = device_create_file(&client->dev, &dev_attr_policy_shadow);
	if (ret) return ret;
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
	ret = device_create_file(&client->dev, &dev_attr_trial_arm);
	if (ret) {
		device_remove_file(&client->dev, &dev_attr_policy_shadow);
		return ret;
	}
#endif
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
	ret = device_create_file(&client->dev, &dev_attr_session_arm);
	if (ret) {
		device_remove_file(&client->dev, &dev_attr_trial_arm);
		device_remove_file(&client->dev, &dev_attr_policy_shadow);
		return ret;
	}
#endif
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
	ret = device_create_file(&client->dev, &dev_attr_household_status);
	if (ret) {
		device_remove_file(&client->dev, &dev_attr_session_arm);
		device_remove_file(&client->dev, &dev_attr_trial_arm);
		device_remove_file(&client->dev, &dev_attr_policy_shadow);
		return ret;
	}
	ret = device_create_file(&client->dev, &dev_attr_household_control);
	if (ret) {
		device_remove_file(&client->dev, &dev_attr_household_status);
		device_remove_file(&client->dev, &dev_attr_session_arm);
		device_remove_file(&client->dev, &dev_attr_trial_arm);
		device_remove_file(&client->dev, &dev_attr_policy_shadow);
		return ret;
	}
 ret = device_create_file(&client->dev, &dev_attr_regular_full);
 if (ret) {t380_pp_attr_remove(client); return ret;}
 ret = device_create_bin_file(&client->dev, &bin_attr_regular_stock_diagnostic);
 if (ret) {t380_pp_attr_remove(client); return ret;}
#endif
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
 ret = device_create_file(&client->dev, &dev_attr_phase_apply);
 if (ret) {
  t380_pp_attr_remove(client);
  return ret;
 }
#endif
	ret = devm_add_action_or_reset(&client->dev, t380_pp_attr_remove, client);
	if (ret) return ret;
	WRITE_ONCE(p->running, true);
		schedule_delayed_work(&p->work, 0);
	return 0;
}
/* Failed callbacks unwind themselves; PM core skips resume for that device.
 * Restart only ordinary observation. Never restore a trial/session/household
 * enable, cached permission, model witness or old lifetime. */
static void t380_pp_failed_suspend(struct t380_periodic *p, bool was_running, bool pm_owned)
{
 struct t380_control *control = i2c_get_clientdata(p->charger);
 struct t380_ga_actor *actor = ((struct t380_battery_thermal *)i2c_get_clientdata(p->dependencies->gauge))->actor;
 if (pm_owned) t380_ga_pm_end_mode(actor, 2);
 mutex_lock(&control->lock);
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
 t380_pp_household_unqualify(p);
 p->household_enabled = p->household_pm_detached = false;
#endif
#ifdef CONFIG_T380_SUPERVISED_CHARGE_TRIAL
 p->trial_active = p->trial_requested = false;
#ifdef CONFIG_T380_CHARGE_SESSION_DEV
 p->session_active = p->session_requested = false;
#ifdef CONFIG_T380_PHASE_CHARGE_DEV
 p->session_rest = false;
#endif
#endif
#endif
 p->history = (struct t380_pa_history){0};
 p->result.acquisition_valid = p->result.permission = false;
 WRITE_ONCE(p->running, was_running);
 mutex_unlock(&control->lock);
 if (was_running) schedule_delayed_work(&p->work, 0);
}
static int t380_pp_suspend_mode(struct device *dev, bool normal_pm)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct t380_control *control;
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
	struct t380_periodic *p;
	bool keep_detached = false;
#endif

	if (client->addr == 0x25) {
		t380_me_suspend(client);
		return 0;
	}
 if (client->addr == 0x71) {
  struct t380_battery_thermal *sensor = i2c_get_clientdata(client);
  int actor_ret;
  if (!sensor) return -ENODEV;
  actor_ret = t380_ga_suspend(sensor->actor, 1, !normal_pm);
  return actor_ret;
 }
	if (client->addr != 0x49) return 0;
	control = i2c_get_clientdata(client);
	if (!control || !control->periodic) return t380_charge_inhibit_check(dev);
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
	p = control->periodic;
	if (normal_pm) {
		mutex_lock(&control->lock);
		if (p->household_qualification_pending)
			t380_pp_household_unqualify(p);
		if (READ_ONCE(p->running) && p->household_enabled &&
		    p->household_detached && !p->household_detaching &&
		    !p->session_requested && !p->trial_requested &&
		    !READ_ONCE(p->session_active) &&
		    !READ_ONCE(p->session_rest) && !p->session_error &&
		    !control->fault &&
		    !t380_charge_household_detached_ready(dev, true)) {
			WRITE_ONCE(p->running, false);
			p->household_pm_detached = true;
			keep_detached = true;
		}
		mutex_unlock(&control->lock);
		if (keep_detached) {
   struct t380_ga_actor *actor = ((struct t380_battery_thermal *)i2c_get_clientdata(p->dependencies->gauge))->actor;
   int actor_ret = t380_ga_consumer_pm(actor, true);
   if (actor_ret) {t380_pp_failed_suspend(p, true, false);return actor_ret;}
			cancel_delayed_work_sync(&p->work);
			/* Normal PM invalidates cached proof, not an observation fault.
			 * The PHY suspend edge is idempotent if it ran first; POST_SUSPEND
			 * clears this edge on resume or an aborted suspend. */
			t380_source_system_suspend(true);
			return 0;
		}
	}
#endif
 {
  bool was_running = READ_ONCE(control->periodic->running), pm_owned = false;
  int stop_error = t380_pp_stop_mode(control->periodic, normal_pm, &pm_owned);
  if (!stop_error) stop_error = t380_charge_force_off(dev);
  if (stop_error && normal_pm) t380_pp_failed_suspend(control->periodic, was_running, pm_owned);
  return stop_error;
 }
}
static int t380_pp_suspend(struct device *dev)
{
	return t380_pp_suspend_mode(dev, true);
}
static int t380_pp_terminal_suspend(struct device *dev)
{
	return t380_pp_suspend_mode(dev, false);
}
static int t380_pp_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct t380_control *control;
	int ret;

	if (client->addr == 0x25) {
		t380_me_resume(client);
		return 0;
	}
 if (client->addr == 0x71) {
  struct t380_battery_thermal *sensor = i2c_get_clientdata(client);
  if (sensor) t380_ga_pm(sensor->actor, false);
  return 0;
 }
	if (client->addr != 0x49) return 0;
	control = i2c_get_clientdata(client);
	if (!control || !control->periodic) return t380_charge_inhibit_check(dev);
#ifdef CONFIG_T380_HOUSEHOLD_CHARGE_DEV
	if (control->periodic->household_pm_detached) {
		ret = t380_charge_household_detached_ready(dev, true);
		if (ret) {
			t380_pp_stop(control->periodic);
			return ret;
		}
		ret = t380_ga_consumer_pm(((struct t380_battery_thermal *)i2c_get_clientdata(control->periodic->dependencies->gauge))->actor, false);
  if (ret) return ret;
		WRITE_ONCE(control->periodic->running, true);
		schedule_delayed_work(&control->periodic->work, 0);
		return 0;
	}
#endif
	ret = t380_charge_force_off(dev);
	if (ret) return ret;
 ret = t380_ga_consumer_pm(((struct t380_battery_thermal *)i2c_get_clientdata(control->periodic->dependencies->gauge))->actor, false);
 if (ret) return ret;
	WRITE_ONCE(control->periodic->running, true);
	schedule_delayed_work(&control->periodic->work, 0);
	return 0;
}
static const struct dev_pm_ops t380_pp_pm = {
	.suspend = t380_pp_suspend, .resume = t380_pp_resume,
	.freeze = t380_pp_terminal_suspend, .thaw = t380_pp_resume,
	.poweroff = t380_pp_terminal_suspend, .restore = t380_pp_resume,
};
static int t380_pp_remove(struct i2c_client *client)
{
	t380_pp_terminal_suspend(&client->dev);
	return 0;
}
static void t380_pp_shutdown(struct i2c_client *client)
{
	int ret = t380_pp_terminal_suspend(&client->dev);

	if (ret) dev_err(&client->dev, "periodic shutdown HIGH failed: %d\n", ret);
}
#endif
