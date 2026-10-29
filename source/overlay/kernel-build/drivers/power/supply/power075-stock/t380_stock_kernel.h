/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_STOCK_KERNEL_H
#define T380_STOCK_KERNEL_H
#include <linux/alarmtimer.h>
#include <linux/pm_wakeup.h>
#include <linux/t380_display_admission.h>
#include "t380_stock_policy.c"
struct t380_stock_dependencies {struct i2c_client *gauge,*muic;};
static void t380_stock_put_dependencies(void *context)
{
	struct t380_stock_dependencies *d = context;

	if (d->muic) put_device(&d->muic->dev);
	if (d->gauge) put_device(&d->gauge->dev);
}
static int t380_stock_supplier(struct i2c_client *consumer, const char *property,
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
static int t380_stock_dependencies_get(struct i2c_client *client,
				    struct t380_stock_dependencies **output)
{
	struct t380_stock_dependencies *d;
	int ret;

	d = devm_kzalloc(&client->dev, sizeof(*d), GFP_KERNEL);
	if (!d) return -ENOMEM;
	ret = devm_add_action_or_reset(&client->dev, t380_stock_put_dependencies, d);
	if (ret) return ret;
	ret = t380_stock_supplier(client, "samsung,policy-gauge", 0x71,
		"samsung,t380-sm5703-fg-diagnostic", &d->gauge);
	if (ret) return ret;
	ret = t380_stock_supplier(client, "samsung,policy-muic", 0x25,
		"samsung,t380-sm5703-muic-observation", &d->muic);
	if (ret) return ret;
	*output = d;
	return 0;
}
/* Periodic control lives entirely on the device. Suspend is not a charging
 * transition: an RTC alarm wakes a serialized monitor like Samsung's battery
 * monitor. Physical I/O failure makes OFF independently of the bus. */
struct t380_stock_runtime {
 struct i2c_client *charger;
 struct t380_stock_dependencies *deps;
 struct t380_battery_thermal *sensor;
 struct power_supply *psy;
 struct workqueue_struct *queue;
 struct delayed_work work;
 struct alarm alarm;
 struct wakeup_source *wake;
 struct t380_stock_state state;
 struct t380_stock_input input;
 struct t380_stock_output output;
 struct t380_pa_history history;
 struct t380_pa_tuple sample;
 struct t380_cs_plan applied;
 struct t380_ic_result transaction;
 u64 sampled_ms, failures;
 int error, last_error, off_error, status, cycle_count, aicl_ma;
 int average_ma, previous_ma;
 bool average_valid;
 bool running, suspended, have_plan, aicl_active;
 u64 policy_ms;
};
#define T380_STOCK_AWAKE_MS 2000
#define T380_STOCK_SLEEP_MS 30000
static bool t380_stock_charger_mode;
static int __init t380_stock_bootmode(char *value)
{t380_stock_charger_mode=!strcmp(value,"charger");return 0;}
early_param("androidboot.mode",t380_stock_bootmode);
static int t380_stock_muic_off(void *context);
static int t380_stock_muic_source_ok(void *context);
static int t380_stock_charger_read(struct t380_stock_runtime *p,
 struct t380_co_observation *c)
{
 int raw[T380_CO_COUNT],i;
 for(i=0;i<T380_CO_COUNT;i++) {
  raw[i]=i2c_smbus_read_byte_data(p->charger,t380_co_addresses[i]);
  if(raw[i]<0)return raw[i];
 }
 return t380_co_decode(raw,c);
}
static bool t380_stock_muic_sink(const struct t380_co_observation *c)
{
 /* This board is USB peripheral-only. Do not reset during source/OTG or
  * voltage faults; normal stock policy still handles those conditions. */
 return c->operation_mode_code==5 && !c->otgfail && !c->vbusovp &&
  !c->vbusuvlo && !c->vbus_conflict;
}
static int t380_stock_source_read(struct t380_stock_runtime *p,
 enum t380_stock_source *source, struct t380_co_observation *c,bool allow_retry)
{
 struct t380_muic_extcon *m=i2c_get_clientdata(p->deps->muic);
 int d1,d3,vbus,r;
 *source=T380_STOCK_SOURCE_UNKNOWN;
 r=t380_stock_charger_read(p,c);if(r)return r;
 /* Only the initial worker read may retry. Its policy and all pre-enable
  * checks then use observations collected after the entire transaction. */
 mutex_lock(&t380_muic_observation_lock);
 if(allow_retry) {
  r=t380_muic_retry_poll_locked(p->deps->muic,c->vbusok,
   t380_stock_muic_sink(c),t380_stock_muic_off,t380_stock_muic_source_ok,p);
  if(r<0)goto out;
  if(r>0) {
   r=t380_stock_charger_read(p,c);if(r)goto out;
   p->state.was_charging=false;
  }
 }
 if(m->retry.error){r=m->retry.error;goto out;}
 /* Unresolved reset routing is an ordinary non-chargeable source, not an
  * invented hardware failure. The next monitor can finish classification. */
 if(!t380_muic_retry_ready_locked(p->deps->muic)){r=0;goto out;}
 d1=i2c_smbus_read_byte_data(p->deps->muic,0x0a);
 d3=i2c_smbus_read_byte_data(p->deps->muic,0x15);
 vbus=i2c_smbus_read_byte_data(p->deps->muic,0x1d);
 if(d1<0 || d3<0 || vbus<0){r=d1<0?d1:d3<0?d3:vbus;goto out;}
 if(!(vbus&2) && !c->vbusok)*source=T380_STOCK_ABSENT;
 else if((vbus&2) && c->vbusok) {
  if(d1==0x04)*source=T380_STOCK_SDP;
  else if(d1==0x20)*source=T380_STOCK_CDP;
  else if(d1==0x40)*source=T380_STOCK_MAINS;
  else if(!d1 && (d3&4))*source=T380_STOCK_TIMEOUT;
 }
 r=0;
out:
 mutex_unlock(&t380_muic_observation_lock);
 return r;
}
static bool t380_stock_hardware_ok(const struct t380_co_observation *c,enum t380_stock_source source)
{
 return !(c->raw[T380_CO_STATUS1]&(BIT(1)|BIT(6))) &&
  !(c->raw[T380_CO_STATUS2]&(BIT(1)|BIT(5)|BIT(6))) &&
  !c->vbusovp && !c->vbus_conflict &&
  (source==T380_STOCK_ABSENT || !c->vbusuvlo);
}
static int t380_stock_off(struct t380_stock_runtime *p)
{
 int r=t380_charge_force_off(&p->charger->dev),v;
 if(!r) {
  v=i2c_smbus_read_byte_data(p->charger,SM5703_STATUS3);
  /* Samsung nCHGEN is the OFF authority. CHGON feedback can lag
   * the verified GPIO transition; keep real I2C errors, not a timing gate. */
  if(v<0)r=v;
 }
 p->off_error=r;return r;
}
static int t380_stock_muic_off(void *context)
{return t380_stock_off(context);}
static int t380_stock_muic_source_ok(void *context)
{
 struct t380_co_observation c;
 int r=t380_stock_charger_read(context,&c);
 if(r)return r;
 return c.vbusok && t380_stock_muic_sink(&c) &&
  t380_stock_hardware_ok(&c,T380_STOCK_TIMEOUT);
}
static int t380_stock_program(struct t380_stock_runtime *p)
{
 struct t380_cs_limits limits={p->output.input_ma,p->output.fast_ma,
  p->output.float_mv,p->output.topoff_ma};
 struct t380_cs_plan plan;
 struct t380_cs_verification verified;
 struct t380_ic_io io={.context=p->charger,.off=t380_control_off,
  .guard=t380_control_guard,.read=t380_control_read,.write=t380_control_write};
 int raw[T380_CS_COUNT],i,r;
 r=t380_cs_encode_profile(&limits,T380_CS_STOCK_ATTACHED,&plan);if(r)return r;
 if(!p->have_plan || memcmp(&plan,&p->applied,sizeof(plan))) {
  r=t380_ic_apply(&io,&plan,&p->transaction);
  if(r){p->have_plan=false;return r;}
  p->applied=plan;p->have_plan=true;
 } else {
  /* Detect real drift/reset instead of silently assuming old programming. */
  for(i=0;i<T380_CS_COUNT;i++) {
   raw[i]=i2c_smbus_read_byte_data(p->charger,plan.fields[i].address);
   if(raw[i]<0)return raw[i];
  }
  r=t380_cs_verify(&plan,raw,&verified);if(r)return r;
 }
 return 0;
}
static void t380_stock_fail(struct t380_stock_runtime *p,int error)
{
 unsigned long flags;
 p->error=p->last_error=error;++p->failures;p->status=POWER_SUPPLY_STATUS_UNKNOWN;p->output.charge=false;
 t380_stock_off(p);p->have_plan=false;p->state.was_charging=false;
 spin_lock_irqsave(&p->sensor->actor->cache_lock,flags);
 p->sensor->actor->measured_error=error;
 p->sensor->actor->cached_status=POWER_SUPPLY_STATUS_UNKNOWN;
 spin_unlock_irqrestore(&p->sensor->actor->cache_lock,flags);
}
static void t380_stock_cycle(struct t380_stock_runtime *p)
{
 struct t380_stock_input in={.cycle_count=p->cycle_count,
  .charger_mode=t380_stock_charger_mode};
 struct t380_co_observation c;
 struct t380_display_snapshot display;
 struct t380_ga_outcome pending;
 unsigned long flags;
 int r,feedback;
 if(t380_ga_physical_take(p->sensor->actor,&pending)) {
  t380_stock_fail(p,pending.physical_error);return;
 }
 r=t380_charge_safety_ready(&p->charger->dev);
 if(!r)r=t380_stock_source_read(p,&in.source,&c,true);
 if(r){t380_stock_fail(p,r);return;}
 if(in.source!=p->input.source)p->aicl_ma=0;
 /* Samsung AICL reduces by100mA to300mA, preserving the negotiated lower
  * limit until the source changes. Silicon retains its own AICL regulation. */
 if(c.aicl_active && c.input_limit_stock_ma>300)
  p->aicl_ma=max(300,c.input_limit_stock_ma-100);
 p->aicl_active=c.aicl_active && c.input_limit_stock_ma>300;
 in.aicl_input_ma=p->aicl_ma;
 r=t380_pa_collect_mode(p->deps->gauge,&p->history,&p->sample,false);
 if(r || !p->sample.tuple_valid){t380_stock_fail(p,r?r:-ENODATA);return;}
 in.now_ms=t380_source_now();in.measurements_ok=true;
 in.monitor_tick=!p->policy_ms || in.now_ms-p->policy_ms>=30000;
 if(in.monitor_tick)p->policy_ms=in.now_ms;
 in.battery_present=!(c.raw[T380_CO_STATUS2]&BIT(4));
 in.hardware_ok=t380_stock_hardware_ok(&c,in.source);
 in.charging_feedback=c.chgon;
 in.temperature_decic=p->sample.field[T380_PA_TEMP].value;
 in.voltage_mv=p->sample.field[T380_PA_VOLTAGE].value/1000;
 in.current_ma=p->sample.field[T380_PA_CURRENT].value/1000;
 if(in.source!=p->input.source)p->average_valid=false;
 if(!p->average_valid){p->average_ma=p->previous_ma=in.current_ma;p->average_valid=true;}
 else if(in.monitor_tick){p->average_ma=(2*p->average_ma+p->previous_ma+in.current_ma)/4;p->previous_ma=in.current_ma;}
 in.average_ma=p->average_ma;
 t380_display_snapshot(&display);in.display_on=!display.known_off;
 spin_lock_irqsave(&t380_source_lock,flags);
 in.usb_configured=t380_stock_usb_ma>=500 && !t380_source_state.bus_suspended;
 spin_unlock_irqrestore(&t380_source_lock,flags);
 mutex_lock(&p->sensor->acquisition_lock);
 t380_rf_capacity_update(&p->sensor->capacity,p->sample.field[T380_PA_SOC].raw,
  in.now_ms,p->sample.field[T380_PA_SOC].started_ms,c.chgon,false);
 in.capacity=p->sensor->capacity.capacity_old;
 t380_stock_step(&p->state,&in,&p->output);
 mutex_unlock(&p->sensor->acquisition_lock);
 p->input=in;
 if(p->output.charge) {
  enum t380_stock_source latest_source;
  struct t380_co_observation latest;
  r=t380_stock_source_read(p,&latest_source,&latest,false);
  if(r || latest_source!=in.source || !t380_stock_hardware_ok(&latest,latest_source) ||
     (latest.raw[T380_CO_STATUS2]&BIT(4))) {t380_stock_fail(p,r?r:-EAGAIN);return;}
 }
 if(p->output.charge)r=t380_stock_program(p);else r=t380_stock_off(p);
 if(r){t380_stock_fail(p,r);return;}
 if(p->output.charge) {
  enum t380_stock_source latest_source;
  struct t380_co_observation latest;
  r=t380_stock_source_read(p,&latest_source,&latest,false);
  if(r || latest_source!=in.source || !t380_stock_hardware_ok(&latest,latest_source) ||
     (latest.raw[T380_CO_STATUS2]&BIT(4))) {t380_stock_fail(p,r?r:-EAGAIN);return;}
  r=t380_stock_gpio(&p->charger->dev,1);if(r){t380_stock_fail(p,r);return;}
 }
 feedback=i2c_smbus_read_byte_data(p->charger,SM5703_STATUS3);
 if(feedback<0) {
  t380_stock_fail(p,feedback);return;
 }
 p->status=in.source==T380_STOCK_ABSENT?POWER_SUPPLY_STATUS_DISCHARGING:
  (p->output.full?POWER_SUPPLY_STATUS_FULL:
   ((feedback&SM5703_CHGON)?POWER_SUPPLY_STATUS_CHARGING:POWER_SUPPLY_STATUS_NOT_CHARGING));
 /* Samsung learns on every qualifying full-current sample, including
  * below100. Only a completed SOC scale advances the full-charge stage. */
 mutex_lock(&p->sensor->acquisition_lock);
 if(p->output.full_candidate)
  t380_rf_dynamic_scale(&p->sensor->capacity,
   t380_rf_raw_tenths(p->sample.field[T380_PA_SOC].raw),in.capacity);
 
 spin_lock_irqsave(&p->sensor->actor->cache_lock,flags);
 p->sensor->actor->measured_error=0;
 spin_unlock_irqrestore(&p->sensor->actor->cache_lock,flags);
 t380_ga_accept_conversion(p->sensor,&p->sample,in.now_ms);
 t380_ga_cache_publish(p->sensor,&p->sample,p->status,true);
 mutex_unlock(&p->sensor->acquisition_lock);
 p->error=0;p->off_error=0;p->sampled_ms=in.now_ms;
}
static void t380_stock_work(struct work_struct *work)
{
 struct t380_stock_runtime *p=container_of(to_delayed_work(work),struct t380_stock_runtime,work);
 struct t380_control *c=i2c_get_clientdata(p->charger);
 __pm_stay_awake(p->wake);
 mutex_lock(&c->lock);
 if(p->running && !p->suspended && !READ_ONCE(t380_source_state.system_suspended))t380_stock_cycle(p);
 mutex_unlock(&c->lock);
 power_supply_changed(p->psy);
 if(READ_ONCE(p->running)) {
  alarm_start_relative(&p->alarm,ms_to_ktime(T380_STOCK_SLEEP_MS));
  if(!READ_ONCE(p->suspended))queue_delayed_work(p->queue,&p->work,msecs_to_jiffies(p->aicl_active?200:T380_STOCK_AWAKE_MS));
 }
 __pm_relax(p->wake);
}
static enum alarmtimer_restart t380_stock_alarm(struct alarm *alarm,ktime_t now)
{
 struct t380_stock_runtime *p=container_of(alarm,struct t380_stock_runtime,alarm);
 if(READ_ONCE(p->running)) {
  __pm_stay_awake(p->wake);
  if(!READ_ONCE(p->suspended))mod_delayed_work(p->queue,&p->work,0);
 }
 return ALARMTIMER_NORESTART;
}
static ssize_t stock_status_show(struct device *dev,struct device_attribute *attr,char *buf)
{
 struct t380_control *c=dev_get_drvdata(dev);struct t380_stock_runtime *p=c->stock;
 struct t380_muic_extcon *m=i2c_get_clientdata(p->deps->muic);
 ssize_t n;unsigned int i;mutex_lock(&c->lock);
 n=scnprintf(buf,PAGE_SIZE,"stock_v1=1 automatic=%u source=%d reason=%d charging_requested=%u status=%d error=%d last_error=%d failures=%llu off_error=%d sampled_ms=%llu input_ma=%d fast_ma=%d float_mv=%d topoff_ma=%d phase=%d health=%d cycle_count=%d age_step=%d age_known=%u remaining_ma_ms=%llu model_gate=0 host_lease=0\n",
 1U,p->input.source,p->output.reason,p->output.charge,p->status,p->error,p->last_error,
 (unsigned long long)p->failures,p->off_error,(unsigned long long)p->sampled_ms,
 p->output.input_ma,p->output.fast_ma,p->output.float_mv,p->output.topoff_ma,p->state.phase,
 p->state.health,p->cycle_count,p->state.age_step,p->output.age_known,(unsigned long long)p->state.remaining_ma_ms);
 /* Same lock order as the charging worker; no notifier runs in this lock. */
 mutex_lock(&t380_muic_observation_lock);
 if(n && buf[n-1]=='\n')--n;
 n+=scnprintf(buf+n,PAGE_SIZE-n," muic_ccic=%d muic_attempts=%u muic_completed=%u muic_stage=%u muic_error=%d muic_pending=%u muic_generation=%u muic_before_d1=%d muic_before_d3=%d muic_before_chg=%d muic_after_d1=%d muic_after_d3=%d muic_after_chg=%d\n",
  t380_mr_ccic_mode,m->retry.attempts,m->retry.completed,m->retry.stage,
  m->retry.error,m->retry.pending_route,m->retry.generation,
  m->retry.before.dev1,m->retry.before.dev3,m->retry.before.chgtype,
  m->retry.after.dev1,m->retry.after.dev3,m->retry.after.chgtype);
 if(n && buf[n-1]=='\n')--n;
 n+=scnprintf(buf+n,PAGE_SIZE-n," muic_before_valid=%d muic_after_valid=%d",
  m->retry.before_valid,m->retry.after_valid);
 for(i=0;i<T380_MR_CONFIG_COUNT;i++)
  n+=scnprintf(buf+n,PAGE_SIZE-n," muic_pre_%02x=%d muic_post_%02x=%d",
   t380_mr_config_regs[i],m->retry.config_before[i],
   t380_mr_config_regs[i],m->retry.config_after[i]);
 n+=scnprintf(buf+n,PAGE_SIZE-n,"\n");
 mutex_unlock(&t380_muic_observation_lock);
 mutex_unlock(&c->lock);return n;
}
static DEVICE_ATTR_RO(stock_status);
/* Stock receives the persistent cycle count from its battery service, not an
 * exact model fingerprint. -1 honestly means that record is unavailable. */
static ssize_t battery_cycle_show(struct device *dev,struct device_attribute *attr,char *buf)
{struct t380_control *c=dev_get_drvdata(dev);return scnprintf(buf,PAGE_SIZE,"%d\n",READ_ONCE(c->stock->cycle_count));}
static ssize_t battery_cycle_store(struct device *dev,struct device_attribute *attr,const char *buf,size_t count)
{
 struct t380_control *c=dev_get_drvdata(dev);int n,r=kstrtoint(buf,10,&n);
 if(r)return r;if(n<0)return -EINVAL;
 mutex_lock(&c->lock);c->stock->cycle_count=n;mutex_unlock(&c->lock);
 mod_delayed_work(c->stock->queue,&c->stock->work,0);return count;
}
static DEVICE_ATTR_RW(battery_cycle);
static struct attribute *t380_stock_attrs[]={&dev_attr_stock_status.attr,&dev_attr_battery_cycle.attr,NULL};
static const struct attribute_group t380_stock_group={.attrs=t380_stock_attrs};
static void t380_stock_cleanup(void *context)
{
 struct t380_stock_runtime *p=context;
 WRITE_ONCE(p->running,false);alarm_cancel(&p->alarm);cancel_delayed_work_sync(&p->work);
 t380_stock_gpio(&p->charger->dev,3);destroy_workqueue(p->queue);
 wakeup_source_unregister(p->wake);
}
static int t380_stock_start(struct i2c_client *client,struct t380_stock_dependencies *deps,struct power_supply *psy)
{
 struct t380_control *c=i2c_get_clientdata(client);
 struct t380_stock_runtime *p;int r;
 if(!alarmtimer_get_rtcdev())return -ENODEV;
 p=devm_kzalloc(&client->dev,sizeof(*p),GFP_KERNEL);if(!p)return -ENOMEM;
 p->charger=client;p->deps=deps;p->psy=psy;p->sensor=i2c_get_clientdata(deps->gauge);
 p->cycle_count=-1;p->status=POWER_SUPPLY_STATUS_UNKNOWN;
 p->sensor->actor->charger=client;t380_stock_init(&p->state);
 p->wake=wakeup_source_register(&client->dev,"t380-stock-monitor");if(!p->wake)return -ENOMEM;
 p->queue=alloc_ordered_workqueue("t380-stock-monitor",WQ_FREEZABLE|WQ_MEM_RECLAIM);
 if(!p->queue){wakeup_source_unregister(p->wake);return -ENOMEM;}
 INIT_DELAYED_WORK(&p->work,t380_stock_work);alarm_init(&p->alarm,ALARM_BOOTTIME,t380_stock_alarm);
 c->stock=p;r=devm_add_action_or_reset(&client->dev,t380_stock_cleanup,p);if(r)return r;
 r=devm_device_add_group(&client->dev,&t380_stock_group);if(r)return r;
 r=t380_stock_gpio(&client->dev,2);if(r)return r;
 WRITE_ONCE(p->running,true);queue_delayed_work(p->queue,&p->work,0);return 0;
}
static int t380_stock_suspend(struct device *dev)
{
 struct i2c_client *client=to_i2c_client(dev);
 if(client->addr==0x25){t380_me_suspend(client);return 0;}
 if(client->addr==0x71) {
  struct t380_battery_thermal *s=i2c_get_clientdata(client);
  bool owned=t380_ga_pm_begin_mode(s->actor,T380_GA_CANCEL_PM,1);
  int r=t380_ga_quiesce(s->actor);
  if(r && owned)t380_ga_pm_end_mode(s->actor,1);return r;
 }
 if(client->addr==0x49) {
  struct t380_control *c=i2c_get_clientdata(client);struct t380_stock_runtime *p=c->stock;
  if(!p)return 0;
  WRITE_ONCE(p->suspended,true);cancel_delayed_work_sync(&p->work);
  alarm_start_relative(&p->alarm,ms_to_ktime(T380_STOCK_SLEEP_MS));
 }
 return 0;
}
static int t380_stock_resume(struct device *dev)
{
 struct i2c_client *client=to_i2c_client(dev);
 if(client->addr==0x25){t380_me_resume(client);return 0;}
 if(client->addr==0x71) {
  struct t380_battery_thermal *s=i2c_get_clientdata(client);t380_ga_pm_end_mode(s->actor,1);
 }
 if(client->addr==0x49) {
  struct t380_control *c=i2c_get_clientdata(client);struct t380_stock_runtime *p=c->stock;
  if(p && p->running){WRITE_ONCE(p->suspended,false);__pm_stay_awake(p->wake);mod_delayed_work(p->queue,&p->work,0);}
 }
 return 0;
}
static int t380_stock_terminal(struct device *dev)
{
 struct i2c_client *client=to_i2c_client(dev);int r=t380_stock_suspend(dev);
 if(client->addr==0x49) {
  struct t380_control *c=i2c_get_clientdata(client);
  if(c->stock){WRITE_ONCE(c->stock->running,false);alarm_cancel(&c->stock->alarm);}
 }
 if(!r)r=t380_stock_gpio(dev,3);else t380_stock_gpio(dev,3);return r;
}
static const struct dev_pm_ops t380_stock_pm={.suspend=t380_stock_suspend,.resume=t380_stock_resume,
 .freeze=t380_stock_terminal,.thaw=t380_stock_resume,.poweroff=t380_stock_terminal,.restore=t380_stock_resume};
static int t380_stock_remove(struct i2c_client *c){return t380_stock_terminal(&c->dev);}
static void t380_stock_shutdown(struct i2c_client *c){t380_stock_terminal(&c->dev);}
static int t380_stock_property(struct power_supply *psy,struct i2c_client *client,
 enum power_supply_property prop,union power_supply_propval *v)
{
 struct t380_control *c;struct t380_stock_runtime *p;int r=0;
 if(client->addr==0x71) {
  struct t380_battery_thermal *sensor=i2c_get_clientdata(client);
  if(prop!=POWER_SUPPLY_PROP_PRESENT && prop!=POWER_SUPPLY_PROP_HEALTH)
   return t380_ga_android_property(sensor,prop,&v->intval);
  client=sensor->actor->charger;if(!client)return -ENODATA;
 }
 c=i2c_get_clientdata(client);if(!c || !c->stock)return -ENODATA;p=c->stock;
 mutex_lock(&c->lock);
 if(p->error || !p->sampled_ms || t380_source_now()-p->sampled_ms>65000)r=-ENODATA;
 else if(prop==POWER_SUPPLY_PROP_PRESENT)v->intval=p->input.battery_present;
 else if(prop==POWER_SUPPLY_PROP_HEALTH) {
  v->intval=!p->input.hardware_ok?POWER_SUPPLY_HEALTH_UNSPEC_FAILURE:
   p->state.health==T380_STOCK_COLD?POWER_SUPPLY_HEALTH_COLD:
   p->state.health!=T380_STOCK_GOOD?POWER_SUPPLY_HEALTH_OVERHEAT:
   p->state.timer_expired?POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE:POWER_SUPPLY_HEALTH_GOOD;
 }
 else if(prop==POWER_SUPPLY_PROP_ONLINE)v->intval=p->input.source!=T380_STOCK_ABSENT;
 else if(prop==POWER_SUPPLY_PROP_STATUS)v->intval=p->status;
 else r=-EINVAL;
 mutex_unlock(&c->lock);return r;
}
#endif
