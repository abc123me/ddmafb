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
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/kthread.h>
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
			      u_int transp, struct fb_info *info) {
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
	u32 frametime_us;
	struct device *dev;
	struct task_struct *dma_thread;
	struct dma_chan *dma_channel;
	dma_addr_t dma_handle;
	void *dma_cpu_addr;
	size_t dma_size;
	bool complete;
};

static void ddmafb_dma_callback(void *data) {
	struct ddmafb_par *par = data;
	par->complete = true;
}

static int ddmafb_thread_init(struct ddmafb_par *par) {
	struct dma_slave_config cfg;
	int ret = 0;

	memset(&cfg, 0, sizeof(struct dma_slave_config));

	/* Configure the channel */
	cfg.direction       = DMA_MEM_TO_DEV;
	cfg.dst_addr        = 0; /* AXI DMA uses stream interface; often 0 */
	cfg.dst_addr_width  = DMA_SLAVE_BUSWIDTH_4_BYTES;
	cfg.dst_maxburst    = 16;
	ret = dmaengine_slave_config(par->dma_channel, &cfg);
	if (ret) {
		dev_err(par->dev, "Failed to configure DMA channel: %d\n", ret);
		return ret;
	}
	//dev_info(par->dev, "Configured 4-byte wide DMA_MEM_TO_DEV slave interface!\n");
	return 0;
}

static int ddmafb_do_xfer(struct ddmafb_par *par) {
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;

	/* Prepare the descriptor */
	desc = dmaengine_prep_slave_single(
		par->dma_channel,
		par->dma_handle,            /* DMA address of buffer */
		par->dma_size,              /* length in bytes */
		DMA_MEM_TO_DEV,             /* direction */
		DMA_PREP_INTERRUPT |        /* fire callback on completion */
		DMA_CTRL_ACK                /* auto-ack the descriptor */
	);
	if (!desc) {
		dev_err(par->dev, "Failed to prepare DMA descriptor\n");
		return -ENOMEM;
	}
	desc->callback       = ddmafb_dma_callback;
	desc->callback_param = par;
	//dev_info("DMA descriptor created at 0x%08X, size 0x%08X!\n", par->dev, par->dma_handle, par->dma_size);

	/* Submit the descriptor */
	cookie = dmaengine_submit(desc);
	if (dma_submit_error(cookie)) {
		dev_err(par->dev, "DMA submit failed\n");
		return -EIO;
	}
	dma_async_issue_pending(par->dma_channel);
	//dev_info(par->dev, "DMA Descriptor submitted to DMA engine\n");
	return 0;
}

static int ddmafb_thread_fn(void *data) {
	struct ddmafb_par *par = data;
	ktime_t last, cur;
	int ret;

	if(!par) {
		pr_err("ddmafb_thread_fn: ddmafb_par pointer is NULL\n");
		return -ENODEV;
	}

	ret = ddmafb_thread_init(par);
	if(ret)
		return ret;

	/* Start doing transfers */
	par->complete = true;
	while (!kthread_should_stop()) {
		cur = ktime_get();
		if (par->frametime_us == 0 || ktime_to_us(ktime_sub(cur, last)) >= par->frametime_us) {
			if (par->complete) {
				last = cur;
				par->complete = false;
				ret = ddmafb_do_xfer(par);
				//dev_info(par->dev, "Transfer done, starting another\n");
				continue;
			}
		}
		msleep(1);
	}
	return 0;
}

/*
 * fb_ops.fb_destroy is called by the last put_fb_info() call at the end
 * of unregister_framebuffer() or fb_release(). Do any cleanup here.
 */
static void ddmafb_destroy(struct fb_info *info) {
	struct ddmafb_par *par = info->par;

	kthread_stop(par->dma_thread);

	//if (info->screen_base)
	//	iounmap(info->screen_base);

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
			   struct ddmafb_params *params) {
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

	of_property_read_u32(np, "framerate", &params->framerate);

	ret = of_property_read_string(np, "dma", &str);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse dma property\n");
		return ret;
	}
	params->chan_name = str;

	return 0;
}

static int ddmafb_probe(struct platform_device *pdev) {
	int ret;
	struct ddmafb_params params;
	struct fb_info *info;
	struct ddmafb_par *par;
	dma_addr_t dma_handle;
	struct dma_chan *dma_channel;
	struct task_struct *dma_thread;
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

	dma_size = params.height * params.stride;
	dma_cpu_addr = dma_alloc_coherent(&pdev->dev, dma_size, &dma_handle, GFP_KERNEL);
	if (!dma_cpu_addr) {
		dev_err(&pdev->dev, "Failed to allocate DMA buffer\n");
		ret = -ENOMEM;
		goto error_release_dma_channel;
	}
	// for some reason using the dma_cpu_addr doesn't work, idfk why
	// this is kernel space so just gonna use the physical address

	info = framebuffer_alloc(sizeof(struct ddmafb_par), &pdev->dev);
	if (!info) {
		ret = -ENOMEM;
		goto error_release_dma_region;
	}
	platform_set_drvdata(pdev, info);

	par = info->par;

	info->fix = ddmafb_fix;
	//info->fix.smem_start = (size_t) dma_cpu_addr;
	info->fix.smem_start = dma_handle;
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

	par->dev = &pdev->dev;
	par->dma_cpu_addr = dma_cpu_addr;
	par->dma_channel = dma_channel;
	par->dma_handle = dma_handle;
	par->dma_size = dma_size;

	if (params.framerate > 0) {
		par->frametime_us = 1000000 / params.framerate;
		dev_info(&pdev->dev, "set to %d fps (%uuS/frame)!\n", params.framerate, par->frametime_us);
	} else {
		par->frametime_us = 0;
		dev_info(&pdev->dev, "framerate limit not set!\n");
	}

	info->fbops = &ddmafb_ops;
	info->pseudo_palette = par->palette;
	info->screen_base = (char*) dma_handle;

	//info->screen_base = (char*) dma_cpu_addr;
	//info->screen_base = ioremap_wc(info->fix.smem_start, info->fix.smem_len);
	/*if (!info->screen_base) {
		dev_info(&pdev->dev, "failed to ioremap screen_base!\n");
		ret = -ENOMEM;
		goto error_fb_release;
	}*/

	dev_info(&pdev->dev, "framebuffer at 0x%lx, 0x%x bytes\n",
			     info->fix.smem_start, info->fix.smem_len);
	dev_info(&pdev->dev, "format=%s, mode=%dx%dx%d, linelength=%d\n",
			     params.format->name,
			     info->var.xres, info->var.yres,
			     info->var.bits_per_pixel, info->fix.line_length);

	//ret = devm_aperture_acquire_for_platform_device(pdev, (resource_size_t) par->dma_cpu_addr, par->dma_size);
	ret = devm_aperture_acquire_for_platform_device(pdev, (resource_size_t) par->dma_handle, par->dma_size);
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

	// Start the DMA thread
	dma_thread = kthread_run(ddmafb_thread_fn, par, "ddmafb");
	if (IS_ERR(dma_thread)) {
		dev_err(&pdev->dev, "Failed to start DMA thread: %ld\n", PTR_ERR(dma_thread));
		return PTR_ERR(dma_thread);
	}
	dev_info(&pdev->dev, "fb%d: started update thread!\n", info->node);

	return 0;

error_unmap:
//	iounmap(info->screen_base);
//error_fb_release:
	framebuffer_release(info);
error_release_dma_region:
	dma_free_coherent(&pdev->dev, dma_size, dma_cpu_addr, dma_handle);
error_release_dma_channel:
	dma_release_channel(dma_channel);
	return ret;
}

static void ddmafb_remove(struct platform_device *pdev) {
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
