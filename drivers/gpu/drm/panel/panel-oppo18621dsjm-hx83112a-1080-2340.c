// SPDX-License-Identifier: GPL-2.0-only
/* 
 * Copyright (c) 2025 Yahia Shamaa
 * Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree:
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct hx83112a_1080_2340 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;
};

static inline
struct hx83112a_1080_2340 *to_hx83112a_1080_2340(struct drm_panel *panel)
{
	return container_of(panel, struct hx83112a_1080_2340, panel);
}

static void hx83112a_1080_2340_reset(struct hx83112a_1080_2340 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(60);
}

static int hx83112a_1080_2340_on(struct hx83112a_1080_2340 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb9, 0x83, 0x11, 0x2a);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x11, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 60);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb9, 0x83, 0x11, 0x2a);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x02);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe7,
					 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
					 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
					 0x00, 0x00, 0x00, 0x00, 0x0e, 0x00,
					 0x00, 0x00, 0x00, 0x22, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc4,
					 0x00, 0xa2, 0xa5, 0x97, 0x00, 0x02);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xbd, 0x00, 0x00, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb9, 0x83, 0x11, 0x2a);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe9, 0xc3);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xcb, 0xc1, 0xba);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe9, 0xc3);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xcb, 0xd1, 0xba);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe9, 0xc9);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb2, 0xe5);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe7,
					 0x0e, 0x0e, 0x1e, 0x61, 0x1e, 0x60);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe9, 0xcc);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xb4, 0xbd);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe9, 0x3f);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x35, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xc9,
					 0x04, 0x08, 0xa0, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x55, 0x00);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0xe4,
					 0x2d, 0x01, 0x2c, 0x00, 0x08, 0x00,
					 0x10, 0x08, 0x04, 0x04, 0x3f, 0x56,
					 0x6d, 0x84, 0x9b, 0xb2, 0xc9, 0xe0,
					 0xf2, 0xff, 0xff, 0xef);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x51, 0x0f, 0xff);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x53, 0x24);
	mipi_dsi_generic_write_seq_multi(&dsi_ctx, 0x29, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 30);

	return dsi_ctx.accum_err;
}

static int hx83112a_1080_2340_off(struct hx83112a_1080_2340 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static int hx83112a_1080_2340_prepare(struct drm_panel *panel)
{
	struct hx83112a_1080_2340 *ctx = to_hx83112a_1080_2340(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	hx83112a_1080_2340_reset(ctx);

	ret = hx83112a_1080_2340_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		return ret;
	}

	return 0;
}

static int hx83112a_1080_2340_unprepare(struct drm_panel *panel)
{
	struct hx83112a_1080_2340 *ctx = to_hx83112a_1080_2340(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = hx83112a_1080_2340_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	return 0;
}

static const struct drm_display_mode hx83112a_1080_2340_mode = {
	.clock = (1080 + 160 + 4 + 12) * (2340 + 86 + 2 + 15) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 160,
	.hsync_end = 1080 + 160 + 4,
	.htotal = 1080 + 160 + 4 + 12,
	.vdisplay = 2340,
	.vsync_start = 2340 + 86,
	.vsync_end = 2340 + 86 + 2,
	.vtotal = 2340 + 86 + 2 + 15,
	.width_mm = 67,
	.height_mm = 145,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int hx83112a_1080_2340_get_modes(struct drm_panel *panel,
						      struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &hx83112a_1080_2340_mode);
}

static const struct drm_panel_funcs hx83112a_1080_2340_panel_funcs = {
	.prepare = hx83112a_1080_2340_prepare,
	.unprepare = hx83112a_1080_2340_unprepare,
	.get_modes = hx83112a_1080_2340_get_modes,
};

static int hx83112a_1080_2340_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct hx83112a_1080_2340 *ctx;
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
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_NO_EOT_PACKET |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	drm_panel_init(&ctx->panel, dev, &hx83112a_1080_2340_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void hx83112a_1080_2340_remove(struct mipi_dsi_device *dsi)
{
	struct hx83112a_1080_2340 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id hx83112a_1080_2340_of_match[] = {
	{ .compatible = "oppodsjm,hx83112a" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, hx83112a_1080_2340_of_match);

static struct mipi_dsi_driver hx83112a_1080_2340_driver = {
	.probe = hx83112a_1080_2340_probe,
	.remove = hx83112a_1080_2340_remove,
	.driver = {
		.name = "panel-oppo18621dsjm-hx83112a-1080-2340",
		.of_match_table = hx83112a_1080_2340_of_match,
	},
};
module_mipi_dsi_driver(hx83112a_1080_2340_driver);

MODULE_AUTHOR("Yahia Shamaa <yehiashamaa987@gmail.com>");
MODULE_DESCRIPTION("DRM driver for himax hx83112a panel");
MODULE_LICENSE("GPL");
