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
	resource_size_t base;
	resource_size_t size;
	struct resource *mem;
};

/*
 * fb_ops.fb_destroy is called by the last put_fb_info() call at the end
 * of unregister_framebuffer() or fb_release(). Do any cleanup here.
 */
static void ddmafb_destroy(struct fb_info *info)
{
	struct ddmafb_par *par = info->par;
	struct resource *mem = par->mem;

	if (info->screen_base)
		iounmap(info->screen_base);

	framebuffer_release(info);

	if (mem)
		release_mem_region(mem->start, resource_size(mem));
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
	struct simplefb_format *format;
	struct resource memory;
};

static int ddmafb_parse_dt(struct platform_device *pdev,
			   struct ddmafb_params *params)
{
	struct device_node *np = pdev->dev.of_node, *mem;
	int ret;
	const char *format;
	int i;

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

	ret = of_property_read_string(np, "format", &format);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse format property\n");
		return ret;
	}
	params->format = NULL;
	for (i = 0; i < ARRAY_SIZE(ddmafb_formats); i++) {
		if (strcmp(format, ddmafb_formats[i].name))
			continue;
		params->format = &ddmafb_formats[i];
		break;
	}
	if (!params->format) {
		dev_err(&pdev->dev, "Invalid format value\n");
		return -EINVAL;
	}

	mem = of_parse_phandle(np, "memory-region", 0);
	if (mem) {
		ret = of_address_to_resource(mem, 0, &params->memory);
		if (ret < 0) {
			dev_err(&pdev->dev, "failed to parse memory-region\n");
			of_node_put(mem);
			return ret;
		}

		if (of_property_present(np, "reg"))
			dev_warn(&pdev->dev, "preferring \"memory-region\" over \"reg\" property\n");

		of_node_put(mem);
	} else {
		memset(&params->memory, 0, sizeof(params->memory));
	}

	return 0;
}

static int ddmafb_probe(struct platform_device *pdev)
{
	int ret;
	struct ddmafb_params params;
	struct fb_info *info;
	struct ddmafb_par *par;
	struct resource *res, *mem;

	if (fb_get_options("ddmafb", NULL))
		return -ENODEV;

	ret = -ENODEV;
	if (pdev->dev.of_node)
		ret = ddmafb_parse_dt(pdev, &params);

	if (ret)
		return ret;

	if (params.memory.start == 0 && params.memory.end == 0) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		if (!res) {
			dev_err(&pdev->dev, "No memory resource\n");
			return -EINVAL;
		}
	} else {
		res = &params.memory;
	}

	mem = request_mem_region(res->start, resource_size(res), "ddmafb");
	if (!mem) {
		/*
		 * We cannot make this fatal. Sometimes this comes from magic
		 * spaces our resource handlers simply don't know about. Use
		 * the I/O-memory resource as-is and try to map that instead.
		 */
		dev_warn(&pdev->dev, "ddmafb: cannot reserve video memory at %pR\n", res);
		mem = res;
	}

	info = framebuffer_alloc(sizeof(struct ddmafb_par), &pdev->dev);
	if (!info) {
		ret = -ENOMEM;
		goto error_release_mem_region;
	}
	platform_set_drvdata(pdev, info);

	par = info->par;

	info->fix = ddmafb_fix;
	info->fix.smem_start = mem->start;
	info->fix.smem_len = resource_size(mem);
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

	par->base = info->fix.smem_start;
	par->size = info->fix.smem_len;

	info->fbops = &ddmafb_ops;
	info->screen_base = ioremap_wc(info->fix.smem_start,
				       info->fix.smem_len);
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

	if (mem != res)
		par->mem = mem; /* release in clean-up handler */

	ret = devm_aperture_acquire_for_platform_device(pdev, par->base, par->size);
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
error_release_mem_region:
	if (mem != res)
		release_mem_region(mem->start, resource_size(mem));
	return ret;
}

static void ddmafb_remove(struct platform_device *pdev)
{
	struct fb_info *info = platform_get_drvdata(pdev);

	/* ddmafb_destroy takes care of info cleanup */
	unregister_framebuffer(info);
}

static const struct of_device_id ddmafb_of_match[] = {
	{ .compatible = "jlo,ddmafb", },
	{ },
};
MODULE_DEVICE_TABLE(of, ddmafb_of_match);

static struct platform_driver ddmafb_driver = {
	.driver = {
		.name = "dma-framebuffer",
		.of_match_table = ddmafb_of_match,
	},
	.probe = ddmafb_probe,
	.remove = ddmafb_remove,
};

module_platform_driver(ddmafb_driver);

MODULE_AUTHOR("Jeremiah Lowe");
MODULE_DESCRIPTION("Distributed DMA framebuffer driver");
MODULE_LICENSE("GPL v2");
