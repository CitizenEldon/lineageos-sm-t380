/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_MUIC_RETRY_H
#define T380_MUIC_RETRY_H
/* One legacy 5 V sink redetection per independently observed cable episode.
 * Constants/sequence: Samsung 36c20b2257535bf07ef1439aae813b843932f5f4,
 * drivers/muic/universal/muic_regmap_sm5703.c:367-417. */
enum t380_mr_stage { T380_MR_IDLE, T380_MR_OFF, T380_MR_OPEN_DM,
 T380_MR_OPEN_DP, T380_MR_MANUAL, T380_MR_RESET, T380_MR_INIT,
 T380_MR_UNMASK, T380_MR_SETTLE, T380_MR_ROUTE, T380_MR_COMPLETE,
 T380_MR_DETACH };
enum t380_mr_kind { T380_MR_UNKNOWN, T380_MR_ABSENT, T380_MR_USB,
 T380_MR_CDP, T380_MR_TA, T380_MR_TIMEOUT };
struct t380_mr_tuple { int dev1, dev2, dev3, adc, vbus, chgtype; };
#define T380_MR_CONFIG_COUNT 10
struct t380_mr_state {
 unsigned int spent, pending_route, candidate, attempts, completed, failures;
 unsigned int generation, stage;
 unsigned int managed, absent, routed_kind;
 int error, before_valid, after_valid;
 unsigned long long candidate_ms, attempted_ms;
 struct t380_mr_tuple previous, before, after;
 int config_before[T380_MR_CONFIG_COUNT], config_after[T380_MR_CONFIG_COUNT];
};
struct t380_mr_io {
 void *context;
 int (*read)(void *, unsigned char);
 int (*write)(void *, unsigned char, unsigned char);
 void (*sleep_ms)(void *, unsigned int);
 int (*off)(void *);
 /* Refresh independent charger VBUS/sink facts:1 allowed,0 absent/role
  * changed,negative actual I/O error. Must not acquire MUIC/control lock. */
 int (*source_ok)(void *);
};
static const unsigned char t380_mr_config_regs[T380_MR_CONFIG_COUNT] =
 { 0x02,0x05,0x06,0x08,0x09,0x13,0x14,0x20,0x21,0x3a };
int t380_mr_poll(struct t380_mr_state *, const struct t380_mr_io *,
 int charger_vbus_ok, int sink_only, int ccic_mode,
 unsigned long long now_ms);
#endif
