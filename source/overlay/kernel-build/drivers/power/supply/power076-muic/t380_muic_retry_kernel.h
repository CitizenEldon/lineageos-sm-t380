/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T380_MUIC_RETRY_KERNEL_H
#define T380_MUIC_RETRY_KERNEL_H
/* Include after t380_muic_extcon.h. Caller holds its charger lock and the
 * shared MUIC mutex across this helper and subsequent source re-observation. */
#include <linux/delay.h>
#include "t380_muic_retry.c"
static int t380_mr_ccic_mode;
static int __init t380_mr_ccic_setup(char *value)
{
 int mode;
 if(!value || kstrtoint(value,0,&mode))return -EINVAL;
 t380_mr_ccic_mode=!!(mode&1);return 0;
}
early_param("ccic_info",t380_mr_ccic_setup);
struct t380_mr_kernel_io {
 struct i2c_client *muic;
 int (*off)(void *);
 int (*source_ok)(void *);
 void *off_context;
};
static int t380_mr_kernel_read(void *context,unsigned char reg)
{struct t380_mr_kernel_io *k=context;return i2c_smbus_read_byte_data(k->muic,reg);}
static int t380_mr_kernel_write(void *context,unsigned char reg,unsigned char value)
{struct t380_mr_kernel_io *k=context;return i2c_smbus_write_byte_data(k->muic,reg,value);}
static void t380_mr_kernel_sleep(void *context,unsigned int ms)
{(void)context;msleep(ms);}
static int t380_mr_kernel_off(void *context)
{struct t380_mr_kernel_io *k=context;return k->off(k->off_context);}
static int t380_mr_kernel_source_ok(void *context)
{struct t380_mr_kernel_io *k=context;return k->source_ok(k->off_context);}
static int t380_muic_retry_poll_locked(struct i2c_client *muic,
 bool charger_vbus_ok,bool sink_only,int (*off)(void *),
 int (*source_ok)(void *),void *off_context)
{
 struct t380_muic_extcon *m=i2c_get_clientdata(muic);
 struct t380_mr_kernel_io k={muic,off,source_ok,off_context};
 struct t380_mr_io io={&k,t380_mr_kernel_read,t380_mr_kernel_write,
  t380_mr_kernel_sleep,t380_mr_kernel_off,t380_mr_kernel_source_ok};
 if(!m || !off || !source_ok)return -ENODEV;
 lockdep_assert_held(&t380_muic_observation_lock);
 return t380_mr_poll(&m->retry,&io,charger_vbus_ok,sink_only,
  t380_mr_ccic_mode,t380_source_now());
}
static bool t380_muic_retry_ready_locked(struct i2c_client *muic)
{
 struct t380_muic_extcon *m=i2c_get_clientdata(muic);
 lockdep_assert_held(&t380_muic_observation_lock);
 return m && !m->retry.pending_route && !m->retry.error;
}
#endif
