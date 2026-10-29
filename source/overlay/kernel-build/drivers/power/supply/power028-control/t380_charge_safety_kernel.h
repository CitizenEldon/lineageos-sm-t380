/* SPDX-License-Identifier: GPL-2.0-only */
/* One physical GPIO owner. Stock policy owns enable; no trials or host leases. */
#ifndef T380_CHARGE_SAFETY_KERNEL_H
#define T380_CHARGE_SAFETY_KERNEL_H
#include <linux/pm.h>
struct t380_inhibit {
 struct gpio_desc *gpio;
 struct mutex lock;
 bool enabled, autonomous;
};
static int t380_owner_check_locked(struct t380_inhibit *s)
{
 struct gpio_chip *chip=gpiod_to_chip(s->gpio);
 const char *owner=gpiochip_is_requested(chip,desc_to_gpio(s->gpio)-chip->base);
 int r;
 if (!owner || strcmp(owner,"t380-charge-safety")) return -EPERM;
 r=gpiod_get_direction(s->gpio);
 if (r) return r<0?r:-EIO;
 r=gpiod_get_raw_value_cansleep(s->gpio);
 return r<0?r:r==!s->enabled?0:-EIO;
}
static int t380_owner_off_locked(struct t380_inhibit *s)
{
 int r=gpiod_direction_output_raw(s->gpio,1);
 if (r) return r;
 r=gpiod_get_raw_value_cansleep(s->gpio);
 if (r!=1) return r<0?r:-EIO;
 s->enabled=false;
 return t380_owner_check_locked(s);
}
/* -1 observation, 0 OFF, 1 enable, 2 monitor registered, 3 monitor removed,
 * 4 strictly verify inhibited. No userspace enable or ownership handoff. */
static int t380_stock_gpio(struct device *dev,int op)
{
 struct device_node *node=of_parse_phandle(dev->of_node,"samsung,charge-inhibit",0);
 struct platform_device *pdev;
 struct t380_inhibit *s;
 int r=-EPROBE_DEFER;
 if (!node) return op==4?0:-EINVAL;
 pdev=of_find_device_by_node(node);of_node_put(node);
 if (!pdev) return r;
 s=platform_get_drvdata(pdev);
 if (!s) goto done;
 mutex_lock(&s->lock);
 if (op==0 || op==3) {
  r=t380_owner_off_locked(s);
  if (op==3) s->autonomous=false;
 } else {
  r=t380_owner_check_locked(s);
  if (r) { t380_owner_off_locked(s); goto unlock; }
  if (op==1) {
   if (!s->autonomous) {r=-ESHUTDOWN;goto unlock;}
   r=gpiod_direction_output_raw(s->gpio,0);
   if (!r) {s->enabled=true;r=t380_owner_check_locked(s);}
   if (r) t380_owner_off_locked(s);
  } else if (op==2) s->autonomous=true;
  else if (op==4) r=s->enabled?-EBUSY:0;
  else r=s->enabled?1:0;
 }
unlock:
 mutex_unlock(&s->lock);
done:
 put_device(&pdev->dev);return r;
}
int t380_charge_inhibit_check(struct device *dev) {return t380_stock_gpio(dev,4);}
EXPORT_SYMBOL_GPL(t380_charge_inhibit_check);
int t380_charge_force_off(struct device *dev) {return t380_stock_gpio(dev,0);}
EXPORT_SYMBOL_GPL(t380_charge_force_off);
int t380_charge_safety_ready(struct device *dev)
{int r=t380_stock_gpio(dev,-1);return r<0?r:0;}
EXPORT_SYMBOL_GPL(t380_charge_safety_ready);
static int t380_charge_observation_mode(struct device *dev) {return t380_stock_gpio(dev,-1);}
static void t380_owner_cleanup(void *arg)
{
 struct t380_inhibit *s=arg;
 mutex_lock(&s->lock);s->autonomous=false;
 if (t380_owner_off_locked(s)) pr_err("t380: GPIO OFF failed during teardown\n");
 mutex_unlock(&s->lock);
}
static int t380_inhibit_probe(struct platform_device *pdev)
{
 struct t380_inhibit *s;
 struct of_phandle_args args;
 int r;
 if (!of_property_read_bool(pdev->dev.of_node,"samsung,safety-owner-inhibit") ||
     of_property_read_bool(pdev->dev.of_node,"samsung,gpio-hog-inhibit")) return -EINVAL;
 r=of_parse_phandle_with_args(pdev->dev.of_node,"samsung,charge-disable-gpios","#gpio-cells",0,&args);
 if (r) return r;
 r=of_device_is_compatible(args.np,"qcom,msm8917-pinctrl") &&
   args.args_count==2 && args.args[0]==129 && args.args[1]==0?0:-EINVAL;
 of_node_put(args.np);if(r)return r;
 s=devm_kzalloc(&pdev->dev,sizeof(*s),GFP_KERNEL);if(!s)return -ENOMEM;
 mutex_init(&s->lock);
 s->gpio=devm_gpiod_get_from_of_node(&pdev->dev,pdev->dev.of_node,
  "samsung,charge-disable-gpios",0,GPIOD_OUT_HIGH,"t380-charge-safety");
 if(IS_ERR(s->gpio))return PTR_ERR(s->gpio);
 r=t380_owner_check_locked(s);if(r)return r;
 r=devm_add_action_or_reset(&pdev->dev,t380_owner_cleanup,s);if(r)return r;
 platform_set_drvdata(pdev,s);return 0;
}
static int t380_owner_pm(struct device *dev)
{
 struct t380_inhibit *s=dev_get_drvdata(dev);int r;
 mutex_lock(&s->lock);
 r=s->autonomous?t380_owner_check_locked(s):t380_owner_off_locked(s);
 if(r)t380_owner_off_locked(s);
 mutex_unlock(&s->lock);return r;
}
static int t380_owner_terminal(struct device *dev)
{
 struct t380_inhibit *s=dev_get_drvdata(dev);int r;
 mutex_lock(&s->lock);s->autonomous=false;r=t380_owner_off_locked(s);
 mutex_unlock(&s->lock);return r;
}
static void t380_safety_shutdown(struct platform_device *pdev)
{if(t380_owner_terminal(&pdev->dev))dev_err(&pdev->dev,"GPIO OFF failed\n");}
static int t380_safety_remove(struct platform_device *pdev)
{t380_safety_shutdown(pdev);return 0;}
static const struct dev_pm_ops t380_safety_pm={
 .suspend=t380_owner_pm,.resume=t380_owner_pm,
 .freeze=t380_owner_terminal,.thaw=t380_owner_pm,
 .poweroff=t380_owner_terminal,.restore=t380_owner_pm,
};
static const struct of_device_id t380_inhibit_match[]={
 {.compatible="samsung,t380-charge-inhibit"},{}
};
static struct platform_driver t380_inhibit_driver={
 .probe=t380_inhibit_probe,.remove=t380_safety_remove,.shutdown=t380_safety_shutdown,
 .driver={.name="t380-charge-inhibit",.of_match_table=t380_inhibit_match,
 .suppress_bind_attrs=true,.pm=&t380_safety_pm},
};
#endif
