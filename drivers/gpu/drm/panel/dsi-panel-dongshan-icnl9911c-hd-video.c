/*
 * Copyright (c) 2015 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/backlight.h>
#include <drm/drmP.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>

#define CONFIG_MTK_PANEL_EXT
#if defined(CONFIG_MTK_PANEL_EXT)
#include "../mediatek/mtk_panel_ext.h"
#include "../mediatek/mtk_log.h"
#include "../mediatek/mtk_drm_graphics_base.h"
#endif

#ifdef CONFIG_MTK_ROUND_CORNER_SUPPORT
#include "../mediatek/mtk_corner_pattern/mtk_data_hw_roundedpattern.h"
#endif

#if defined(CONFIG_LEDS_MTK_I2C)
#include "../../../misc/mediatek/leds/leds-mtk-i2c.h"
#endif

extern int mtk_atoi(const char *str);

struct lcm {
	struct device *dev;
	struct drm_panel panel;
	struct backlight_device *backlight;
	struct gpio_desc *pm_enable_gpio;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *bias_pos, *bias_neg;
	struct gpio_desc *bl_iset_en_gpio;

	bool prepared;
	bool enabled;

	int error;
	unsigned int cabc_mode;
};

#define lcm_dcs_write_seq(ctx, seq...) \
({\
	const u8 d[] = { seq };\
	BUILD_BUG_ON_MSG(ARRAY_SIZE(d) > 64, "DCS sequence too big for stack");\
	lcm_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

#define lcm_dcs_write_seq_static(ctx, seq...) \
({\
	static const u8 d[] = { seq };\
	lcm_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

static inline struct lcm *panel_to_lcm(struct drm_panel *panel)
{
	return container_of(panel, struct lcm, panel);
}

static void lcm_dcs_write(struct lcm *ctx, const void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;
	char *addr;

	if (ctx->error < 0) {
		dev_err(ctx->dev, "%s: there is a error %zd before,now writing seq: %ph\n", __func__, ctx->error, data);
		ctx->error = 0;
	}

	addr = (char *)data;
	if ((int)*addr < 0xB0)
		ret = mipi_dsi_dcs_write_buffer(dsi, data, len);
	else
		ret = mipi_dsi_generic_write(dsi, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %zd writing seq: %ph\n", ret, data);
		ctx->error = ret;
	}
}

#if defined(CONFIG_RT5081_PMU_DSV) || defined(CONFIG_MT6370_PMU_DSV)
static struct regulator *disp_bias_pos;
static struct regulator *disp_bias_neg;


static int lcm_panel_bias_regulator_init(void)
{
	static int regulator_inited;
	int ret = 0;

	if (regulator_inited)
		return ret;

	/* please only get regulator once in a driver */
	disp_bias_pos = regulator_get(NULL, "dsv_pos");
	if (IS_ERR(disp_bias_pos)) { /* handle return value */
		ret = PTR_ERR(disp_bias_pos);
		pr_err("get dsv_pos fail, error: %d\n", ret);
		return ret;
	}

	disp_bias_neg = regulator_get(NULL, "dsv_neg");
	if (IS_ERR(disp_bias_neg)) { /* handle return value */
		ret = PTR_ERR(disp_bias_neg);
		pr_err("get dsv_neg fail, error: %d\n", ret);
		return ret;
	}

	regulator_inited = 1;
	return ret; /* must be 0 */

}

static int lcm_panel_bias_enable(void)
{
	int ret = 0;
	int retval = 0;

	lcm_panel_bias_regulator_init();

	/* set voltage with min & max*/
	ret = regulator_set_voltage(disp_bias_pos, 5400000, 5400000);
	if (ret < 0)
		pr_err("set voltage disp_bias_pos fail, ret = %d\n", ret);
	retval |= ret;

	ret = regulator_set_voltage(disp_bias_neg, 5400000, 5400000);
	if (ret < 0)
		pr_err("set voltage disp_bias_neg fail, ret = %d\n", ret);
	retval |= ret;

	/* enable regulator */
	ret = regulator_enable(disp_bias_pos);
	if (ret < 0)
		pr_err("enable regulator disp_bias_pos fail, ret = %d\n", ret);
	retval |= ret;

	ret = regulator_enable(disp_bias_neg);
	if (ret < 0)
		pr_err("enable regulator disp_bias_neg fail, ret = %d\n", ret);
	retval |= ret;

	return retval;
}

static int lcm_panel_bias_disable(void)
{
	int ret = 0;
	int retval = 0;

	lcm_panel_bias_regulator_init();

	ret = regulator_disable(disp_bias_neg);
	if (ret < 0)
		pr_err("disable regulator disp_bias_neg fail, ret = %d\n", ret);
	retval |= ret;

	ret = regulator_disable(disp_bias_pos);
	if (ret < 0)
		pr_err("disable regulator disp_bias_pos fail, ret = %d\n", ret);
	retval |= ret;

	return retval;
}
#endif

static void kernel_vref_reg_update(struct lcm *ctx)
{
	char *r = NULL;
	unsigned char vref_reg_buf[4] = {0};
	u8 vref_reg_num = 0;

	r = strstr(saved_command_line, "androidboot.vref_reg=");
	snprintf(vref_reg_buf, 4, "%s", (r+21));
	vref_reg_num = mtk_atoi(vref_reg_buf);
	lcm_dcs_write_seq(ctx, 0xF6, (const u8)vref_reg_num);
}

static void lcm_panel_init(struct lcm *ctx)
{
    struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
    struct device *dev = &dsi->dev;
    
    u8 dcs_f6_val;
    u8 write_buf[2];
    ssize_t ret;

	pr_info("%s\n", __func__);

	ctx->reset_gpio =
		devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(ctx->dev, "%s: cannot get reset_gpio %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return;
	}
	mdelay(10);
	gpiod_set_value(ctx->reset_gpio, 0);
	mdelay(2);
	gpiod_set_value(ctx->reset_gpio, 1);
	mdelay(10);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);

	/* 7 represents GPIOD_OUT_LOW or similar internal flags */
    ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
    
    if (IS_ERR(ctx->reset_gpio)) {
        dev_err(dev, "%s: cannot get reset gpio %ld\n", "lcm_panel_init", PTR_ERR(ctx->reset_gpio));
		return;
    }
	
    /* Initial Generic Writes */
    lcm_dcs_write_seq_static(ctx, 0xf0, 0x5a, 0x59); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xf1, 0xa5, 0xa6); // mipi_dsi_generic_write

    /* DCS Read-Modify-Write for register 0xF6 */
    if (ctx->error >= 0) {
        ret = mipi_dsi_dcs_read(dsi, 0xF6, &dcs_f6_val, 1);
        if (ret < 0) {
            dev_err(dev, "error %zd reading dcs seq:(%#x)\n", ret, 0xF6);
            ctx->error = (int)ret;
        }
    }

    /* Value modification (Brightness/Gamma cap logic) */
    dcs_f6_val += 0x10;
    if (dcs_f6_val > 0x3F) {
        dcs_f6_val = 0x3F;
    }

    write_buf[0] = 0xF6;
    write_buf[1] = dcs_f6_val;

    /* Write modified 0xF6 buffer back */
    if (ctx->error >= 0) {
        if (write_buf[1] < 0xB0) {
            ret = mipi_dsi_dcs_write_buffer(dsi, write_buf, 2);
        } else {
            ret = mipi_dsi_generic_write(dsi, write_buf, 2);
        }
        if (ret < 0) {
            dev_err(dev, "error %zd writing seq: %ph\n", ret, write_buf);
            ctx->error = (int)ret;
        }
    }

    lcm_dcs_write_seq_static(ctx, 0xc3, 0x6, 0x0, 0xff, 0x0, 0xff, 0x0, 0x0, 0x81, 0x1); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xc4, 0x84, 0x1, 0x2b, 0x41, 0x0, 0x3c, 0x0, 0x3, 0x3, 0x2e); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xc5, 0x3, 0x1c, 0x70, 0x54, 0x40, 0x10, 0x42, 0x44, 0x8, 0xe, 0x14); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xc6, 0x87, 0xa2, 0x24, 0x22, 0x22, 0x31, 0x7f, 0x34, 0x8, 0x4); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xc7, 0xf7, 0xc6, 0xa7, 0x8f, 0x63, 0x43, 0x11, 0x63, 0x2a, 0xfe, 0xd0, 0x9c, 0xf4, 0xc8, 0xab, 0x82, 0x6a, 0x47, 0x1a, 0x7f, 0xc0, 0x0); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xc8, 0xf7, 0xc6, 0xa7, 0x8f, 0x63, 0x43, 0x11, 0x63, 0x2a, 0xfe, 0xd0, 0x9c, 0xf4, 0xc8, 0xab, 0x82, 0x6a, 0x47, 0x1a, 0x7f, 0xc0, 0x0); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xcb, 0x0); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xd0, 0x80, 0xd, 0xff, 0xf, 0x63); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xd2, 0x42); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xfe, 0xff, 0xff, 0xff, 0x40); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xe0, 0x30, 0x0, 0x80, 0x88, 0x11, 0x3f, 0x22, 0x62, 0xdf, 0xa0, 0x4, 0xcc, 0x1, 0xff, 0xf6, 0xff, 0xf0, 0xfd, 0xff, 0xfd, 0xf8, 0xf5, 0xfc, 0xfc, 0xfd, 0xff); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xe1, 0xef, 0xfe, 0xfe, 0xfe, 0xfe, 0xee, 0xf0, 0x20, 0x33, 0xff, 0x0, 0x0, 0x6a, 0x90, 0xc0, 0xd, 0x6a, 0xf0, 0x3e, 0xff, 0x0, 0x6, 0x40); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xf1, 0x5a, 0x59); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xf0, 0xa5, 0xa6); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0x35, 0x0); // mipi_dsi_dcs_write_buffer
	lcm_dcs_write_seq_static(ctx, 0x51, 0x0, 0x0); // mipi_dsi_dcs_write_buffer

    lcm_dcs_write_seq_static(ctx, 0x35, 0x0); // mipi_dsi_dcs_write_buffer
	lcm_dcs_write_seq_static(ctx, 0x51, 0x0, 0x0); // mipi_dsi_dcs_write_buffer
	lcm_dcs_write_seq_static(ctx, 0x53, 0x2c); // mipi_dsi_dcs_write_buffer
	lcm_dcs_write_seq_static(ctx, 0x11); // mipi_dsi_dcs_write_buffer

    msleep(120); /* 0x78 */
    lcm_dcs_write_seq_static(ctx, 0x29); // mipi_dsi_dcs_write_buffer
    msleep(10);
}

static int lcm_disable(struct drm_panel *panel)
{
	// skip this for now lol
	// return 0;

	struct lcm *ctx = panel_to_lcm(panel);

	if (!ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = false;

	return 0;
}

int lcm_power_enable(void);
int lcm_power_disable(void);

static int lcm_unprepare(struct drm_panel *panel)
{
	// this breaks the LCD for now
	return 0;

	struct lcm *ctx = panel_to_lcm(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
    struct device *dev = &dsi->dev;

	if (!ctx->prepared)
		return 0;

	// todo: look closer at this in ghidra
	lcm_dcs_write_seq_static(ctx, 0xf0, 0x5a, 0x59); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xf1, 0xa5, 0xa6); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xbb, 0x1, 0x5, 0x9, 0x11, 0xd, 0x19, 0x1d, 0x15, 0x25, 0x69, 0x0, 0x21, 0x25); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xf0, 0xa5, 0xa6); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0xf1, 0x5a, 0x59); // mipi_dsi_generic_write
	lcm_dcs_write_seq_static(ctx, 0x26, 0x8); // mipi_dsi_dcs_write_buffer
	lcm_dcs_write_seq_static(ctx, 0x26, 0x8); // mipi_dsi_dcs_write_buffer

	lcm_dcs_write_seq_static(ctx, 0x28);
	msleep(20);
	lcm_dcs_write_seq_static(ctx, 0x10);
	msleep(100);

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	gpiod_set_value(ctx->reset_gpio, 1);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);

	ctx->error = 0;
	ctx->prepared = false;

    lcm_power_disable();

	return 0;
}

int lcm_prepare(struct drm_panel *panel)
{
	// assume the LK initialized the display for now
	return 0;

    struct lcm *ctx = panel_to_lcm(panel);
    struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
    struct device *dev = &dsi->dev;
    
    u8 dcs_f6_val;
    u8 write_buf[2];
    ssize_t ret;

    pr_info("[%d_%s] hxl_check_lcd_resum_ente\n", 0x123, __func__);

    if (ctx->prepared)
        return 0;

	lcm_panel_init(ctx);
    lcm_power_enable();

	ret = ctx->error;

	if (ret < 0)
		lcm_unprepare(panel);

	ctx->prepared = true;

	ctx->cabc_mode = 0; //UI mode
    mtk_panel_tch_rst(panel);

    return ret;
}

static int lcm_enable(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = true;

	return 0;
}

#define HFP (48)
#define HSA (4)
#define HBP (48)
// 1000 seems to work fine?
#define VFP_60HZ (150)
#define VSA (4)
#define VBP (32)
#define VAC (1640)
#define HAC (720)

static struct drm_display_mode default_mode = {
	.clock = 142467,
	.hdisplay = HAC,
	.hsync_start = 768, // HAC + HFP
	.hsync_end = 772, // HAC + HFP + HSA
	.htotal = 820, // HAC + HFP + HSA + HBP
	.vdisplay = VAC,
	.vsync_start = 1790, // VAC + VFP_60HZ
	.vsync_end = 1794, // VAC + VFP_60HZ + VSA
	.vtotal = 1826, // VAC + VFP_60HZ + VSA + VBP
	.vrefresh = 60,
};

#if defined(CONFIG_MTK_PANEL_EXT)
static int panel_ext_reset(struct drm_panel *panel, int on)
{
	struct lcm *ctx = panel_to_lcm(panel);

	ctx->reset_gpio =
		devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(ctx->dev, "%s: cannot get reset_gpio %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	gpiod_set_value(ctx->reset_gpio, on);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);

	return 0;
}

static int panel_ata_check(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	unsigned char data[3] = {0x00, 0x00, 0x00};
	unsigned char id[3] = {0x00, 0x00, 0x00};
	ssize_t ret;

	ret = mipi_dsi_dcs_read(dsi, 0x4, data, 3);
	if (ret < 0) {
		pr_err("%s error\n", __func__);
		return 0;
	}

	DDPINFO("ATA read data %x %x %x\n", data[0], data[1], data[2]);

	if (data[0] == id[0] &&
			data[1] == id[1] &&
			data[2] == id[2])
		return 1;

	DDPINFO("ATA expect read data is %x %x %x\n",
			id[0], id[1], id[2]);

	return 0;
}

static int lcm_setbacklight_cmdq(void *dsi, dcs_write_gce cb,
	void *handle, unsigned int level)
{
	char bl_tb0[] = {0x51, 0xFF, 0x0E};
	// char dimming_tb[] = {0x53, 0x24};

	pr_debug("icnl9911c level : %d\n", level);

	if(level > 0xfe) level = 0xff;

	bl_tb0[1] = ((level >> 3) & 0xFF);
	bl_tb0[2] = ((level << 1) & 0x0E);

	if (!cb)
		return -1;

	//if (!level)
	//	cb(dsi, handle, dimming_tb, ARRAY_SIZE(dimming_tb));

	cb(dsi, handle, bl_tb0, ARRAY_SIZE(bl_tb0));

	return 0;
}

static int lcm_get_virtual_heigh(void)
{
	return VAC;
}

static int lcm_get_virtual_width(void)
{
	return HAC;
}

static struct mtk_panel_params ext_params = {
	.pll_clk = 285,
	.vfp_low_power = VFP_60HZ,
	.cust_esd_check = 1,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
	},
	.dyn_fps = {
		.switch_en = 1,
		.vact_timing_fps = 60,
	},
	.lcm_index = 2,
};

static int mtk_panel_ext_param_set(struct drm_panel *panel, unsigned int mode)
{
	struct mtk_panel_ext *ext = find_panel_ext(panel);
	int ret = 0;

	if (mode == 0)
		ext->params = &ext_params;
	else
		ret = 1;

	return ret;
}

static int mtk_panel_ext_param_get(struct mtk_panel_params *ext_para,
			 unsigned int mode)
{
	int ret = 0;

	if (mode == 0)
		ext_para = &ext_params;
	else
		ret = 1;

	return ret;

}

// seems to be only declared in motos code?

//static int panel_cabc_set_cmdq(struct drm_panel *panel, void *dsi,
//			      dcs_write_gce cb, void *handle, unsigned int cabc_mode)
//{
//	const unsigned int cabc_value_map[3] = {1, 3, 0};
//	int cabc_value = 1;
//	char cabc_tb[2] = {0x55, 0x01};
//	u8 cabc_tb1[] = {0xF0, 0x5A, 0x59};//Password open
//	u8 cabc_tb2[] = {0xF1, 0xA5, 0xA6};//Password open
//	u8 cabc_ui_tb3[] = {0xE0, 0x30, 0x00, 0x80, 0x88, 0x11, 0x3F, 0x22, 0x62, 0xDF, 0xA0, 0x04, 0xCC, 0x01, 0xFF, 0xF6, 0xFF, 0xF0, 0xFD, 0xFF, 0xFD, 0xF8, 0xF5, 0xFC, 0xFC, 0xFD, 0xFF};
//	u8 cabc_ui_tb4[] = {0xE1, 0xEF, 0xFE, 0xFE, 0xFE, 0xFE, 0xEE, 0xF0, 0x20, 0x33, 0xFF, 0x00, 0x00, 0x6A, 0x90, 0xC0, 0x0D, 0x6A, 0xF0, 0x3E, 0xFF, 0x00, 0x07, 0xD0};
//	u8 cabc_movie_tb3[] = {0xE0, 0x30, 0x00, 0x80, 0x88, 0x11, 0x3F, 0x22, 0x62, 0xDF, 0xA0, 0x04, 0xCC, 0x01, 0xFF, 0xFA, 0xFF, 0xF0, 0xFD, 0xFF, 0xFB, 0xF8, 0xF5, 0xFC, 0xFC, 0xFB, 0xFF};
//	u8 cabc_movie_tb4[] = {0xE1, 0xBC, 0xF8, 0xCC, 0xFA, 0xDB, 0x9B, 0xF0, 0xE7, 0xF0, 0x85, 0xF0, 0x70, 0x00, 0x50, 0x00, 0x9A, 0xFD, 0xF0, 0xE0, 0xFF, 0x00, 0x07, 0xD0};
//	u8 cabc_tb5[] = {0xF1, 0x5A, 0x59};//Password off
//	u8 cabc_tb6[] = {0xF0, 0xA5, 0xA6};//Password off
//
//	struct lcm *ctx = panel_to_lcm(panel);
//
//	if (ctx->cabc_mode == cabc_mode)
//		goto done;
//
//	if (!cb)
//		return -1;
//
//	if (cabc_mode > 2) return -1;
//
//	cabc_value = cabc_value_map[cabc_mode];
//	cabc_tb[1] = cabc_value;
//
//	cb(dsi, handle, cabc_tb, ARRAY_SIZE(cabc_tb));
//	cb(dsi, handle, cabc_tb1, ARRAY_SIZE(cabc_tb1));
//	cb(dsi, handle, cabc_tb2, ARRAY_SIZE(cabc_tb2));
//
//	if (cabc_value == 3) {
//		cb(dsi, handle, cabc_movie_tb3, ARRAY_SIZE(cabc_movie_tb3));
//		cb(dsi, handle, cabc_movie_tb4, ARRAY_SIZE(cabc_movie_tb4));
//	}else {
//		cb(dsi, handle, cabc_ui_tb3, ARRAY_SIZE(cabc_ui_tb3));
//		cb(dsi, handle, cabc_ui_tb4, ARRAY_SIZE(cabc_ui_tb4));
//	}
//
//	cb(dsi, handle, cabc_tb1, ARRAY_SIZE(cabc_tb5));
//	cb(dsi, handle, cabc_tb2, ARRAY_SIZE(cabc_tb6));
//	pr_info(" set cabc to %d\n", cabc_value);
//
//done:
//	ctx->cabc_mode = cabc_mode;
//	return 0;
//}
//
//static void panel_cabc_get_state(struct drm_panel *panel, unsigned int *cabc_mode)
//{
//	struct lcm *ctx = panel_to_lcm(panel);
//
//	*cabc_mode = ctx->cabc_mode;
//}

static struct mtk_panel_funcs ext_funcs = {
	.reset = panel_ext_reset,
	.set_backlight_cmdq = lcm_setbacklight_cmdq,
	.ext_param_set = mtk_panel_ext_param_set,
	.ext_param_get = mtk_panel_ext_param_get,
	// .ata_check = panel_ata_check,
	.get_virtual_heigh = lcm_get_virtual_heigh,
	.get_virtual_width = lcm_get_virtual_width,
	//.cabc_set_cmdq = panel_cabc_set_cmdq,
	//.cabc_get_state = panel_cabc_get_state,
};
#endif

struct panel_desc {
	const struct drm_display_mode *modes;
	unsigned int num_modes;

	unsigned int bpc;

	struct {
		unsigned int width;
		unsigned int height;
	} size;

	struct {
		unsigned int prepare;
		unsigned int enable;
		unsigned int disable;
		unsigned int unprepare;
	} delay;
};

static int lcm_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		dev_err(panel->drm->dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(panel->connector, mode);

	// busted out ye old ruler for this
	panel->connector->display_info.width_mm = 69;
	panel->connector->display_info.height_mm = 159;

	return 1;
}

static const struct drm_panel_funcs lcm_drm_funcs = {
	.disable = lcm_disable,
	.unprepare = lcm_unprepare,
	.prepare = lcm_prepare,
	.enable = lcm_enable,
	.get_modes = lcm_get_modes,
};

static int lcm_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct lcm *ctx;
	struct device_node *backlight;
	int ret;
	struct device_node *dsi_node, *remote_node = NULL, *endpoint = NULL;

	if (strstr(saved_command_line, "mipi_vid_dongshan_icnl9911c_720p_653") ||
                        !strstr(saved_command_line, "lcd_name=")) {
		pr_err("%s dongshan icnl9911c\n", __func__);
	} else {
		pr_err("not match dongshan icnl9911c !!!\n");
		return -ENODEV;
	}
	dsi_node = of_get_parent(dev->of_node);
	if (dsi_node) {
		endpoint = of_graph_get_next_endpoint(dsi_node, NULL);
		if (endpoint) {
			remote_node = of_graph_get_remote_port_parent(endpoint);
			if (!remote_node) {
				pr_info("No panel connected,skip probe lcm\n");
				return -ENODEV;
			}
			pr_info("device node name:%s\n", remote_node->name);
		}
	}
	if (remote_node != dev->of_node) {
		pr_info("%s+ skip probe due to not current lcm\n", __func__);
		return -ENODEV;
	}

	ctx = devm_kzalloc(dev, sizeof(struct lcm), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	// *(undefined8 *)(param_1 + 0x3e0) = 0xe05; todo smth
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO
			 | MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET
			 | MIPI_DSI_CLOCK_NON_CONTINUOUS;

	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		ctx->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!ctx->backlight)
			return -EPROBE_DEFER;
	}

	lcm_power_enable();
	//ctx->bl_iset_en_gpio = devm_gpiod_get(dev, "bl-iset-en", GPIOD_IN);
	//if (IS_ERR(ctx->bl_iset_en_gpio)) {
	//	dev_err(dev, "%s: cannot get bl_iset_en_gpio %ld\n",
	//		__func__, PTR_ERR(ctx->bl_iset_en_gpio));
	//	return PTR_ERR(ctx->bl_iset_en_gpio);
	//}
	//devm_gpiod_put(dev, ctx->bl_iset_en_gpio);

	ctx->bl_iset_en_gpio = 0;

	ctx->prepared = true;
	ctx->enabled = true;

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &lcm_drm_funcs;

	ret = drm_panel_add(&ctx->panel);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		drm_panel_remove(&ctx->panel);

#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_handle_reg(&ctx->panel);
	ret = mtk_panel_ext_create(dev, &ext_params, &ext_funcs, &ctx->panel);
	if (ret < 0)
		return ret;
#endif

	pr_info("%s-\n", __func__);

	return ret;
}

static int lcm_remove(struct mipi_dsi_device *dsi)
{
	struct lcm *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id lcm_of_match[] = {
	{ .compatible = "icnl9911c,dongshan,vdo", },
	{ }
};

MODULE_DEVICE_TABLE(of, lcm_of_match);

static struct mipi_dsi_driver lcm_driver = {
	.probe = lcm_probe,
	.remove = lcm_remove,
	.driver = {
		.name = "panel-icnl9911c-dongshan-vdo",
		.owner = THIS_MODULE,
		.of_match_table = lcm_of_match,
	},
};

module_mipi_dsi_driver(lcm_driver);

MODULE_AUTHOR("Yi-Lun Wang <Yi-Lun.Wang@mediatek.com>");
MODULE_DESCRIPTION("icnl9911c dongshan VDO LCD Panel Driver");
MODULE_LICENSE("GPL v2");
