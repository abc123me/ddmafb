// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simplest possible simple frame-buffer driver, as a platform device
 *
 * Copyright (c) 2013, Stephen Warren
 *
 * Based on q40fb.c, which was:
 * Copyright (C) 2001 Richard Zidlicky <rz@linux-m68k.org>
 *
 * Also based on offb.c, which was:
 * Copyright (C) 1997 Geert Uytterhoeven
 * Copyright (C) 1996 Paul Mackerras
 */

#include <linux/aperture.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_data/simplefb.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_clk.h>
#include <linux/of_dma.h>
#include <linux/of_platform.h>
#include <linux/parser.h>
#include <linux/pm_domain.h>
#include <linux/regulator/consumer.h>

static const struct fb_fix_screeninfo ddmafb_fix = {
	.id		= "simple",
	.type		= FB_TYPE_PACKED_PIXELS,
	.visual		= FB_VISUAL_TRUECOLOR,
	.accel		= FB_ACCEL_NONE,
};

static const struct fb_var_screeninfo ddmafb_var = {
	.height		= -1,
	.width		= -1,
	.activate	= FB_ACTIVATE_NOW,
	.vmode		= FB_VMODE_NONINTERLACED,
};

#define PSEUDO_PALETTE_SIZE 16

static int ddmafb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			      u_int transp, struct fb_info *info)
{
	u32 *pal = info->pseudo_palette;
	u32 cr = red >> (16 - info->var.red.length);
	u32 cg = green >> (16 - info->var.green.length);
	u32 cb = blue >> (16 - info->var.blue.length);
	u32 value;

	if (regno >= PSEUDO_PALETTE_SIZE)
		return -EINVAL;

	value = (cr << info->var.red.offset) |
		(cg << info->var.green.offset) |
		(cb << info->var.blue.offset);
	if (info->var.transp.length > 0) {
		u32 mask = (1 << info->var.transp.length) - 1;
		mask <<= info->var.transp.offset;
		value |= mask;
	}
	pal[regno] = value;

	return 0;
}

struct ddmafb_par {
	u32 palette[PSEUDO_PALETTE_SIZE];
	struct dma_chan *dma_channel;
	dma_addr_t dma_handle;
	void *dma_cpu_addr;
	size_t dma_size;
};

/*
 * fb_ops.fb_destroy is called by the last put_fb_info() call at the end
 * of unregister_framebuffer() or fb_release(). Do any cleanup here.
 */
static void ddmafb_destroy(struct fb_info *info)
{
	if (info->screen_base)
		iounmap(info->screen_base);

	framebuffer_release(info);
}

static const struct fb_ops ddmafb_ops = {
	.owner		= THIS_MODULE,
	FB_DEFAULT_IOMEM_OPS,
	.fb_destroy	= ddmafb_destroy,
	.fb_setcolreg	= ddmafb_setcolreg,
};

static struct simplefb_format ddmafb_formats[] = SIMPLEFB_FORMATS;

struct ddmafb_params {
	u32 width;
	u32 height;
	u32 stride;
	u32 framerate;
	struct simplefb_format *format;
	const char *chan_name;
};

static int ddmafb_parse_dt(struct platform_device *pdev,
			   struct ddmafb_params *params)
{
	struct device_node *np = pdev->dev.of_node;
	int ret;
	const char *str;
	int i;

	memset(params, 0, sizeof(struct ddmafb_params));

	ret = of_property_read_u32(np, "width", &params->width);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse width property\n");
		return ret;
	}

	ret = of_property_read_u32(np, "height", &params->height);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse height property\n");
		return ret;
	}

	ret = of_property_read_u32(np, "stride", &params->stride);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse stride property\n");
		return ret;
	}

	ret = of_property_read_u32(np, "framerate", &params->framerate);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse framerate property\n");
		return ret;
	}

	ret = of_property_read_string(np, "format", &str);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse format property\n");
		return ret;
	}
	for (i = 0; i < ARRAY_SIZE(ddmafb_formats); i++) {
		if (strcmp(str, ddmafb_formats[i].name))
			continue;
		params->format = &ddmafb_formats[i];
		break;
	}
	if (!params->format) {
		dev_err(&pdev->dev, "Invalid format value\n");
		return -EINVAL;
	}

	ret = of_property_read_string(np, "dma", &str);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse format property\n");
		return ret;
	}
	params->chan_name = str;

	return 0;
}

static int ddmafb_probe(struct platform_device *pdev)
{
	int ret;
	struct ddmafb_params params;
	struct fb_info *info;
	struct ddmafb_par *par;
	dma_addr_t dma_handle;
	struct dma_chan *dma_channel;
	size_t dma_size;
	void *dma_cpu_addr;

	if (fb_get_options("ddmafb", NULL))
		return -ENODEV;

	ret = -ENODEV;
	if (pdev->dev.of_node)
		ret = ddmafb_parse_dt(pdev, &params);

	if (ret)
		return ret;

	dma_channel = dma_request_chan(&pdev->dev, params.chan_name);
	if (IS_ERR(dma_channel)) {
		dev_err(&pdev->dev, "Failed to request DMA channel %s: %ld\n", params.chan_name, PTR_ERR(dma_channel));
		return PTR_ERR(dma_channel);
	}

	dma_size = params.width * params.stride;
	dma_cpu_addr = dma_alloc_coherent(&pdev->dev, dma_size, &dma_handle, GFP_KERNEL);
	if (!dma_cpu_addr) {
		dev_err(&pdev->dev, "Failed to allocate DMA buffer\n");
		ret = -ENOMEM;
		goto error_release_dma_channel;
	}

	info = framebuffer_alloc(sizeof(struct ddmafb_par), &pdev->dev);
	if (!info) {
		ret = -ENOMEM;
		goto error_release_dma_region;
	}
	platform_set_drvdata(pdev, info);

	par = info->par;

	info->fix = ddmafb_fix;
	info->fix.smem_start = (size_t) dma_cpu_addr;
	info->fix.smem_len = dma_size;
	info->fix.line_length = params.stride;

	info->var = ddmafb_var;
	info->var.xres = params.width;
	info->var.yres = params.height;
	info->var.xres_virtual = params.width;
	info->var.yres_virtual = params.height;
	info->var.bits_per_pixel = params.format->bits_per_pixel;
	info->var.red = params.format->red;
	info->var.green = params.format->green;
	info->var.blue = params.format->blue;
	info->var.transp = params.format->transp;

	par->dma_cpu_addr = dma_cpu_addr;
	par->dma_channel = dma_channel;
	par->dma_handle = dma_handle;
	par->dma_size = dma_size;

	info->fbops = &ddmafb_ops;
	info->screen_base = ioremap_wc(info->fix.smem_start, info->fix.smem_len);
	if (!info->screen_base) {
		ret = -ENOMEM;
		goto error_fb_release;
	}
	info->pseudo_palette = par->palette;

	dev_info(&pdev->dev, "framebuffer at 0x%lx, 0x%x bytes\n",
			     info->fix.smem_start, info->fix.smem_len);
	dev_info(&pdev->dev, "format=%s, mode=%dx%dx%d, linelength=%d\n",
			     params.format->name,
			     info->var.xres, info->var.yres,
			     info->var.bits_per_pixel, info->fix.line_length);

	ret = devm_aperture_acquire_for_platform_device(pdev, (resource_size_t) par->dma_cpu_addr, par->dma_size);
	if (ret) {
		dev_err(&pdev->dev, "Unable to acquire aperture: %d\n", ret);
		goto error_unmap;
	}
	ret = register_framebuffer(info);
	if (ret < 0) {
		dev_err(&pdev->dev, "Unable to register ddmafb: %d\n", ret);
		goto error_unmap;
	}

	dev_info(&pdev->dev, "fb%d: ddmafb registered!\n", info->node);

	return 0;

error_unmap:
	iounmap(info->screen_base);
error_fb_release:
	framebuffer_release(info);
error_release_dma_region:
	dma_free_coherent(&pdev->dev, dma_size, dma_cpu_addr, dma_handle);
error_release_dma_channel:
	dma_release_channel(dma_channel);
	return ret;
}

static void ddmafb_remove(struct platform_device *pdev)
{
	struct fb_info *info = platform_get_drvdata(pdev);
	struct ddmafb_par *par = info->par;

	unregister_framebuffer(info); /* calls ddmafb_destroy */

	dma_free_coherent(&pdev->dev, par->dma_size, par->dma_cpu_addr, par->dma_handle);

	dma_release_channel(par->dma_channel);
}

static const struct of_device_id ddmafb_of_match[] = {
	{ .compatible = "ddma-framebuffer", },
	{ },
};
MODULE_DEVICE_TABLE(of, ddmafb_of_match);

static struct platform_driver ddmafb_driver = {
	.driver = {
		.name = "ddma-framebuffer",
		.of_match_table = ddmafb_of_match,
	},
	.probe = ddmafb_probe,
	.remove = ddmafb_remove,
};

module_platform_driver(ddmafb_driver);

MODULE_AUTHOR("Jeremiah Lowe");
MODULE_DESCRIPTION("Distributed DMA framebuffer driver");
MODULE_LICENSE("GPL v2");
