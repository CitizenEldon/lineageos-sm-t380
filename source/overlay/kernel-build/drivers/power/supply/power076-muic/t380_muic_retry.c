/* SPDX-License-Identifier: GPL-2.0-only */
#include "t380_muic_retry.h"
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif
static int t380_mr_tuple_read(const struct t380_mr_io *io, struct t380_mr_tuple *t)
{
 static const unsigned char regs[]={0x0a,0x0b,0x15,0x07,0x1d,0x24};
 int *fields[]={&t->dev1,&t->dev2,&t->dev3,&t->adc,&t->vbus,&t->chgtype};
 unsigned int i;
 for(i=0;i<6;i++){int r=io->read(io->context,regs[i]);if(r<0)return r;*fields[i]=r;}
 return 0;
}
static int t380_mr_same(const struct t380_mr_tuple *a,const struct t380_mr_tuple *b)
{
 return a->dev1==b->dev1 && a->dev2==b->dev2 && a->dev3==b->dev3 &&
  a->adc==b->adc && a->vbus==b->vbus && a->chgtype==b->chgtype;
}
static enum t380_mr_kind t380_mr_kind(const struct t380_mr_tuple *t)
{
 if(!(t->vbus&2))return T380_MR_ABSENT;
 /* No accessory/OTG/jig redetection or routing authority. */
 if(t->dev2 || t->adc!=0x1f)return T380_MR_UNKNOWN;
 if(t->dev1==4 && !t->dev3 && (t->chgtype&31)==4)return T380_MR_USB;
 if(t->dev1==0x20 && !t->dev3 && (t->chgtype&31)==2)return T380_MR_CDP;
 if(t->dev1==0x40 && !t->dev3 && (t->chgtype&31)==1)return T380_MR_TA;
 if(!t->dev1 && t->dev3==4 && (t->chgtype&31)==8)return T380_MR_TIMEOUT;
 return T380_MR_UNKNOWN;
}
static int t380_mr_config(const struct t380_mr_io *io,int *values)
{
 unsigned int i;
 for(i=0;i<T380_MR_CONFIG_COUNT;i++){
  int r=io->read(io->context,t380_mr_config_regs[i]);if(r<0)return r;values[i]=r;
 }
 return 0;
}
static int t380_mr_write(const struct t380_mr_io *io,unsigned char reg,int val)
{
 int r=io->write(io->context,reg,val);
 if(r<0)return r;
 r=io->read(io->context,reg);
 return r<0?r:r==val?0:-EIO;
}
static int t380_mr_update(const struct t380_mr_io *io,unsigned char reg,int mask,int val)
{
 int old=io->read(io->context,reg),next;
 if(old<0)return old;
 next=(old&~mask)|(val&mask);
 return next==old?0:t380_mr_write(io,reg,next);
}
static int t380_mr_route(const struct t380_mr_io *io,enum t380_mr_kind kind)
{
 int r;
 /* Stock COM_USB_AP=0x25; COM_OPEN_WITH_V_BUS=1. Always retain VBUS. */
 r=t380_mr_write(io,0x13,kind==T380_MR_TA?1:0x25);if(r)return r;
 r=t380_mr_write(io,0x14,kind==T380_MR_TA?0x10:0);if(r)return r;
 r=t380_mr_update(io,0x02,4,0);if(r)return r;
 return t380_mr_update(io,0x20,0x20,kind==T380_MR_TA?0x20:0);
}
static int t380_mr_detach(const struct t380_mr_io *io)
{
 int r=io->off(io->context);if(r)return r;
 r=t380_mr_write(io,0x13,0);if(r)return r;
 r=t380_mr_write(io,0x14,0);if(r)return r;
 r=t380_mr_update(io,0x02,4,4);if(r)return r;
 return t380_mr_update(io,0x20,0x20,0);
}
static int t380_mr_finish(struct t380_mr_state *s,const struct t380_mr_io *io)
{
 struct t380_mr_tuple a,b;
 enum t380_mr_kind kind;
 int r;
 r=io->source_ok(io->context);if(r<=0)return r;
 r=t380_mr_tuple_read(io,&a);if(r)return r;
 io->sleep_ms(io->context,20);
 r=t380_mr_tuple_read(io,&b);if(r)return r;
 s->after=b;
 kind=t380_mr_kind(&b);
 /* Reset-generated absence/unknown must neither rearm nor become USB/DCP.
  * Keep data open; subsequent ordinary polls can finish routing. */
 if(!t380_mr_same(&a,&b) || kind==T380_MR_UNKNOWN || kind==T380_MR_ABSENT)
  return 0;
 r=io->source_ok(io->context);if(r<=0)return r;
 s->stage=T380_MR_ROUTE;
 r=t380_mr_route(io,kind);if(r)return r;
 r=t380_mr_config(io,s->config_after);if(r)return r;
 s->after_valid=1;s->pending_route=0;s->routed_kind=kind;
 s->completed++;s->stage=T380_MR_COMPLETE;
 return 0;
}
int t380_mr_poll(struct t380_mr_state *s,const struct t380_mr_io *io,
 int charger_vbus_ok,int sink_only,int ccic_mode,unsigned long long now_ms)
{
 struct t380_mr_tuple t;
 static const unsigned char init_regs[]={0x05,0x06,0x08,0x09,0x20,0x3a};
 static const unsigned char init_vals[]={0xdc,0,0,1,6,0};
 unsigned int i;
 int r,detached_managed;
 r=t380_mr_tuple_read(io,&t);if(r)return r;
 /* The caller's charger VBUS sample is independent of MUIC RESET. */
 if(!charger_vbus_ok && !(t.vbus&2)){
  if(s->absent)return s->error;
  s->absent=1;
  detached_managed=s->managed;
  if(s->managed){s->stage=T380_MR_DETACH;r=t380_mr_detach(io);if(r)goto failed;}
  if(s->spent || s->candidate || s->pending_route)s->generation++;
  s->spent=s->pending_route=s->candidate=0;s->error=0;s->stage=T380_MR_IDLE;
  s->managed=0;s->routed_kind=T380_MR_ABSENT;
  return detached_managed?1:0;
 }
 s->absent=0;
 if(s->error)return s->error;
 if(!charger_vbus_ok || !sink_only || !ccic_mode){s->candidate=0;return 0;}
 if(s->spent){
  enum t380_mr_kind kind=t380_mr_kind(&t);
  /* A quick physical cable swap can fall between polls. Restore data route
   * for its fresh valid class without spending another reset. */
  if(s->managed && kind>T380_MR_ABSENT && kind!=s->routed_kind)s->pending_route=1;
  if(s->pending_route){r=io->off(io->context);if(r)goto failed;
   r=t380_mr_finish(s,io);if(r)goto failed;return 1;}
  return 0;
 }
 if(t380_mr_kind(&t)!=T380_MR_TIMEOUT){s->candidate=0;return 0;}
 if(!s->candidate || !t380_mr_same(&s->previous,&t)){
  s->candidate=1;s->candidate_ms=now_ms;s->previous=t;return 0;
 }
 if(now_ms-s->candidate_ms<20)return 0;
 r=io->source_ok(io->context);if(r<0)return r;if(!r)return 0;
 /* Spend before the first mutation, including a failed OFF request. */
 s->spent=1;s->candidate=0;s->attempts++;s->attempted_ms=now_ms;s->before=t;
 s->before_valid=s->after_valid=0;s->error=0;s->stage=T380_MR_OFF;
 r=io->off(io->context);if(r)goto failed;
 r=t380_mr_config(io,s->config_before);if(r)goto failed;s->before_valid=1;
 s->managed=1;
 s->stage=T380_MR_OPEN_DM;r=t380_mr_update(io,0x13,0xe0,0);if(r)goto failed;
 s->stage=T380_MR_OPEN_DP;r=t380_mr_update(io,0x13,0x1c,0);if(r)goto failed;
 s->stage=T380_MR_MANUAL;r=t380_mr_update(io,0x02,4,0);if(r)goto failed;
 s->stage=T380_MR_RESET;
 /* RESET self-clears: do not require a persistent1 readback or retry writes. */
 r=io->read(io->context,0x1b);if(r<0)goto failed;
 r=io->write(io->context,0x1b,r|1);if(r<0)goto failed;
 r=io->read(io->context,0x1b);if(r<0)goto failed;
 s->stage=T380_MR_INIT;
 for(i=0;i<sizeof(init_regs);i++){
  r=t380_mr_write(io,init_regs[i],init_vals[i]);if(r)goto failed;
 }
 s->stage=T380_MR_UNMASK;r=t380_mr_update(io,0x02,1,0);if(r)goto failed;
 s->stage=T380_MR_SETTLE;s->pending_route=1;
 /* Stock completes asynchronously via IRQ. This polling port allows one
  * existing 2s monitor period, then requires two fresh stable reads. */
 io->sleep_ms(io->context,2000);
 r=t380_mr_finish(s,io);if(r)goto failed;
 return 1;
failed:
 s->error=r;s->failures++;
 return r;
}
