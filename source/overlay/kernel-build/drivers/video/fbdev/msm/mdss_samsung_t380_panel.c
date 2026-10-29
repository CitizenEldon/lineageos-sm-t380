// SPDX-License-Identifier: GPL-2.0-only
/* SM-T380 S6D7AA0X11: only the operations generic MDSS does not implement.
 * Duty values come from samsung,candela_map_table_revA in the captured
 * Rev05 DT, not the different generic Samsung panel include.
 */
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include "mdss_dsi.h"
#include "mdss_samsung_t380_panel.h"

static const u8 t380_pwm_duty[] = {
	0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3,
	4, 4, 5, 5, 5, 6, 6, 7, 7, 7, 8, 8, 9, 9, 9, 10,
	10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 15, 15, 15, 16, 16,
	17, 17, 17, 18, 18, 19, 19, 19, 20, 20, 21, 21, 21, 22, 22, 23,
	23, 23, 24, 24, 25, 25, 25, 26, 26, 27, 27, 27, 28, 28, 29, 29,
	29, 30, 30, 31, 32, 33, 34, 35, 36, 36, 37, 38, 39, 40, 41, 42,
	43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58,
	59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74,
	75, 76, 77, 78, 79, 81, 82, 83, 84, 85, 87, 88, 89, 90, 91, 93,
	94, 95, 97, 99, 100, 101, 102, 103, 105, 106, 107, 108, 109, 111, 112, 113,
	114, 116, 117, 118, 119, 120, 121, 123, 124, 125, 126, 127, 129, 130, 132, 133,
	135, 136, 137, 138, 139, 141, 142, 143, 144, 145, 147, 149, 150, 151, 153, 154,
	155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170,
	171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186,
	187, 188, 188, 188, 188, 188, 189, 189, 189, 189, 189, 190, 190, 190, 190, 190,
	190, 190, 190, 190, 190, 191, 191, 191, 191, 191, 192, 192, 192, 192, 192, 193,
	223
};

static bool t380_panel(struct mdss_dsi_ctrl_pdata *ctrl)
{
	return !strcmp(ctrl->panel_data.panel_info.panel_name,
				   "ss_dsi_panel_S6D7AA0X11_TV080WXM_WXGA");
}

bool mdss_samsung_t380_backlight(struct mdss_dsi_ctrl_pdata *ctrl, int level)
{
	static DEFINE_MUTEX(pwm_lock);
	static char payload[2];
	static struct dsi_cmd_desc cmd = {
		{DTYPE_DCS_LWRITE, 1, 0, 0, 0, sizeof(payload)}, payload
	};
	struct dcs_cmd_req req = {0};
	int ret;

	if (!t380_panel(ctrl))
		return false;
	if (!ctrl->cmdlist_commit) {
		pr_err("T380 panel: DSI command commit unavailable\n");
		return true;
	}

	/* The legacy table maps 0..256 to a byte. Directly casting 256 to
	 * u8 would extinguish the panel at the userspace maximum.
	 */
	mutex_lock(&pwm_lock);
	payload[0] = 0x51;
	payload[1] = t380_pwm_duty[clamp(level, 0, 256)];
	req.cmds = &cmd;
	req.cmds_cnt = 1;
	req.flags = CMD_REQ_COMMIT | CMD_CLK_CTRL | CMD_REQ_HS_MODE;
	/* COMMIT normally consumes and transmits synchronously. Keep the
	 * descriptor and payload persistent even on a failed commit path;
	 * cmdlist_put copies req by value, including these pointers.
	 */
	ret = mdss_dsi_cmdlist_put(ctrl, &req);
	mutex_unlock(&pwm_lock);
	if (ret < 0)
		pr_err("T380 panel: brightness command failed: %d\n", ret);
	return true;
}

void mdss_samsung_t380_pre_reset_off(struct mdss_dsi_ctrl_pdata *ctrl)
{
	if (t380_panel(ctrl))
		usleep_range(5000, 5100);
}
