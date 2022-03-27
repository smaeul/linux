// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021-2022 Samuel Holland <samuel@sholland.org>
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/irq.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_epd_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_simple_kms_helper.h>

#define EBC_DSP_START			0x0000
#define EBC_DSP_START_DSP_OUT_LOW		BIT(31)
#define EBC_DSP_START_DSP_SDCE_WIDTH(x)		((x) << 16)
#define EBC_DSP_START_DSP_EINK_MODE		BIT(13)
#define EBC_DSP_START_SW_BURST_CTRL		BIT(12)
#define EBC_DSP_START_DSP_FRM_TOTAL(x)		((x) << 2)
#define EBC_DSP_START_DSP_RST			BIT(1)
#define EBC_DSP_START_DSP_FRM_START		BIT(0)
#define EBC_EPD_CTRL			0x0004
#define EBC_EPD_CTRL_EINK_MODE_SWAP		BIT(31)
#define EBC_EPD_CTRL_DSP_GD_END(x)		((x) << 16)
#define EBC_EPD_CTRL_DSP_GD_ST(x)		((x) << 8)
#define EBC_EPD_CTRL_DSP_THREE_WIN_MODE		BIT(7)
#define EBC_EPD_CTRL_DSP_SDDW_MODE		BIT(6)
#define EBC_EPD_CTRL_EPD_AUO			BIT(5)
#define EBC_EPD_CTRL_EPD_PWR(x)			((x) << 2)
#define EBC_EPD_CTRL_EPD_GDRL			BIT(1)
#define EBC_EPD_CTRL_EPD_SDSHR			BIT(0)
#define EBC_DSP_CTRL			0x0008
#define EBC_DSP_CTRL_DSP_SWAP_MODE(x)		((x) << 30)
#define EBC_DSP_CTRL_DSP_DIFF_MODE		BIT(29)
#define EBC_DSP_CTRL_DSP_LUT_MODE		BIT(28)
#define EBC_DSP_CTRL_DSP_VCOM_MODE		BIT(27)
#define EBC_DSP_CTRL_DSP_GDOE_POL		BIT(26)
#define EBC_DSP_CTRL_DSP_GDSP_POL		BIT(25)
#define EBC_DSP_CTRL_DSP_GDCLK_POL		BIT(24)
#define EBC_DSP_CTRL_DSP_SDCE_POL		BIT(23)
#define EBC_DSP_CTRL_DSP_SDOE_POL		BIT(22)
#define EBC_DSP_CTRL_DSP_SDLE_POL		BIT(21)
#define EBC_DSP_CTRL_DSP_SDCLK_POL		BIT(20)
#define EBC_DSP_CTRL_DSP_SDCLK_DIV(x)		((x) << 16)
#define EBC_DSP_CTRL_DSP_BACKGROUND(x)		((x) << 0)
#define EBC_DSP_HTIMING0		0x000c
#define EBC_DSP_HTIMING0_DSP_HTOTAL(x)		((x) << 16)
#define EBC_DSP_HTIMING0_DSP_HS_END(x)		((x) << 0)
#define EBC_DSP_HTIMING1		0x0010
#define EBC_DSP_HTIMING1_DSP_HACT_END(x)	((x) << 16)
#define EBC_DSP_HTIMING1_DSP_HACT_ST(x)		((x) << 0)
#define EBC_DSP_VTIMING0		0x0014
#define EBC_DSP_VTIMING0_DSP_VTOTAL(x)		((x) << 16)
#define EBC_DSP_VTIMING0_DSP_VS_END(x)		((x) << 0)
#define EBC_DSP_VTIMING1		0x0018
#define EBC_DSP_VTIMING1_DSP_VACT_END(x)	((x) << 16)
#define EBC_DSP_VTIMING1_DSP_VACT_ST(x)		((x) << 0)
#define EBC_DSP_ACT_INFO		0x001c
#define EBC_DSP_ACT_INFO_DSP_HEIGHT(x)		((x) << 16)
#define EBC_DSP_ACT_INFO_DSP_WIDTH(x)		((x) << 0)
#define EBC_WIN_CTRL			0x0020
#define EBC_WIN_CTRL_WIN2_FIFO_THRESHOLD(x)	((x) << 19)
#define EBC_WIN_CTRL_WIN_EN			BIT(18)
#define EBC_WIN_CTRL_AHB_INCR_NUM_REG(x)	((x) << 13)
#define EBC_WIN_CTRL_AHB_BURST_REG(x)		((x) << 10)
#define EBC_WIN_CTRL_WIN_FIFO_THRESHOLD(x)	((x) << 2)
#define EBC_WIN_CTRL_WIN_FMT_Y4			(0x0 << 0)
#define EBC_WIN_CTRL_WIN_FMT_Y8			(0x1 << 0)
#define EBC_WIN_CTRL_WIN_FMT_XRGB8888		(0x2 << 0)
#define EBC_WIN_CTRL_WIN_FMT_RGB565		(0x3 << 0)
#define EBC_WIN_MST0			0x0024
#define EBC_WIN_MST1			0x0028
#define EBC_WIN_VIR			0x002c
#define EBC_WIN_VIR_WIN_VIR_HEIGHT(x)		((x) << 16)
#define EBC_WIN_VIR_WIN_VIR_WIDTH(x)		((x) << 0)
#define EBC_WIN_ACT			0x0030
#define EBC_WIN_ACT_WIN_ACT_HEIGHT(x)		((x) << 16)
#define EBC_WIN_ACT_WIN_ACT_WIDTH(x)		((x) << 0)
#define EBC_WIN_DSP			0x0034
#define EBC_WIN_DSP_WIN_DSP_HEIGHT(x)		((x) << 16)
#define EBC_WIN_DSP_WIN_DSP_WIDTH(x)		((x) << 0)
#define EBC_WIN_DSP_ST			0x0038
#define EBC_WIN_DSP_ST_WIN_DSP_YST(x)		((x) << 16)
#define EBC_WIN_DSP_ST_WIN_DSP_XST(x)		((x) << 0)
#define EBC_INT_STATUS			0x003c
#define EBC_INT_STATUS_DSP_FRM_INT_NUM(x)	((x) << 12)
#define EBC_INT_STATUS_LINE_FLAG_INT_CLR	BIT(11)
#define EBC_INT_STATUS_DSP_FRM_INT_CLR		BIT(10)
#define EBC_INT_STATUS_DSP_END_INT_CLR		BIT(9)
#define EBC_INT_STATUS_FRM_END_INT_CLR		BIT(8)
#define EBC_INT_STATUS_LINE_FLAG_INT_MSK	BIT(7)
#define EBC_INT_STATUS_DSP_FRM_INT_MSK		BIT(6)
#define EBC_INT_STATUS_DSP_END_INT_MSK		BIT(5)
#define EBC_INT_STATUS_FRM_END_INT_MSK		BIT(4)
#define EBC_INT_STATUS_LINE_FLAG_INT_ST		BIT(3)
#define EBC_INT_STATUS_DSP_FRM_INT_ST		BIT(2)
#define EBC_INT_STATUS_DSP_END_INT_ST		BIT(1)
#define EBC_INT_STATUS_FRM_END_INT_ST		BIT(0)
#define EBC_VCOM0			0x0040
#define EBC_VCOM1			0x0044
#define EBC_VCOM2			0x0048
#define EBC_VCOM3			0x004c
#define EBC_CONFIG_DONE			0x0050
#define EBC_CONFIG_DONE_REG_CONFIG_DONE		BIT(0)
#define EBC_VNUM			0x0054
#define EBC_VNUM_DSP_VCNT(x)			((x) << 16)
#define EBC_VNUM_LINE_FLAG_NUM(x)		((x) << 0)
#define EBC_WIN_MST2			0x0058
#define EBC_LUT_DATA			0x1000

#define EBC_MAX_PHASES			256
#define EBC_NUM_LUT_REGS		0x1000
#define EBC_NUM_SUPPLIES		3

#define EBC_REFRESH_TIMEOUT		msecs_to_jiffies(3000)
#define EBC_SUSPEND_DELAY_MS		2000

struct rockchip_ebc {
	struct clk			*dclk;
	struct clk			*hclk;
	struct completion		display_end;
	struct drm_crtc			crtc;
	struct drm_device		drm;
	struct drm_encoder		encoder;
	struct drm_epd_lut		lut;
	struct drm_epd_lut_file		lut_file;
	struct drm_plane		plane;
	struct kmem_cache		*area_cache;
	struct regmap			*regmap;
	struct regulator_bulk_data	supplies[EBC_NUM_SUPPLIES];
	struct task_struct		*refresh_thread;
	u32				dsp_start;
	u16				hact_start;
	u16				vact_start;
	bool				lut_changed;
	bool				reset_complete;
};

static int default_waveform = DRM_EPD_WF_GC16;
module_param(default_waveform, int, 0644);
MODULE_PARM_DESC(default_waveform, "waveform to use for display updates");

static bool diff_mode = true;
module_param(diff_mode, bool, 0644);
MODULE_PARM_DESC(diff_mode, "only compute waveforms for changed pixels");

static bool skip_reset;
module_param(skip_reset, bool, 0444);
MODULE_PARM_DESC(skip_reset, "skip the initial display reset");

DEFINE_DRM_GEM_FOPS(rockchip_ebc_fops);

static const struct drm_driver rockchip_ebc_drm_driver = {
	.lastclose		= drm_fb_helper_lastclose,
	DRM_GEM_SHMEM_DRIVER_OPS,
	.major			= 0,
	.minor			= 6,
	.name			= "rockchip-ebc",
	.desc			= "Rockchip E-Book Controller",
	.date			= "20220618",
	.driver_features	= DRIVER_ATOMIC | DRIVER_GEM | DRIVER_MODESET,
	.fops			= &rockchip_ebc_fops,
};

static const struct drm_mode_config_funcs rockchip_ebc_mode_config_funcs = {
	.fb_create		= drm_gem_fb_create_with_dirty,
	.atomic_check		= drm_atomic_helper_check,
	.atomic_commit		= drm_atomic_helper_commit,
};

/**
 * struct rockchip_ebc_area - describes a damaged area of the display
 *
 * @list: Used to put this area in the state/context/refresh thread list
 * @clip: The rectangular clip of this damage area
 */
struct rockchip_ebc_area {
	struct list_head		list;
	struct drm_rect			clip;
};

static void rockchip_ebc_free_areas(struct rockchip_ebc *ebc,
				    struct list_head *areas)
{
	struct rockchip_ebc_area *area, *next;

	list_for_each_entry_safe(area, next, areas, list)
		kmem_cache_free(ebc->area_cache, area);
}

/**
 * struct rockchip_ebc_ctx - context for performing display refreshes
 *
 * @kref: Reference count, maintained as part of the CRTC's atomic state
 * @areas: Queue of damaged areas to be refreshed
 * @areas_lock: Lock protecting access to @areas
 * @ebc: EBC device owning this context
 * @final: Display contents (Y4) after all pending refreshes
 * @next: Display contents (Y4) after this refresh
 * @prev: Display contents (Y4) before this refresh
 * @next_dma: DMA address for @next buffer
 * @prev_dma: DMA address for @prev buffer
 * @gray4_pitch: Horizontal line length of a Y4 pixel buffer in bytes
 * @gray4_size: Size of a Y4 pixel buffer in bytes
 */
struct rockchip_ebc_ctx {
	struct kref			kref;
	struct list_head		areas;
	spinlock_t			areas_lock;
	struct rockchip_ebc		*ebc;
	u8				*final;
	u8				*next;
	u8				*prev;
	dma_addr_t			next_dma;
	dma_addr_t			prev_dma;
	u32				gray4_pitch;
	u32				gray4_size;
};

static struct rockchip_ebc_ctx *rockchip_ebc_ctx_alloc(struct rockchip_ebc *ebc,
						       u32 width, u32 height)
{
	struct device *dev = ebc->drm.dev;
	struct rockchip_ebc_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	kref_init(&ctx->kref);
	INIT_LIST_HEAD(&ctx->areas);
	spin_lock_init(&ctx->areas_lock);
	ctx->ebc = ebc;
	ctx->gray4_pitch = width / 2;
	ctx->gray4_size  = ctx->gray4_pitch * height;

	ctx->final = kmalloc(ctx->gray4_size, GFP_KERNEL);
	if (!ctx->final)
		goto err_free_ctx;

	ctx->next = dma_alloc_noncoherent(dev, ctx->gray4_size,
					  &ctx->next_dma,
					  DMA_TO_DEVICE, GFP_KERNEL);
	if (!ctx->next)
		goto err_free_final;

	ctx->prev = dma_alloc_noncoherent(dev, ctx->gray4_size,
					  &ctx->prev_dma,
					  DMA_TO_DEVICE, GFP_KERNEL);
	if (!ctx->prev)
		goto err_free_next;

	return ctx;

err_free_next:
	dma_free_noncoherent(dev, ctx->gray4_size, ctx->next,
			     ctx->next_dma, DMA_TO_DEVICE);
err_free_final:
	kfree(ctx->final);
err_free_ctx:
	kfree(ctx);

	return NULL;
}

static void rockchip_ebc_ctx_release(struct kref *kref)
{
	struct rockchip_ebc_ctx *ctx =
		container_of(kref, struct rockchip_ebc_ctx, kref);
	struct device *dev = ctx->ebc->drm.dev;

	dma_free_noncoherent(dev, ctx->gray4_size, ctx->prev,
			     ctx->prev_dma, DMA_TO_DEVICE);
	dma_free_noncoherent(dev, ctx->gray4_size, ctx->next,
			     ctx->next_dma, DMA_TO_DEVICE);
	kfree(ctx->final);
	rockchip_ebc_free_areas(ctx->ebc, &ctx->areas);
	kfree(ctx);
}

/*
 * CRTC
 */

struct ebc_crtc_state {
	struct drm_crtc_state		base;
	struct rockchip_ebc_ctx		*ctx;
};

static inline struct ebc_crtc_state *
to_ebc_crtc_state(struct drm_crtc_state *crtc_state)
{
	return container_of(crtc_state, struct ebc_crtc_state, base);
}

enum ebc_refresh_type {
	CLEAR_SCREEN,
	GLOBAL_REFRESH,
};

static void rockchip_ebc_refresh(struct rockchip_ebc *ebc,
				 struct rockchip_ebc_ctx *ctx,
				 enum ebc_refresh_type refresh_type,
				 enum drm_epd_waveform waveform)
{
	struct drm_display_mode *mode = &ebc->crtc.state->adjusted_mode;
	struct drm_device *drm = &ebc->drm;
	struct device *dev = drm->dev;
	unsigned int dsp_ctrl = 0;
	int ret;

	/* Resume asynchronously while preparing to refresh. */
	ret = pm_runtime_get(dev);
	if (ret < 0) {
		drm_err(drm, "Failed to request resume: %d\n", ret);
		return;
	}

	ret = drm_epd_lut_set_waveform(&ebc->lut, waveform);
	if (ret < 0)
		drm_err(drm, "Failed to set LUT waveform: %d\n", ret);
	else if (ret)
		ebc->lut_changed = true;

	/* Wait for the resume to complete before writing any registers. */
	ret = pm_runtime_resume(dev);
	if (ret < 0) {
		drm_err(drm, "Failed to resume: %d\n", ret);
		pm_runtime_put(dev);
		return;
	}

	/* This flag may have been set above, or by the runtime PM callback. */
	if (ebc->lut_changed) {
		ebc->lut_changed = false;
		regmap_bulk_write(ebc->regmap, EBC_LUT_DATA,
				  ebc->lut.buf, EBC_NUM_LUT_REGS);
	}

	regmap_write(ebc->regmap, EBC_DSP_START,
		     ebc->dsp_start);

	if (refresh_type != CLEAR_SCREEN && diff_mode)
		dsp_ctrl |= EBC_DSP_CTRL_DSP_DIFF_MODE;
	regmap_update_bits(ebc->regmap, EBC_DSP_CTRL,
			   EBC_DSP_CTRL_DSP_DIFF_MODE,
			   dsp_ctrl);

	for (;;) {
		u32 win_start, win_bytes;
		struct drm_rect win;
		LIST_HEAD(areas);

		if (refresh_type == CLEAR_SCREEN) {
			win = (struct drm_rect) {
				0, 0, mode->hdisplay, mode->vdisplay
			};

			win_start = 0;
			win_bytes = ctx->gray4_size;

			memset(ctx->next + win_start, 0xff, win_bytes);
		} else if (refresh_type == GLOBAL_REFRESH) {
			struct drm_rect win = { width, height, 0, 0 };
			struct rockchip_ebc_area *area;

			/* Consume the list of damaged areas before copying final. */
			spin_lock(&ctx->areas_lock);
			list_splice_tail_init(&ctx->areas, &areas);
			spin_unlock(&ctx->areas_lock);

			if (list_empty(&ctx->areas))
				break;

			win = (struct drm_rect) {
				mode->hdisplay, mode->vdisplay, 0, 0
			};

			list_for_each_entry(area, &areas, list) {
				win.x1 = min(win.x1, area->clip.x1);
				win.y1 = min(win.y1, area->clip.y1);
				win.x2 = max(win.x2, area->clip.x2);
				win.y2 = max(win.y2, area->clip.y2);
			}

			/* Must start/end on a clock cycle boundary. */
			win.x1 &= ~7;
			win.x2 +=  7;
			win.x2 &= ~7;

			win_start = win.y1 * ctx->gray4_pitch + win.x1 / 2;
			win_bytes = (drm_rect_height(&win) - 1) * ctx->gray4_pitch +
				    drm_rect_width(&win) / 2;

			memcpy(ctx->next + win_start, ctx->final + win_start,
			       win_bytes);
		}

		dma_sync_single_for_device(dev, ctx->next_dma + win_start,
					   win_bytes, DMA_TO_DEVICE);
		dma_sync_single_for_device(dev, ctx->prev_dma + win_start,
					   win_bytes, DMA_TO_DEVICE);

		regmap_write(ebc->regmap, EBC_WIN_MST0,
			     ctx->next_dma + win_start);
		regmap_write(ebc->regmap, EBC_WIN_MST1,
			     ctx->prev_dma + win_start);
		regmap_write(ebc->regmap, EBC_WIN_VIR,
			     EBC_WIN_VIR_WIN_VIR_HEIGHT(drm_rect_height(&win)) |
			     EBC_WIN_VIR_WIN_VIR_WIDTH(ctx->gray4_pitch * 2));
		regmap_write(ebc->regmap, EBC_WIN_ACT,
			     EBC_WIN_ACT_WIN_ACT_HEIGHT(drm_rect_height(&win)) |
			     EBC_WIN_ACT_WIN_ACT_WIDTH(drm_rect_width(&win)));
		regmap_write(ebc->regmap, EBC_WIN_DSP,
			     EBC_WIN_DSP_WIN_DSP_HEIGHT(drm_rect_height(&win)) |
			     EBC_WIN_DSP_WIN_DSP_WIDTH(drm_rect_width(&win)));
		regmap_write(ebc->regmap, EBC_WIN_DSP_ST,
			     EBC_WIN_DSP_ST_WIN_DSP_YST(ebc->vact_start + win.y1) |
			     EBC_WIN_DSP_ST_WIN_DSP_XST(ebc->hact_start + win.x1 / 8));
		regmap_write(ebc->regmap, EBC_CONFIG_DONE,
			     EBC_CONFIG_DONE_REG_CONFIG_DONE);
		regmap_write(ebc->regmap, EBC_DSP_START,
			     ebc->dsp_start |
			     EBC_DSP_START_DSP_FRM_TOTAL(ebc->lut.num_phases - 1) |
			     EBC_DSP_START_DSP_FRM_START);

		/* Free the areas while waiting for the hardware to finish. */
		rockchip_ebc_free_areas(ebc, &areas);

		if (!wait_for_completion_timeout(&ebc->display_end,
						 EBC_REFRESH_TIMEOUT))
			drm_err(drm, "Refresh timed out!\n");

		memcpy(ctx->prev + win_start, ctx->next + win_start, win_bytes);

		if (refresh_type == CLEAR_SCREEN)
			break;
	}

	/* Drive the output pins low once the refresh is complete. */
	regmap_write(ebc->regmap, EBC_DSP_START,
		     ebc->dsp_start |
		     EBC_DSP_START_DSP_OUT_LOW);

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
}

static int rockchip_ebc_refresh_thread(void *data)
{
	struct rockchip_ebc *ebc = data;
	struct rockchip_ebc_ctx *ctx;

	while (!kthread_should_stop()) {
		/* The context will change each time the thread is unparked. */
		ctx = to_ebc_crtc_state(READ_ONCE(ebc->crtc.state))->ctx;

		/*
		 * LUTs use both the old and the new pixel values as inputs.
		 * However, the initial contents of the display are unknown.
		 * The special RESET waveform will initialize the display to
		 * white regardless of its current contents.
		 */
		if (!ebc->reset_complete) {
			ebc->reset_complete = true;
			rockchip_ebc_refresh(ebc, ctx, CLEAR_SCREEN,
					     DRM_EPD_WF_RESET);
		} else {
			/*
			 * Initialize the buffers before use. This is deferred
			 * to the kthread to avoid slowing down atomic_check.
			 *
			 * ctx->final is initialized by the first plane update.
			 *
			 * ctx->next and ctx->prev are set to 0xff because:
			 *  1) the display is cleared to white by the reset
			 *     waveform, and
			 *  2) the driver maintains the invariant that the
			 *     display is white whenever the CRTC is disabled.
			 */
			memset(ctx->next, 0xff, ctx->gray4_size);
			memset(ctx->prev, 0xff, ctx->gray4_size);
		}

		while (!kthread_should_park()) {
			rockchip_ebc_refresh(ebc, ctx, GLOBAL_REFRESH,
					     default_waveform);

			set_current_state(TASK_IDLE);
			if (list_empty(&ctx->areas))
				schedule();
			__set_current_state(TASK_RUNNING);
		}

		/*
		 * Clear the display before disabling the CRTC. Use the
		 * highest-quality waveform to minimize visible artifacts.
		 */
		rockchip_ebc_refresh(ebc, ctx, CLEAR_SCREEN, DRM_EPD_WF_GC16);

		kthread_parkme();
	}

	return 0;
}

static inline struct rockchip_ebc *crtc_to_ebc(struct drm_crtc *crtc)
{
	return container_of(crtc, struct rockchip_ebc, crtc);
}

static void rockchip_ebc_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	struct rockchip_ebc *ebc = crtc_to_ebc(crtc);
	struct drm_display_mode mode = crtc->state->adjusted_mode;
	struct drm_display_mode sdck;
	u16 hsync_width, vsync_width;
	u16 pixels_per_sdck;
	bool bus_16bit;

	/*
	 * Hardware needs horizontal timings in SDCK (source driver clock)
	 * cycles, not pixels. Bus width is either 8 bits (normal) or 16 bits
	 * (DRM_MODE_FLAG_CLKDIV2), and each pixel uses two data bits.
	 */
	bus_16bit = !!(mode.flags & DRM_MODE_FLAG_CLKDIV2);
	pixels_per_sdck = bus_16bit ? 8 : 4;
	sdck.hdisplay = mode.hdisplay / pixels_per_sdck;
	sdck.hsync_start = mode.hsync_start / pixels_per_sdck;
	sdck.hsync_end = mode.hsync_end / pixels_per_sdck;
	sdck.htotal = mode.htotal / pixels_per_sdck;
	sdck.hskew = mode.hskew / pixels_per_sdck;

	/*
	 * Linux timing order is display/fp/sync/bp. Hardware timing order is
	 * sync/bp/display/fp, aka sync/start/display/end.
	 */
	ebc->hact_start = sdck.htotal - sdck.hsync_start;
	ebc->vact_start = mode.vtotal - mode.vsync_start;

	hsync_width = sdck.hsync_end - sdck.hsync_start;
	vsync_width = mode.vsync_end - mode.vsync_start;

	clk_set_rate(ebc->dclk, mode.clock * 1000);

	ebc->dsp_start = EBC_DSP_START_DSP_SDCE_WIDTH(sdck.hdisplay) |
			 EBC_DSP_START_SW_BURST_CTRL;
	regmap_write(ebc->regmap, EBC_EPD_CTRL,
		     EBC_EPD_CTRL_DSP_GD_END(sdck.htotal - sdck.hskew) |
		     EBC_EPD_CTRL_DSP_GD_ST(hsync_width + sdck.hskew) |
		     EBC_EPD_CTRL_DSP_SDDW_MODE * bus_16bit);
	regmap_write(ebc->regmap, EBC_DSP_CTRL,
		     /* no swap */
		     EBC_DSP_CTRL_DSP_SWAP_MODE(bus_16bit ? 2 : 3) |
		     EBC_DSP_CTRL_DSP_LUT_MODE |
		     EBC_DSP_CTRL_DSP_SDCLK_DIV(pixels_per_sdck - 1));
	regmap_write(ebc->regmap, EBC_DSP_HTIMING0,
		     EBC_DSP_HTIMING0_DSP_HTOTAL(sdck.htotal) |
		     /* sync end == sync width */
		     EBC_DSP_HTIMING0_DSP_HS_END(hsync_width));
	regmap_write(ebc->regmap, EBC_DSP_HTIMING1,
		     EBC_DSP_HTIMING1_DSP_HACT_END(ebc->hact_start + sdck.hdisplay) |
		     /* minus 1 for a fixed delay in the timing sequence */
		     EBC_DSP_HTIMING1_DSP_HACT_ST(ebc->hact_start - 1));
	regmap_write(ebc->regmap, EBC_DSP_VTIMING0,
		     EBC_DSP_VTIMING0_DSP_VTOTAL(mode.vtotal) |
		     /* sync end == sync width */
		     EBC_DSP_VTIMING0_DSP_VS_END(vsync_width));
	regmap_write(ebc->regmap, EBC_DSP_VTIMING1,
		     EBC_DSP_VTIMING1_DSP_VACT_END(ebc->vact_start + mode.vdisplay) |
		     EBC_DSP_VTIMING1_DSP_VACT_ST(ebc->vact_start));
	regmap_write(ebc->regmap, EBC_DSP_ACT_INFO,
		     EBC_DSP_ACT_INFO_DSP_HEIGHT(mode.vdisplay) |
		     EBC_DSP_ACT_INFO_DSP_WIDTH(mode.hdisplay));
	regmap_write(ebc->regmap, EBC_WIN_CTRL,
		     /* FIFO depth - 16 */
		     EBC_WIN_CTRL_WIN2_FIFO_THRESHOLD(496) |
		     EBC_WIN_CTRL_WIN_EN |
		     /* INCR16 */
		     EBC_WIN_CTRL_AHB_BURST_REG(7) |
		     /* FIFO depth - 16 */
		     EBC_WIN_CTRL_WIN_FIFO_THRESHOLD(240) |
		     EBC_WIN_CTRL_WIN_FMT_Y4);
}

static int rockchip_ebc_crtc_atomic_check(struct drm_crtc *crtc,
					  struct drm_atomic_state *state)
{
	struct rockchip_ebc *ebc = crtc_to_ebc(crtc);
	struct ebc_crtc_state *ebc_crtc_state;
	struct drm_crtc_state *crtc_state;
	struct rockchip_ebc_ctx *ctx;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (!crtc_state->mode_changed)
		return 0;

	if (crtc_state->enable) {
		struct drm_display_mode *mode = &crtc_state->adjusted_mode;
		long rate = mode->clock * 1000;

		rate = clk_round_rate(ebc->dclk, rate);
		if (rate < 0)
			return rate;
		mode->clock = rate / 1000;

		ctx = rockchip_ebc_ctx_alloc(ebc, mode->hdisplay, mode->vdisplay);
		if (!ctx)
			return -ENOMEM;
	} else {
		ctx = NULL;
	}

	ebc_crtc_state = to_ebc_crtc_state(crtc_state);
	if (ebc_crtc_state->ctx)
		kref_put(&ebc_crtc_state->ctx->kref, rockchip_ebc_ctx_release);
	ebc_crtc_state->ctx = ctx;

	return 0;
}

static void rockchip_ebc_crtc_atomic_flush(struct drm_crtc *crtc,
					   struct drm_atomic_state *state)
{
}

static void rockchip_ebc_crtc_atomic_enable(struct drm_crtc *crtc,
					    struct drm_atomic_state *state)
{
	struct rockchip_ebc *ebc = crtc_to_ebc(crtc);
	struct drm_crtc_state *crtc_state;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (crtc_state->mode_changed)
		kthread_unpark(ebc->refresh_thread);
}

static void rockchip_ebc_crtc_atomic_disable(struct drm_crtc *crtc,
					     struct drm_atomic_state *state)
{
	struct rockchip_ebc *ebc = crtc_to_ebc(crtc);
	struct drm_crtc_state *crtc_state;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (crtc_state->mode_changed)
		kthread_park(ebc->refresh_thread);
}

static const struct drm_crtc_helper_funcs rockchip_ebc_crtc_helper_funcs = {
	.mode_set_nofb		= rockchip_ebc_crtc_mode_set_nofb,
	.atomic_check		= rockchip_ebc_crtc_atomic_check,
	.atomic_flush		= rockchip_ebc_crtc_atomic_flush,
	.atomic_enable		= rockchip_ebc_crtc_atomic_enable,
	.atomic_disable		= rockchip_ebc_crtc_atomic_disable,
};

static void rockchip_ebc_crtc_destroy_state(struct drm_crtc *crtc,
					    struct drm_crtc_state *crtc_state);

static void rockchip_ebc_crtc_reset(struct drm_crtc *crtc)
{
	struct ebc_crtc_state *ebc_crtc_state;

	if (crtc->state)
		rockchip_ebc_crtc_destroy_state(crtc, crtc->state);

	ebc_crtc_state = kzalloc(sizeof(*ebc_crtc_state), GFP_KERNEL);
	if (!ebc_crtc_state)
		return;

	__drm_atomic_helper_crtc_reset(crtc, &ebc_crtc_state->base);
}

static struct drm_crtc_state *
rockchip_ebc_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct ebc_crtc_state *ebc_crtc_state;

	if (!crtc->state)
		return NULL;

	ebc_crtc_state = kzalloc(sizeof(*ebc_crtc_state), GFP_KERNEL);
	if (!ebc_crtc_state)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &ebc_crtc_state->base);

	ebc_crtc_state->ctx = to_ebc_crtc_state(crtc->state)->ctx;
	if (ebc_crtc_state->ctx)
		kref_get(&ebc_crtc_state->ctx->kref);

	return &ebc_crtc_state->base;
}

static void rockchip_ebc_crtc_destroy_state(struct drm_crtc *crtc,
					    struct drm_crtc_state *crtc_state)
{
	struct ebc_crtc_state *ebc_crtc_state = to_ebc_crtc_state(crtc_state);

	if (ebc_crtc_state->ctx)
		kref_put(&ebc_crtc_state->ctx->kref, rockchip_ebc_ctx_release);

	__drm_atomic_helper_crtc_destroy_state(&ebc_crtc_state->base);

	kfree(ebc_crtc_state);
}

static const struct drm_crtc_funcs rockchip_ebc_crtc_funcs = {
	.reset			= rockchip_ebc_crtc_reset,
	.destroy		= drm_crtc_cleanup,
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= drm_atomic_helper_page_flip,
	.atomic_duplicate_state	= rockchip_ebc_crtc_duplicate_state,
	.atomic_destroy_state	= rockchip_ebc_crtc_destroy_state,
};

/*
 * Plane
 */

struct ebc_plane_state {
	struct drm_shadow_plane_state	base;
	struct list_head		areas;
};

static inline struct ebc_plane_state *
to_ebc_plane_state(struct drm_plane_state *plane_state)
{
	return container_of(plane_state, struct ebc_plane_state, base.base);
}

static inline struct rockchip_ebc *plane_to_ebc(struct drm_plane *plane)
{
	return container_of(plane, struct rockchip_ebc, plane);
}

static int rockchip_ebc_plane_atomic_check(struct drm_plane *plane,
					   struct drm_atomic_state *state)
{
	struct drm_plane_state *old_plane_state, *plane_state;
	struct rockchip_ebc *ebc = plane_to_ebc(plane);
	struct drm_atomic_helper_damage_iter iter;
	struct ebc_plane_state *ebc_plane_state;
	struct drm_crtc_state *crtc_state;
	struct rockchip_ebc_area *area;
	struct drm_rect clip;
	int ret;

	plane_state = drm_atomic_get_new_plane_state(state, plane);
	if (!plane_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, plane_state->crtc);
	ret = drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						  DRM_PLANE_HELPER_NO_SCALING,
						  DRM_PLANE_HELPER_NO_SCALING,
						  true, true);
	if (ret)
		return ret;

	ebc_plane_state = to_ebc_plane_state(plane_state);
	old_plane_state = drm_atomic_get_old_plane_state(state, plane);
	drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
	drm_atomic_for_each_plane_damage(&iter, &clip) {
		area = kmem_cache_alloc(ebc->area_cache, GFP_KERNEL);
		if (!area)
			return -ENOMEM;

		area->clip = clip;
		list_add_tail(&area->list, &ebc_plane_state->areas);
	}

	return 0;
}

static bool rockchip_ebc_blit_fb(const struct rockchip_ebc_ctx *ctx,
				 const struct drm_rect *dst_clip,
				 const void *vaddr,
				 const struct drm_framebuffer *fb,
				 const struct drm_rect *src_clip)
{
	unsigned int dst_pitch = ctx->gray4_pitch;
	unsigned int src_pitch = fb->pitches[0];
	unsigned int x, y;
	const void *src;
	u8 changed = 0;
	void *dst;

	dst = ctx->final + dst_clip->y1 * dst_pitch + dst_clip->x1 / 2;
	src = vaddr      + src_clip->y1 * src_pitch + src_clip->x1;

	for (y = src_clip->y1; y < src_clip->y2; y++) {
		const u8 *sbuf = src;
		u8 *dbuf = dst;

		x = dst_clip->x1 / 2;

		if (dst_clip->x1 % 2) {
			u8 hi = *sbuf++;
			u8 old = *dbuf;
			u8 out;

			out = (old & 0xf) | (hi & 0xf0);
			changed |= out ^ old;
			*dbuf++ = out;
			x++;
		}

		for (; x < dst_clip->x2 / 2; x++) {
			u8 lo = *sbuf++;
			u8 hi = *sbuf++;
			u8 out;

			out = (lo >> 4) | (hi & 0xf0);
			changed |= out ^ *dbuf;
			*dbuf++ = out;
		}

		if (dst_clip->x2 % 2) {
			u8 lo = *sbuf++;
			u8 old = *dbuf;
			u8 out;

			out = (lo >> 4) | (old & 0xf0);
			changed |= out ^ old;
			*dbuf++ = out;
		}

		dst += dst_pitch;
		src += src_pitch;
	}

	return !!changed;
}

static void rockchip_ebc_plane_atomic_update(struct drm_plane *plane,
					     struct drm_atomic_state *state)
{
	struct rockchip_ebc *ebc = plane_to_ebc(plane);
	struct ebc_plane_state *ebc_plane_state;
	struct rockchip_ebc_area *area, *next;
	struct drm_plane_state *plane_state;
	struct drm_crtc_state *crtc_state;
	struct rockchip_ebc_ctx *ctx;
	int translate_x, translate_y;
	struct drm_rect src;
	const void *vaddr;

	plane_state = drm_atomic_get_new_plane_state(state, plane);
	if (!plane_state->crtc)
		return;

	crtc_state = drm_atomic_get_new_crtc_state(state, plane_state->crtc);
	ctx = to_ebc_crtc_state(crtc_state)->ctx;

	drm_rect_fp_to_int(&src, &plane_state->src);
	translate_x = plane_state->dst.x1 - src.x1;
	translate_y = plane_state->dst.y1 - src.y1;

	ebc_plane_state = to_ebc_plane_state(plane_state);
	vaddr = ebc_plane_state->base.data[0].vaddr;

	list_for_each_entry_safe(area, next, &ebc_plane_state->areas, list) {
		struct drm_rect *dst_clip = &area->clip;
		struct drm_rect src_clip = area->clip;

		/* Convert from plane coordinates to CRTC coordinates. */
		drm_rect_translate(dst_clip, translate_x, translate_y);

		if (!rockchip_ebc_blit_fb(ctx, dst_clip, vaddr,
					  plane_state->fb, &src_clip)) {
			/* Drop the area if the FB didn't actually change. */
			list_del(&area->list);
			kmem_cache_free(ebc->area_cache, area);
		}
	}

	if (list_empty(&ebc_plane_state->areas))
		return;

	spin_lock(&ctx->areas_lock);
	list_splice_tail_init(&ebc_plane_state->areas, &ctx->areas);
	spin_unlock(&ctx->areas_lock);

	wake_up_process(ebc->refresh_thread);
}

static const struct drm_plane_helper_funcs rockchip_ebc_plane_helper_funcs = {
	.prepare_fb		= drm_gem_prepare_shadow_fb,
	.cleanup_fb		= drm_gem_cleanup_shadow_fb,
	.atomic_check		= rockchip_ebc_plane_atomic_check,
	.atomic_update		= rockchip_ebc_plane_atomic_update,
};

static void rockchip_ebc_plane_destroy_state(struct drm_plane *plane,
					     struct drm_plane_state *plane_state);

static void rockchip_ebc_plane_reset(struct drm_plane *plane)
{
	struct ebc_plane_state *ebc_plane_state;

	if (plane->state)
		rockchip_ebc_plane_destroy_state(plane, plane->state);

	ebc_plane_state = kzalloc(sizeof(*ebc_plane_state), GFP_KERNEL);
	if (!ebc_plane_state)
		return;

	__drm_gem_reset_shadow_plane(plane, &ebc_plane_state->base);

	INIT_LIST_HEAD(&ebc_plane_state->areas);
}

static struct drm_plane_state *
rockchip_ebc_plane_duplicate_state(struct drm_plane *plane)
{
	struct ebc_plane_state *ebc_plane_state;

	if (!plane->state)
		return NULL;

	ebc_plane_state = kzalloc(sizeof(*ebc_plane_state), GFP_KERNEL);
	if (!ebc_plane_state)
		return NULL;

	__drm_gem_duplicate_shadow_plane_state(plane, &ebc_plane_state->base);

	INIT_LIST_HEAD(&ebc_plane_state->areas);

	return &ebc_plane_state->base.base;
}

static void rockchip_ebc_plane_destroy_state(struct drm_plane *plane,
					     struct drm_plane_state *plane_state)
{
	struct ebc_plane_state *ebc_plane_state = to_ebc_plane_state(plane_state);
	struct rockchip_ebc *ebc = plane_to_ebc(plane);

	rockchip_ebc_free_areas(ebc, &ebc_plane_state->areas);

	__drm_gem_destroy_shadow_plane_state(&ebc_plane_state->base);

	kfree(ebc_plane_state);
}

static const struct drm_plane_funcs rockchip_ebc_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	.reset			= rockchip_ebc_plane_reset,
	.atomic_duplicate_state	= rockchip_ebc_plane_duplicate_state,
	.atomic_destroy_state	= rockchip_ebc_plane_destroy_state,
};

static const u32 rockchip_ebc_plane_formats[] = {
	DRM_FORMAT_R4,
};

static const u64 rockchip_ebc_plane_format_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

static int rockchip_ebc_drm_init(struct rockchip_ebc *ebc)
{
	struct drm_device *drm = &ebc->drm;
	struct drm_bridge *bridge;
	int ret;

	ret = drmm_epd_lut_file_init(drm, &ebc->lut_file, "rockchip/ebc.wbf");
	if (ret)
		return ret;

	ret = drmm_epd_lut_init(&ebc->lut_file, &ebc->lut,
				DRM_EPD_LUT_4BIT_PACKED, EBC_MAX_PHASES);
	if (ret)
		return ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.max_width = DRM_SHADOW_PLANE_MAX_WIDTH;
	drm->mode_config.max_height = DRM_SHADOW_PLANE_MAX_HEIGHT;
	drm->mode_config.funcs = &rockchip_ebc_mode_config_funcs;
	drm->mode_config.quirk_addfb_prefer_host_byte_order = true;

	drm_plane_helper_add(&ebc->plane, &rockchip_ebc_plane_helper_funcs);
	ret = drm_universal_plane_init(drm, &ebc->plane, 0,
				       &rockchip_ebc_plane_funcs,
				       rockchip_ebc_plane_formats,
				       ARRAY_SIZE(rockchip_ebc_plane_formats),
				       rockchip_ebc_plane_format_modifiers,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;

	drm_plane_enable_fb_damage_clips(&ebc->plane);

	drm_crtc_helper_add(&ebc->crtc, &rockchip_ebc_crtc_helper_funcs);
	ret = drm_crtc_init_with_planes(drm, &ebc->crtc, &ebc->plane, NULL,
					&rockchip_ebc_crtc_funcs, NULL);
	if (ret)
		return ret;

	ebc->encoder.possible_crtcs = drm_crtc_mask(&ebc->crtc);
	ret = drm_simple_encoder_init(drm, &ebc->encoder, DRM_MODE_ENCODER_NONE);
	if (ret)
		return ret;

	bridge = devm_drm_of_get_bridge(drm->dev, drm->dev->of_node, 0, 0);
	if (IS_ERR(bridge))
		return PTR_ERR(bridge);

	ret = drm_bridge_attach(&ebc->encoder, bridge, NULL, 0);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_fbdev_generic_setup(drm, 8);

	return 0;
}

static int __maybe_unused rockchip_ebc_suspend(struct device *dev)
{
	struct rockchip_ebc *ebc = dev_get_drvdata(dev);
	int ret;

	ret = drm_mode_config_helper_suspend(&ebc->drm);
	if (ret)
		return ret;

	return pm_runtime_force_suspend(dev);
}

static int __maybe_unused rockchip_ebc_resume(struct device *dev)
{
	struct rockchip_ebc *ebc = dev_get_drvdata(dev);

	pm_runtime_force_resume(dev);

	return drm_mode_config_helper_resume(&ebc->drm);
}

static int rockchip_ebc_runtime_suspend(struct device *dev)
{
	struct rockchip_ebc *ebc = dev_get_drvdata(dev);

	regcache_cache_only(ebc->regmap, true);

	clk_disable_unprepare(ebc->dclk);
	clk_disable_unprepare(ebc->hclk);
	regulator_bulk_disable(EBC_NUM_SUPPLIES, ebc->supplies);

	return 0;
}

static int rockchip_ebc_runtime_resume(struct device *dev)
{
	struct rockchip_ebc *ebc = dev_get_drvdata(dev);
	int ret;

	ret = regulator_bulk_enable(EBC_NUM_SUPPLIES, ebc->supplies);
	if (ret)
		return ret;

	ret = clk_prepare_enable(ebc->hclk);
	if (ret)
		goto err_disable_supplies;

	ret = clk_prepare_enable(ebc->dclk);
	if (ret)
		goto err_disable_hclk;

	/*
	 * Do not restore the LUT registers here, because the temperature or
	 * waveform may have changed since the last refresh. Instead, have the
	 * refresh thread program the LUT during the next refresh.
	 */
	ebc->lut_changed = true;

	regcache_cache_only(ebc->regmap, false);
	regcache_mark_dirty(ebc->regmap);
	regcache_sync(ebc->regmap);

	regmap_write(ebc->regmap, EBC_INT_STATUS,
		     EBC_INT_STATUS_DSP_END_INT_CLR |
		     EBC_INT_STATUS_LINE_FLAG_INT_MSK |
		     EBC_INT_STATUS_DSP_FRM_INT_MSK |
		     EBC_INT_STATUS_FRM_END_INT_MSK);

	return 0;

err_disable_hclk:
	clk_disable_unprepare(ebc->hclk);
err_disable_supplies:
	regulator_bulk_disable(EBC_NUM_SUPPLIES, ebc->supplies);

	return ret;
}

static const struct dev_pm_ops rockchip_ebc_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(rockchip_ebc_suspend, rockchip_ebc_resume)
	SET_RUNTIME_PM_OPS(rockchip_ebc_runtime_suspend,
			   rockchip_ebc_runtime_resume, NULL)
};

static bool rockchip_ebc_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case EBC_DSP_START:
	case EBC_INT_STATUS:
	case EBC_CONFIG_DONE:
	case EBC_VNUM:
		return true;
	default:
		/* Do not cache the LUT registers. */
		return reg > EBC_WIN_MST2;
	}
}

static const struct regmap_config rockchip_ebc_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.volatile_reg	= rockchip_ebc_volatile_reg,
	.max_register	= 0x4ffc, /* end of EBC_LUT_DATA */
	.cache_type	= REGCACHE_FLAT,
};

static const char *const rockchip_ebc_supplies[EBC_NUM_SUPPLIES] = {
	"panel",
	"vcom",
	"vdrive",
};

static irqreturn_t rockchip_ebc_irq(int irq, void *dev_id)
{
	struct rockchip_ebc *ebc = dev_id;
	unsigned int status;

	regmap_read(ebc->regmap, EBC_INT_STATUS, &status);

	if (status & EBC_INT_STATUS_DSP_END_INT_ST) {
		status |= EBC_INT_STATUS_DSP_END_INT_CLR;
		complete(&ebc->display_end);
	}

	regmap_write(ebc->regmap, EBC_INT_STATUS, status);

	return IRQ_HANDLED;
}

static int rockchip_ebc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rockchip_ebc *ebc;
	void __iomem *base;
	int i, ret;

	ebc = devm_drm_dev_alloc(dev, &rockchip_ebc_drm_driver,
				 struct rockchip_ebc, drm);
	if (IS_ERR(ebc))
		return PTR_ERR(ebc);

	platform_set_drvdata(pdev, ebc);
	init_completion(&ebc->display_end);
	ebc->reset_complete = skip_reset;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	ebc->regmap = devm_regmap_init_mmio(dev, base,
					     &rockchip_ebc_regmap_config);
	if (IS_ERR(ebc->regmap))
		return PTR_ERR(ebc->regmap);

	regcache_cache_only(ebc->regmap, true);

	ebc->dclk = devm_clk_get(dev, "dclk");
	if (IS_ERR(ebc->dclk))
		return dev_err_probe(dev, PTR_ERR(ebc->dclk),
				     "Failed to get dclk\n");

	ebc->hclk = devm_clk_get(dev, "hclk");
	if (IS_ERR(ebc->hclk))
		return dev_err_probe(dev, PTR_ERR(ebc->hclk),
				     "Failed to get hclk\n");

	for (i = 0; i < EBC_NUM_SUPPLIES; i++)
		ebc->supplies[i].supply = rockchip_ebc_supplies[i];

	ret = devm_regulator_bulk_get(dev, EBC_NUM_SUPPLIES, ebc->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get supplies\n");

	ret = devm_request_irq(dev, platform_get_irq(pdev, 0),
			       rockchip_ebc_irq, 0, dev_name(dev), ebc);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to request IRQ\n");

	pm_runtime_set_autosuspend_delay(dev, EBC_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);
	if (!pm_runtime_enabled(dev)) {
		ret = rockchip_ebc_runtime_resume(&pdev->dev);
		if (ret)
			return ret;
	}

	ebc->area_cache = KMEM_CACHE(rockchip_ebc_area, 0);
	if (!ebc->area_cache) {
		ret = -ENOMEM;
		goto err_disable_pm;
	}

	ebc->refresh_thread = kthread_create(rockchip_ebc_refresh_thread,
					     ebc, "ebc-refresh/%s",
					     dev_name(dev));
	if (IS_ERR(ebc->refresh_thread)) {
		ret = dev_err_probe(dev, PTR_ERR(ebc->refresh_thread),
				    "Failed to start refresh thread\n");
		goto err_destroy_cache;
	}

	kthread_park(ebc->refresh_thread);
	sched_set_fifo(ebc->refresh_thread);

	ret = rockchip_ebc_drm_init(ebc);
	if (ret)
		goto err_stop_kthread;

	return 0;

err_stop_kthread:
	kthread_stop(ebc->refresh_thread);
err_destroy_cache:
	kmem_cache_destroy(ebc->area_cache);
err_disable_pm:
	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		rockchip_ebc_runtime_suspend(dev);

	return ret;
}

static int rockchip_ebc_remove(struct platform_device *pdev)
{
	struct rockchip_ebc *ebc = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	drm_dev_unregister(&ebc->drm);
	drm_atomic_helper_shutdown(&ebc->drm);

	kthread_stop(ebc->refresh_thread);

	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		rockchip_ebc_runtime_suspend(dev);

	return 0;
}

static void rockchip_ebc_shutdown(struct platform_device *pdev)
{
	struct rockchip_ebc *ebc = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	drm_atomic_helper_shutdown(&ebc->drm);

	if (!pm_runtime_status_suspended(dev))
		rockchip_ebc_runtime_suspend(dev);
}

static const struct of_device_id rockchip_ebc_of_match[] = {
	{ .compatible = "rockchip,rk3568-ebc" },
	{ }
};
MODULE_DEVICE_TABLE(of, rockchip_ebc_of_match);

static struct platform_driver rockchip_ebc_driver = {
	.probe		= rockchip_ebc_probe,
	.remove		= rockchip_ebc_remove,
	.shutdown	= rockchip_ebc_shutdown,
	.driver		= {
		.name		= "rockchip-ebc",
		.of_match_table	= rockchip_ebc_of_match,
		.pm		= &rockchip_ebc_dev_pm_ops,
	},
};
module_platform_driver(rockchip_ebc_driver);

MODULE_AUTHOR("Samuel Holland <samuel@sholland.org>");
MODULE_DESCRIPTION("Rockchip EBC driver");
MODULE_LICENSE("GPL");
