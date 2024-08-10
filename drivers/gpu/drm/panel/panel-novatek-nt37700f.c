// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024, The Linux Foundation. All rights reserved.
 * Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree:
 *   Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct nt37700f_tianma {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
};

static inline
struct nt37700f_tianma *to_nt37700f_tianma(struct drm_panel *panel)
{
	return container_of(panel, struct nt37700f_tianma, panel);
}

static void nt37700f_tianma_reset(struct nt37700f_tianma *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int nt37700f_tianma_on(struct nt37700f_tianma *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xc0, 0x56);
	mipi_dsi_dcs_write_seq(dsi, 0xca, 0x52);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x06);
	mipi_dsi_dcs_write_seq(dsi, 0xb5, 0x2b, 0x1a);
	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x01);
	mipi_dsi_dcs_write_seq(dsi, 0xcd, 0x04, 0x82);
	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x02);
	mipi_dsi_dcs_write_seq(dsi, 0xcc, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x55);
	mipi_dsi_dcs_write_seq(dsi, 0xf6, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x56);
	mipi_dsi_dcs_write_seq(dsi, 0xf6, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x07);
	mipi_dsi_dcs_write_seq(dsi, 0xf3, 0x07);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x05);
	mipi_dsi_dcs_write_seq(dsi, 0xf3, 0x25);
	mipi_dsi_dcs_write_seq(dsi, 0x90, 0x01);

	ret = mipi_dsi_dcs_set_column_address(dsi, 0x0000, 0x0437);
	if (ret < 0) {
		dev_err(dev, "Failed to set column address: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_dcs_set_page_address(dsi, 0x0000, 0x086f);
	if (ret < 0) {
		dev_err(dev, "Failed to set page address: %d\n", ret);
		return ret;
	}

	mipi_dsi_dcs_write_seq(dsi, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);

	ret = mipi_dsi_dcs_set_tear_on(dsi, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	if (ret < 0) {
		dev_err(dev, "Failed to set tear on: %d\n", ret);
		return ret;
	}

	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xc0, 0x56);
	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x02);
	mipi_dsi_dcs_write_seq(dsi, 0xcd, 0x00);
	mipi_dsi_dcs_write_seq(dsi, 0xf0, 0x55, 0xaa, 0x52, 0x08, 0x04);
	mipi_dsi_dcs_write_seq(dsi, 0xd0, 0x11, 0x64);
	mipi_dsi_dcs_write_seq(dsi, 0x6f, 0x09);
	mipi_dsi_dcs_write_seq(dsi, 0xb1, 0x20);

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}
	msleep(120);

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display on: %d\n", ret);
		return ret;
	}

	return 0;
}

static int nt37700f_tianma_disable(struct drm_panel *panel)
{
	struct nt37700f_tianma *ctx = to_nt37700f_tianma(panel);
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct device *dev = &dsi->dev;
	int ret;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to set display off: %d\n", ret);
		return ret;
	}
	msleep(50);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(dev, "Failed to enter sleep mode: %d\n", ret);
		return ret;
	}
	msleep(100);

	return 0;
}

static int nt37700f_tianma_prepare(struct drm_panel *panel)
{
	struct nt37700f_tianma *ctx = to_nt37700f_tianma(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	nt37700f_tianma_reset(ctx);

	ret = nt37700f_tianma_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		return ret;
	}

	return 0;
}

static int nt37700f_tianma_unprepare(struct drm_panel *panel)
{
	struct nt37700f_tianma *ctx = to_nt37700f_tianma(panel);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	return 0;
}

static const struct drm_display_mode nt37700f_tianma_mode = {
	.clock = (1080 + 32 + 32 + 98) * (2160 + 32 + 4 + 98) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 32,
	.hsync_end = 1080 + 32 + 32,
	.htotal = 1080 + 32 + 32 + 98,
	.vdisplay = 2160,
	.vsync_start = 2160 + 32,
	.vsync_end = 2160 + 32 + 4,
	.vtotal = 2160 + 32 + 4 + 98,
	.width_mm = 69,
	.height_mm = 137,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int nt37700f_tianma_get_modes(struct drm_panel *panel,
				     struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &nt37700f_tianma_mode);
}

static const struct drm_panel_funcs nt37700f_tianma_panel_funcs = {
	.prepare = nt37700f_tianma_prepare,
	.unprepare = nt37700f_tianma_unprepare,
	.disable = nt37700f_tianma_disable,
	.get_modes = nt37700f_tianma_get_modes,
};

static int nt37700f_tianma_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

// TODO: Check if /sys/class/backlight/.../actual_brightness actually returns
// correct values. If not, remove this function.
static int nt37700f_tianma_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness_large(dsi, &brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return brightness;
}

static const struct backlight_ops nt37700f_tianma_bl_ops = {
	.update_status = nt37700f_tianma_bl_update_status,
	.get_brightness = nt37700f_tianma_bl_get_brightness,
};

static struct backlight_device *
nt37700f_tianma_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 2047,
		.max_brightness = 2047,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &nt37700f_tianma_bl_ops, &props);
}

static int nt37700f_tianma_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct nt37700f_tianma *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	drm_panel_init(&ctx->panel, dev, &nt37700f_tianma_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = nt37700f_tianma_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void nt37700f_tianma_remove(struct mipi_dsi_device *dsi)
{
	struct nt37700f_tianma *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id nt37700f_tianma_of_match[] = {
	{ .compatible = "novatek,nt37700f" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, nt37700f_tianma_of_match);

static struct mipi_dsi_driver nt37700f_tianma_driver = {
	.probe = nt37700f_tianma_probe,
	.remove = nt37700f_tianma_remove,
	.driver = {
		.name = "panel-novatek-nt37700f",
		.of_match_table = nt37700f_tianma_of_match,
	},
};
module_mipi_dsi_driver(nt37700f_tianma_driver);

MODULE_DESCRIPTION("DRM driver for nt37700f cmd mode dsi tianma panel");
MODULE_LICENSE("GPL");
