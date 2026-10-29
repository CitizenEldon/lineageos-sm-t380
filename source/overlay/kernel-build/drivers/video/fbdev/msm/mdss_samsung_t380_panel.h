/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MDSS_SAMSUNG_T380_PANEL_H
#define MDSS_SAMSUNG_T380_PANEL_H
#include <linux/types.h>
struct mdss_dsi_ctrl_pdata;
bool mdss_samsung_t380_backlight(struct mdss_dsi_ctrl_pdata *ctrl, int level);
void mdss_samsung_t380_pre_reset_off(struct mdss_dsi_ctrl_pdata *ctrl);
#endif
