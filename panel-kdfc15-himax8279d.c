// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019, Huaqin Telecom Technology Co., Ltd
 *
 * Author: Jerry Han <jerry.han.hq@gmail.com>
 * Author: qing yu tian <1741706321@qq.com>
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>

#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/backlight.h>  /* 2026.8.6 tianqingyu */

#include <drm/drm_device.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>

#define KDXS_PANEL_DEBUG 1
#define KDXS_SCOPE_INIT_WINDOW 0
#define KDXS_SCOPE_PRE_INIT_DELAY_MS 500
#define KDXS_SCOPE_POST_INIT_DELAY_MS 1000
#define KDXS_INIT_SEND_DCS_WRITE 1
#define KDXS_INIT_SEND_DCS_BUFFER 2
#define KDXS_INIT_SEND_GENERIC 3
#define KDXS_INIT_SEND_MODE KDXS_INIT_SEND_DCS_BUFFER
#define KDXS_INIT_INTER_CMD_DELAY 0
#define KDXS_INIT_INTER_CMD_DELAY_MS 40
#define KDXS_DCS_READBACK_DEBUG 0

#if KDXS_PANEL_DEBUG
#define kdxs_dbg(dev, fmt, ...) \
	dev_info(dev, "[kdxs] " fmt, ##__VA_ARGS__)
#else
#define kdxs_dbg(dev, fmt, ...) \
	do { \
	} while (0)
#endif

#if KDXS_SCOPE_INIT_WINDOW
#define kdxs_scope_pause(dev, stage, delay_ms) \
	do { \
		kdxs_dbg(dev, "scope window %s: delay %u ms\n", \
			 stage, (unsigned int)(delay_ms)); \
		msleep(delay_ms); \
	} while (0)
#else
#define kdxs_scope_pause(dev, stage, delay_ms) \
	do { \
	} while (0)
#endif

struct panel_cmd {
	u8 cmd;
	u8 data;
};

struct panel_desc {
	const struct drm_display_mode *display_mode;
	unsigned int bpc;
	unsigned int width_mm;
	unsigned int height_mm;

	unsigned long mode_flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;
	const struct panel_cmd *on_cmds;
	unsigned int on_cmds_num;
};

struct panel_info {
	struct drm_panel base;
	struct mipi_dsi_device *link;
	const struct panel_desc *desc;
	struct regulator *power;
	struct gpio_desc *reset_gpio;
};

static inline struct panel_info *to_panel_info(struct drm_panel *panel)
{
	return container_of(panel, struct panel_info, base);
}

static void disable_gpios(struct panel_info *pinfo)
{
	gpiod_set_value(pinfo->reset_gpio, 1);
}

static void kdxs_dcs_readback(struct drm_panel *panel)
{
#if KDXS_DCS_READBACK_DEBUG
	struct panel_info *pinfo = to_panel_info(panel);
	static const u8 read_cmds[] = { 0x0A, 0x0D, 0x0C };
	static const char * const read_names[] = {
		"power mode",
		"display mode",
		"pixel format",
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(read_cmds); i++) {
		u8 value = 0;
		int err;

		err = mipi_dsi_dcs_read(pinfo->link, read_cmds[i], &value, 1);
		if (err < 0) {
			dev_err(panel->dev,
				"[kdxs] dcs readback failed: cmd=0x%02x (%s) err=%d\n",
				read_cmds[i], read_names[i], err);
			continue;
		}

		kdxs_dbg(panel->dev,
			 "dcs readback: cmd=0x%02x (%s) val=0x%02x len=%d\n",
			 read_cmds[i], read_names[i], value, err);
	}
#endif
}

static int send_mipi_cmds(struct drm_panel *panel, const struct panel_cmd *cmds)
{
	struct panel_info *pinfo = to_panel_info(panel);
	unsigned int i = 0;
	int err;
	u8 payload[2];

	kdxs_dbg(panel->dev, "send init cmds start, count=%u\n",
		 pinfo->desc->on_cmds_num);

	for (i = 0; i < pinfo->desc->on_cmds_num; i++) {
		kdxs_dbg(panel->dev, "init cmd[%u]: cmd=0x%02x data=0x%02x\n",
			 i, cmds[i].cmd, cmds[i].data);

		payload[0] = cmds[i].cmd;
		payload[1] = cmds[i].data;

#if KDXS_INIT_SEND_MODE == KDXS_INIT_SEND_DCS_WRITE
		err = mipi_dsi_dcs_write(pinfo->link, payload[0], &payload[1], 1);
#elif KDXS_INIT_SEND_MODE == KDXS_INIT_SEND_DCS_BUFFER
		err = mipi_dsi_dcs_write_buffer(pinfo->link, payload,
						sizeof(payload));
#elif KDXS_INIT_SEND_MODE == KDXS_INIT_SEND_GENERIC
		err = mipi_dsi_generic_write(pinfo->link, payload,
					     sizeof(payload));
#else
#error "Unsupported KDXS_INIT_SEND_MODE"
#endif

		if (err < 0) {
			dev_err(panel->dev,
				"[kdxs] init cmd[%u] failed: cmd=0x%02x data=0x%02x err=%d\n",
				i, cmds[i].cmd, cmds[i].data, err);
			return err;
		}

#if KDXS_INIT_INTER_CMD_DELAY
		kdxs_dbg(panel->dev,
			 "init cmd[%u] delay %u ms\n",
			 i, (unsigned int)KDXS_INIT_INTER_CMD_DELAY_MS);
		msleep(KDXS_INIT_INTER_CMD_DELAY_MS);
#endif
	}

	kdxs_dbg(panel->dev, "send init cmds done\n");

	return 0;
}

static int kdxs_panel_disable(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int err;

	/* 2026.8.6 tianqingyu: close backlight before display off */
	if (pinfo->base.backlight)
		backlight_disable(pinfo->base.backlight);

	err = mipi_dsi_dcs_set_display_off(pinfo->link);
	if (err < 0) {
		dev_err(panel->dev, "failed to set display off: %d\n", err);
		return err;
	}

	return 0;
}

static int kdxs_panel_unprepare(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int err;

	err = mipi_dsi_dcs_enter_sleep_mode(pinfo->link);
	if (err < 0)
		dev_err(panel->dev, "failed to enter sleep mode: %d\n", err);

	/* sleep_mode_delay: 120ms - 121ms */
	usleep_range(120000, 121000);

	disable_gpios(pinfo);
	regulator_disable(pinfo->power);

	return 0;
}

static int kdxs_panel_prepare(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int err;

	kdxs_dbg(panel->dev, "prepare enter\n");

	err = regulator_enable(pinfo->power);
	if (err < 0) {
		dev_err(panel->dev, "failed to enable power: %d\n", err);
		return err;
	}
	kdxs_dbg(panel->dev, "power enabled\n");

	/* reset pulse: high -> low -> high */
	gpiod_set_value(pinfo->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value(pinfo->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value(pinfo->reset_gpio, 0);
	usleep_range(120000, 121000);

	err = send_mipi_cmds(&pinfo->base, pinfo->desc->on_cmds);
	if (err < 0) {
		dev_err(panel->dev, "failed to send DCS Init Code: %d\n", err);
		goto poweroff;
	}

	err = mipi_dsi_dcs_exit_sleep_mode(pinfo->link);
	if (err < 0) {
		dev_err(panel->dev, "failed to exit sleep mode: %d\n", err);
		goto poweroff;
	}

	kdxs_dbg(panel->dev, "exit sleep mode done\n");

	/* T6: sleep out delay 120ms - 121ms */
	usleep_range(120000, 121000);

	kdxs_dcs_readback(&pinfo->base);

	return 0;

poweroff:
	disable_gpios(pinfo);
	regulator_disable(pinfo->power);
	return err;
}

static int kdxs_panel_enable(struct drm_panel *panel)
{
	struct panel_info *pinfo = to_panel_info(panel);
	int err;

	err = mipi_dsi_dcs_set_display_on(pinfo->link);
	if (err < 0) {
		dev_err(panel->dev, "failed to set display on: %d\n", err);
		return err;

	}

	/* T7: display on delay 20ms - 21ms */
	usleep_range(120000, 121000);

	/* 2026.8.6 tianqingyu: enable backlight after display on */
	if (pinfo->base.backlight)
		backlight_enable(pinfo->base.backlight);
	kdxs_dbg(panel->dev, "enable enter, backlight=%p\n", pinfo->base.backlight);


	kdxs_dbg(panel->dev, "enable done\n");
	return 0;
}

static int kdxs_panel_get_modes(struct drm_panel *panel,
			       struct drm_connector *connector)
{
	struct panel_info *pinfo = to_panel_info(panel);
	const struct drm_display_mode *m = pinfo->desc->display_mode;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, m);
	if (!mode) {
		dev_err(pinfo->base.dev, "failed to add mode %ux%u@%u\n",
			m->hdisplay, m->vdisplay, drm_mode_vrefresh(m));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = pinfo->desc->width_mm;
	connector->display_info.height_mm = pinfo->desc->height_mm;
	connector->display_info.bpc = pinfo->desc->bpc;

	return 1;
}

static const struct drm_panel_funcs panel_funcs = {
	.disable = kdxs_panel_disable,
	.unprepare = kdxs_panel_unprepare,
	.prepare = kdxs_panel_prepare,
	.enable = kdxs_panel_enable,
	.get_modes = kdxs_panel_get_modes,
};

static const struct drm_display_mode default_display_mode = {
	.clock = 156297,
	.hdisplay = 1200,
	.hsync_start = 1200 + 60,
	.hsync_end = 1200 + 60 + 24,
	.htotal = 1200 + 60 + 24 + 56,
	.vdisplay = 1920,
	.vsync_start = 1920 + 14,
	.vsync_end = 1920 + 14 + 2,
	.vtotal = 1920 + 14 + 2 + 8,
};

/* 10 inch */
static const struct panel_cmd kdxs_himax8279_on_cmds[] = {
	/* page 1 */
	{ 0xB0, 0x01 },
	{ 0xC3, 0x0F },
	{ 0xC4, 0x00 },
	{ 0xC5, 0x00 },
	{ 0xC6, 0x00 },
	{ 0xC7, 0x00 },
	{ 0xC8, 0x0D },
	{ 0xC9, 0x12 },
	{ 0xCA, 0x11 },
	{ 0xCD, 0x1D },
	{ 0xCE, 0x1B },
	{ 0xCF, 0x0B },
	{ 0xD0, 0x09 },
	{ 0xD1, 0x07 },
	{ 0xD2, 0x05 },
	{ 0xD3, 0x01 },
	{ 0xD7, 0x10 },
	{ 0xD8, 0x00 },
	{ 0xD9, 0x00 },
	{ 0xDA, 0x00 },
	{ 0xDB, 0x00 },
	{ 0xDC, 0x0E },
	{ 0xDD, 0x12 },
	{ 0xDE, 0x11 },
	{ 0xE1, 0x1E },
	{ 0xE2, 0x1C },
	{ 0xE3, 0x0C },
	{ 0xE4, 0x0A },
	{ 0xE5, 0x08 },
	{ 0xE6, 0x06 },
	{ 0xE7, 0x02 },
	/* page 3 */
	{ 0xB0, 0x03 },
	{ 0xBE, 0x03 },
	{ 0xCC, 0x44 },
	{ 0xC8, 0x07 },
	{ 0xC9, 0x05 },
	{ 0xCA, 0x42 },
	{ 0xCD, 0x3E },
	{ 0xCF, 0x60 },
	{ 0xD2, 0x04 },
	{ 0xD3, 0x04 },
	{ 0xD4, 0x01 },
	{ 0xD5, 0x00 },
	{ 0xD6, 0x03 },
	{ 0xD7, 0x04 },
	{ 0xD9, 0x01 },
	{ 0xDB, 0x01 },
	{ 0xE4, 0xF0 },
	{ 0xE5, 0x0A },
	/* page 0 */
	{ 0xB0, 0x00 },
	{ 0xBD, 0x63 },
	{ 0xC2, 0x08 },
	{ 0xC4, 0x10 },
	/* page 2 */
	{ 0xB0, 0x02 },
	{ 0xC0, 0x00 },
	{ 0xC1, 0x0C },
	{ 0xC2, 0x14 },
	{ 0xC3, 0x21 },
	{ 0xC4, 0x21 },
	{ 0xC5, 0x20 },
	{ 0xC6, 0x1F },
	{ 0xC7, 0x1E },
	{ 0xC8, 0x1A },
	{ 0xC9, 0x16 },
	{ 0xCA, 0x15 },
	{ 0xCB, 0x15 },
	{ 0xCC, 0x16 },
	{ 0xCD, 0x1B },
	{ 0xCE, 0x1C },
	{ 0xCF, 0x1E },
	{ 0xD0, 0x07 },
	{ 0xD1, 0x00 },
	{ 0xD2, 0x00 },
	{ 0xD3, 0x0C },
	{ 0xD4, 0x14 },
	{ 0xD5, 0x21 },
	{ 0xD6, 0x21 },
	{ 0xD7, 0x20 },
	{ 0xD8, 0x1F },
	{ 0xD9, 0x1E },
	{ 0xDA, 0x1A },
	{ 0xDB, 0x16 },
	{ 0xDC, 0x15 },
	{ 0xDD, 0x15 },
	{ 0xDE, 0x16 },
	{ 0xDF, 0x1B },
	{ 0xE0, 0x1C },
	{ 0xE1, 0x1E },
	{ 0xE2, 0x07 },
};

static const struct panel_desc kdxs_himax8279_panel_desc = {
	.display_mode = &default_display_mode,
	.bpc = 8,
	.width_mm = 135,
	.height_mm = 216,
	/* .mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE
	 *                | MIPI_DSI_MODE_NO_EOT_PACKET,
	 */
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
		      MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_NO_EOT_PACKET,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
	.on_cmds = kdxs_himax8279_on_cmds,
	.on_cmds_num = ARRAY_SIZE(kdxs_himax8279_on_cmds),
};

static const struct of_device_id panel_of_match[] = {
	{
		.compatible = "kdxs,himax8279",
		.data = &kdxs_himax8279_panel_desc,
	},
	{
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, panel_of_match);

static int panel_add(struct panel_info *pinfo)
{
	struct device *dev = &pinfo->link->dev;
	int ret;

	pinfo->power = devm_regulator_get(dev, "power");
	if (IS_ERR(pinfo->power)) {
		return dev_err_probe(dev, PTR_ERR(pinfo->power),
				     "failed to get power regulator\n");
	}
	kdxs_dbg(dev, "got power regulator\n");

	pinfo->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(pinfo->reset_gpio)) {
		return dev_err_probe(dev, PTR_ERR(pinfo->reset_gpio),
				     "failed to get reset gpio\n");
	}
	kdxs_dbg(dev, "got reset gpio\n");

	drm_panel_init(&pinfo->base, dev, &panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	ret = drm_panel_of_backlight(&pinfo->base);
	if (ret)
		return ret;

	drm_panel_add(&pinfo->base);

	return 0;
}

static int panel_probe(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo;
	const struct panel_desc *desc;
	int err;

	kdxs_dbg(&dsi->dev, "probe enter\n");

	pinfo = devm_kzalloc(&dsi->dev, sizeof(*pinfo), GFP_KERNEL);
	if (!pinfo)
		return -ENOMEM;

	desc = of_device_get_match_data(&dsi->dev);
	dsi->mode_flags = desc->mode_flags;
	dsi->format = desc->format;
	dsi->lanes = desc->lanes;
	pinfo->desc = desc;

	pinfo->link = dsi;
	mipi_dsi_set_drvdata(dsi, pinfo);

	pinfo->base.prepare_prev_first = true;

	err = panel_add(pinfo);
	if (err < 0)
		return err;

	usleep_range(20000, 21000);

	/* kdxs_scope_pause(&dsi->dev, "before-init", KDXS_SCOPE_PRE_INIT_DELAY_MS); */

	err = mipi_dsi_attach(dsi);
	if (err < 0)
		goto poweroff;
	else
		kdxs_dbg(&dsi->dev, "dsi attach done\n");

	return err;

poweroff:
	kdxs_dbg(&dsi->dev, "probe init failed, poweroff err=%d\n", err);
	disable_gpios(pinfo);
	regulator_disable(pinfo->power);
	drm_panel_remove(&pinfo->base);
	return err;
}

static void panel_remove(struct mipi_dsi_device *dsi)
{
	struct panel_info *pinfo = mipi_dsi_get_drvdata(dsi);
	int err;

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", err);

	drm_panel_remove(&pinfo->base);
}

static struct mipi_dsi_driver panel_driver = {
	.driver = {
		.name = "panel-kdxs-himax8279",
		.of_match_table = panel_of_match,
	},
	.probe = panel_probe,
	.remove = panel_remove,
};
module_mipi_dsi_driver(panel_driver);

MODULE_AUTHOR("Jerry Han <jerry.han.hq@gmail.com>");
MODULE_AUTHOR("qing yu tian <1741706321@qq.com>");
MODULE_DESCRIPTION("KDXS Himax8279 driver");
MODULE_LICENSE("GPL v2");
