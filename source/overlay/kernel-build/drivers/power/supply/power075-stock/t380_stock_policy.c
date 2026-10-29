/* SPDX-License-Identifier: GPL-2.0-only */
#include "t380_stock_policy.h"

/* r05 battery,age_data. Unknown cycle count retains the configured/default
 * step, as sec_bat_aging_check does; it is reported separately as unknown. */
const struct t380_stock_age t380_stock_ages[5] = {
 {0,4300,4230,4200,93}, {200,4280,4210,4180,92},
 {250,4260,4190,4160,91}, {300,4240,4170,4140,90},
 {1000,4190,4120,4090,89}
};
#define STOCK_NORMAL_BUDGET (17400ULL * 1000 * 1883)
#define STOCK_RECHARGE_BUDGET (5400ULL * 1000 * 1883)
static int stock_min(int a, int b) { return a < b ? a : b; }
void t380_stock_init(struct t380_stock_state *s)
{
 *s = (struct t380_stock_state){ .phase=T380_STOCK_FIRST,
  .health=T380_STOCK_GOOD, .remaining_ma_ms=STOCK_NORMAL_BUDGET };
}
static void stock_thermal(struct t380_stock_state *s, int t)
{
 if (t >= 800) s->health=T380_STOCK_HOT_LIMIT;
 else if (s->health==T380_STOCK_HOT_LIMIT) {
  if (t <= 750) s->health=T380_STOCK_HOT;
 } else if (t >= 500) s->health=T380_STOCK_HOT;
 else if (t <= 0) s->health=T380_STOCK_COLD;
 else if (t >= 50 && t <= 450) s->health=T380_STOCK_GOOD;
}
void t380_stock_step(struct t380_stock_state *s,
 const struct t380_stock_input *in, struct t380_stock_output *o)
{
 const struct t380_stock_age *age;
 t380_stock_u64 elapsed=0, debit;
 int i, effective, recharge_mv;
 bool thermal_recovered;
 *o=(struct t380_stock_output){ .reason=T380_STOCK_MEASUREMENT_ERROR };
 if (!s || !in) return;
 if (!s->initialized) { s->initialized=true; s->last_ms=in->now_ms; }
 if (in->now_ms < s->last_ms) goto finish;
 elapsed=in->now_ms-s->last_ms;
 s->last_ms=in->now_ms;
 /* A known stock cycle count selects its board age row. No exact gauge-table
  * equality, learned-table write, or inferred battery identity is involved. */
 if (in->measurements_ok && in->cycle_count>=0 && in->temperature_decic>=50) {
  for (i=4;i>=0;i--) if (in->cycle_count>=t380_stock_ages[i].cycles) break;
  s->age_step=i;
 }
 age=&t380_stock_ages[s->age_step];
 o->age_known=in->cycle_count>=0; o->age_step=s->age_step;
 o->float_mv=age->float_mv; o->topoff_ma=350;
 if (!in->measurements_ok || in->capacity<0 || in->capacity>100) goto finish;
 if (!in->battery_present) { o->reason=T380_STOCK_BATTERY_ABSENT; goto finish; }
 if (!in->hardware_ok) { o->reason=T380_STOCK_HARDWARE_FAULT; goto finish; }
 if (in->source==T380_STOCK_ABSENT) {
  int step=s->age_step;
  t380_stock_init(s); s->age_step=step;
  s->initialized=true; s->last_ms=in->now_ms;
  o->reason=T380_STOCK_NO_SOURCE; goto finish;
 }
 switch (in->source) {
 case T380_STOCK_SDP:
  o->input_ma=o->fast_ma=(!in->usb_configured && !in->charger_mode)?100:500;
  break;
 case T380_STOCK_CDP: o->input_ma=o->fast_ma=1000; break;
 case T380_STOCK_MAINS: o->input_ma=1800; o->fast_ma=2100; break;
 case T380_STOCK_TIMEOUT: o->input_ma=o->fast_ma=500; break;
 default: o->reason=T380_STOCK_SOURCE_UNCLASSIFIED; goto finish;
 }
 s->attached=true;
 /* Stock weighted safety clock; display state affects timer accounting, not
  * eligibility. No host process, attendance, SOC target or short trial timer. */
 if (s->was_charging && !(in->display_on && s->previous_display_on)) {
  debit=elapsed > STOCK_NORMAL_BUDGET ? STOCK_NORMAL_BUDGET : elapsed;
  debit*=s->last_effective_ma;
  s->remaining_ma_ms=debit>=s->remaining_ma_ms?0:s->remaining_ma_ms-debit;
 }
 if (in->monitor_tick) {
  if (in->average_ma<0) s->negative_samples++; else s->negative_samples=0;
 }
 if (s->negative_samples>=5) {
  s->remaining_ma_ms=STOCK_NORMAL_BUDGET; s->negative_samples=0;
 }
 if (!s->remaining_ma_ms) s->timer_expired=true;
 if (s->timer_expired) { o->reason=T380_STOCK_SAFETY_TIMER; goto finish; }
 thermal_recovered=s->health!=T380_STOCK_GOOD;
 stock_thermal(s,in->temperature_decic);
 thermal_recovered=thermal_recovered && s->health==T380_STOCK_GOOD;
 if (s->health!=T380_STOCK_GOOD) {
  o->float_mv=4120; o->reason=T380_STOCK_THERMAL_STOP; goto finish;
 }
 if (in->temperature_decic<=150) s->low_current=true;
 else if (in->temperature_decic>=200) s->low_current=false;
 if (!s->swelling && (in->temperature_decic>=410 ||
  in->temperature_decic<=50 || (thermal_recovered &&
   (in->temperature_decic>=390 || in->temperature_decic<=100)))) {
  s->swelling=true; s->swelling_charging=false; s->swelling_full=false;
  s->remaining_ma_ms=STOCK_NORMAL_BUDGET;
 }
 if (s->swelling && in->temperature_decic>=100 && in->temperature_decic<=390) {
  s->swelling=false; s->swelling_charging=false; s->swelling_full=false;
  s->phase=T380_STOCK_FIRST;
 }
 if (s->swelling) {
  o->float_mv=4120; o->topoff_ma=250;
  o->fast_ma=stock_min(o->fast_ma,in->temperature_decic<=100?1050:1550);
  if (!s->swelling_charging && in->voltage_mv<3950) {
   s->swelling_charging=true; s->swelling_full=false;
  }
  if (in->monitor_tick && s->swelling_charging && in->charging_feedback &&
   in->current_ma>0 && in->current_ma<350 &&
   in->average_ma>0 && in->average_ma<250) {
   s->swelling_charging=false; s->swelling_full=true;
   s->remaining_ma_ms=STOCK_NORMAL_BUDGET;
  }
  if (!s->swelling_charging) { o->reason=T380_STOCK_SWELLING_REST; goto finish; }
 } else {
  if (s->low_current) o->fast_ma=stock_min(o->fast_ma,1050);
  recharge_mv=s->low_current?age->float_mv-150:age->recharge_mv;
  if (s->phase==T380_STOCK_REST) {
   if (in->voltage_mv>recharge_mv) { o->reason=T380_STOCK_FULL; goto finish; }
   s->phase=T380_STOCK_FIRST; s->recharging=true;
   s->remaining_ma_ms=STOCK_RECHARGE_BUDGET;
  }
  if (in->monitor_tick && in->charging_feedback && in->capacity>=age->full_soc &&
   in->voltage_mv>=age->full_mv && in->current_ma>0 && in->current_ma<350 &&
   in->average_ma>0 && in->average_ma<(s->phase==T380_STOCK_FIRST?350:250)) {
   /* Samsung keeps learning CHARGE_FULL(displayed capacity) on every
    * qualifying monitor sample below100; full_soc is an admission threshold
    * for learning, not permission to terminate below a completed SOC scale. */
   o->full_candidate=true;
   if (in->capacity==100) {
    if (s->phase==T380_STOCK_FIRST) s->phase=T380_STOCK_SECOND;
    else { s->phase=T380_STOCK_REST; s->recharging=false;
     o->reason=T380_STOCK_FULL; goto finish; }
   }
  }
 }
 if (in->aicl_input_ma>0) o->input_ma=stock_min(o->input_ma,in->aicl_input_ma);
 effective=o->input_ma*5000;
 effective=effective>o->fast_ma*o->float_mv?o->fast_ma:
  (effective/o->float_mv)*9/10;
 s->last_effective_ma=effective;
 o->charge=true; o->reason=T380_STOCK_CHARGING;
finish:
 o->phase=s->phase; o->health=s->health;
 o->full=s->phase!=T380_STOCK_FIRST || s->swelling_full;
 s->was_charging=o->charge && in->charging_feedback;
 s->previous_display_on=in->display_on;
}
