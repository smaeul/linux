// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2021-2022 Samuel Holland <samuel@sholland.org>
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/iio/consumer.h>
#include <linux/irq.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

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

#define EBC_FRAME_PENDING		(-1U)

#define EBC_MAX_PHASES			256

#define EBC_NUM_LUT_REGS		0x1000
#define EBC_NUM_SUPPLIES		3

#define EBC_FRAME_TIMEOUT		msecs_to_jiffies(25)
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
	struct iio_channel		*temperature_channel;
	struct regmap			*regmap;
	struct regulator_bulk_data	supplies[EBC_NUM_SUPPLIES];
	struct task_struct		*refresh_thread;
	u32				dsp_start;
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
	.minor			= 3,
	.name			= "rockchip-ebc",
	.desc			= "Rockchip E-Book Controller",
	.date			= "20220303",
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
 * @frame_begin: The frame number when this damage area starts being refreshed
 */
struct rockchip_ebc_area {
	struct list_head		list;
	struct drm_rect			clip;
	u32				frame_begin;
};

/**
 * struct rockchip_ebc_ctx - context for performing display refreshes
 *
 * @kref: Reference count, maintained as part of the CRTC's atomic state
 * @queue: Queue of damaged areas to be refreshed
 * @queue_lock: Lock protecting access to @queue
 * @prev: Display contents (Y4) before this refresh
 * @next: Display contents (Y4) after this refresh
 * @final: Display contents (Y4) after all pending refreshes
 * @phase: Buffers for selecting a phase from the EBC's LUT, 1 byte/pixel
 * @gray4_pitch: Horizontal line length of a Y4 pixel buffer in bytes
 * @gray4_size: Size of a Y4 pixel buffer in bytes
 * @phase_pitch: Horizontal line length of a phase buffer in bytes
 * @phase_size: Size of a phase buffer in bytes
 */
struct rockchip_ebc_ctx {
	struct kref			kref;
	struct list_head		queue;
	spinlock_t			queue_lock;
	u8				*prev;
	u8				*next;
	u8				*final;
	u8				*phase[2];
	u32				gray4_pitch;
	u32				gray4_size;
	u32				phase_pitch;
	u32				phase_size;
};

static void rockchip_ebc_ctx_free(struct rockchip_ebc_ctx *ctx)
{
	struct rockchip_ebc_area *area;

	list_for_each_entry(area, &ctx->queue, list)
		kfree(area);
	kfree(ctx->prev);
	kfree(ctx->next);
	kfree(ctx->final);
	kfree(ctx->phase[0]);
	kfree(ctx->phase[1]);
	kfree(ctx);
}

static struct rockchip_ebc_ctx *rockchip_ebc_ctx_alloc(u32 width, u32 height)
{
	u32 gray4_size = width * height / 2;
	u32 phase_size = width * height;
	struct rockchip_ebc_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	ctx->prev = kmalloc(gray4_size, GFP_KERNEL);
	ctx->next = kmalloc(gray4_size, GFP_KERNEL);
	ctx->final = kmalloc(gray4_size, GFP_KERNEL);
	ctx->phase[0] = kmalloc(phase_size, GFP_KERNEL);
	ctx->phase[1] = kmalloc(phase_size, GFP_KERNEL);
	if (!ctx->prev || !ctx->next || !ctx->final ||
	    !ctx->phase[0] || !ctx->phase[1]) {
		rockchip_ebc_ctx_free(ctx);
		return NULL;
	}

	kref_init(&ctx->kref);
	INIT_LIST_HEAD(&ctx->queue);
	spin_lock_init(&ctx->queue_lock);
	ctx->gray4_pitch = width / 2;
	ctx->gray4_size  = gray4_size;
	ctx->phase_pitch = width;
	ctx->phase_size  = phase_size;

	return ctx;
}

static void rockchip_ebc_ctx_release(struct kref *kref)
{
	struct rockchip_ebc_ctx *ctx =
		container_of(kref, struct rockchip_ebc_ctx, kref);

	return rockchip_ebc_ctx_free(ctx);
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

static void rockchip_ebc_global_refresh(struct rockchip_ebc *ebc,
					const struct rockchip_ebc_ctx *ctx)
{
	struct drm_device *drm = &ebc->drm;
	u32 gray4_size = ctx->gray4_size;
	struct device *dev = drm->dev;

	dma_sync_single_for_device(dev, virt_to_phys(ctx->next),
				   gray4_size, DMA_TO_DEVICE);
	dma_sync_single_for_device(dev, virt_to_phys(ctx->prev),
				   gray4_size, DMA_TO_DEVICE);

	reinit_completion(&ebc->display_end);
	regmap_write(ebc->regmap, EBC_CONFIG_DONE,
		     EBC_CONFIG_DONE_REG_CONFIG_DONE);
	regmap_write(ebc->regmap, EBC_DSP_START,
		     ebc->dsp_start |
		     EBC_DSP_START_DSP_FRM_TOTAL(ebc->lut.num_phases - 1) |
		     EBC_DSP_START_DSP_FRM_START);
	if (!wait_for_completion_timeout(&ebc->display_end,
					 EBC_REFRESH_TIMEOUT))
		drm_err(drm, "Refresh timed out!\n");

	memcpy(ctx->prev, ctx->next, gray4_size);
}

static bool rockchip_ebc_schedule_area(struct list_head *areas,
				       struct rockchip_ebc_area *area,
				       u32 current_frame, u32 num_phases)
{
	struct rockchip_ebc_area *other;
	u32 frame_begin = current_frame;

	list_for_each_entry(other, areas, list) {
		struct drm_rect intersection;
		u32 other_end;

		/* Only consider areas before this one in the list. */
		if (other == area)
			break;

		/* Skip areas that finish refresh before this area begins. */
		other_end = other->frame_begin + num_phases;
		if (other_end <= frame_begin)
			continue;

		/* If there is no collision, the areas are independent. */
		intersection = area->clip;
		if (!drm_rect_intersect(&intersection, &other->clip))
			continue;

		/* If the other area already started, wait until it finishes. */
		if (other->frame_begin < current_frame) {
			frame_begin = other_end;
			continue;
		}

		/*
		 * If the other area has not started yet, and completely
		 * contains this area, then this area is redundant.
		 */
		if (drm_rect_equals(&area->clip, &intersection))
			return false;

		/* Otherwise, start at the same time as the other area. */
		frame_begin = other->frame_begin;
	}

	area->frame_begin = frame_begin;

	return true;
}

static void rockchip_ebc_blit_phase(const struct rockchip_ebc_ctx *ctx,
				    u8 *dst, u8 phase,
				    const struct drm_rect *clip)
{
	unsigned int pitch = ctx->phase_pitch;
	unsigned int width = clip->x2 - clip->x1;
	unsigned int y;
	u8 *dst_line;

	dst_line = dst + clip->y1 * pitch + clip->x1;

	for (y = clip->y1; y < clip->y2; y++) {
		memset(dst_line, phase, width);

		dst_line += pitch;
	}
}

static void rockchip_ebc_blit_pixels(const struct rockchip_ebc_ctx *ctx,
				     u8 *dst, const u8 *src,
				     const struct drm_rect *clip)
{
	unsigned int x1_bytes = clip->x1 / 2;
	unsigned int x2_bytes = clip->x2 / 2;
	unsigned int pitch = ctx->gray4_pitch;
	unsigned int width = x2_bytes - x1_bytes;
	const u8 *src_line;
	unsigned int y;
	u8 *dst_line;

	dst_line = dst + clip->y1 * pitch + x1_bytes;
	src_line = src + clip->y1 * pitch + x1_bytes;

	for (y = clip->y1; y < clip->y2; y++) {
		memcpy(dst_line, src_line, width);

		dst_line += pitch;
		src_line += pitch;
	}
}

static void rockchip_ebc_partial_refresh(struct rockchip_ebc *ebc,
					 struct rockchip_ebc_ctx *ctx)
{
	dma_addr_t next_handle = virt_to_phys(ctx->next);
	dma_addr_t prev_handle = virt_to_phys(ctx->prev);
	struct rockchip_ebc_area *area, *next_area;
	u32 last_phase = ebc->lut.num_phases - 1;
	struct drm_device *drm = &ebc->drm;
	u32 gray4_size = ctx->gray4_size;
	struct device *dev = drm->dev;
	LIST_HEAD(areas);
	u32 frame;

	for (frame = 0;; frame++) {
		/* Swap phase buffers to minimize latency between frames. */
		u8 *phase_buffer = ctx->phase[frame % 2];
		dma_addr_t phase_handle = virt_to_phys(phase_buffer);
		bool sync_next = false;
		bool sync_prev = false;

		/* Move the queued damage areas to the local list. */
		spin_lock(&ctx->queue_lock);
		list_splice_tail_init(&ctx->queue, &areas);
		spin_unlock(&ctx->queue_lock);

		list_for_each_entry_safe(area, next_area, &areas, list) {
			s32 frame_delta;
			u32 phase;

			/*
			 * Determine when this area can start its refresh.
			 * If the area is redundant, drop it immediately.
			 */
			if (area->frame_begin == EBC_FRAME_PENDING &&
			    !rockchip_ebc_schedule_area(&areas, area, frame,
							ebc->lut.num_phases)) {
				list_del(&area->list);
				kfree(area);
				continue;
			}

			frame_delta = frame - area->frame_begin;
			if (frame_delta < 0)
				continue;

			/* Copy ctx->final to ctx->next on the first frame. */
			if (frame_delta == 0) {
				rockchip_ebc_blit_pixels(ctx, ctx->next,
							 ctx->final,
							 &area->clip);
				sync_next = true;
			}

			/*
			 * Take advantage of the fact that the last phase in a
			 * waveform is always zero (neutral polarity). Instead
			 * of writing the actual phase number, write 0xff (the
			 * last possible phase number), which is guaranteed to
			 * be neutral for every waveform.
			 */
			phase = frame_delta >= last_phase ? 0xff : frame_delta;
			rockchip_ebc_blit_phase(ctx, phase_buffer, phase,
						&area->clip);

			/*
			 * Copy ctx->next to ctx->prev after the last phase.
			 * Technically, this races with the hardware computing
			 * the last phase, but the last phase is all zeroes
			 * anyway, regardless of prev/next (see above).
			 *
			 * Keeping the area in the list for one extra frame
			 * also ensures both phase buffers get set to 0xff.
			 */
			if (frame_delta > last_phase) {
				rockchip_ebc_blit_pixels(ctx, ctx->prev,
							 ctx->next,
							 &area->clip);
				sync_prev = true;

				list_del(&area->list);
				kfree(area);
			}
		}

		if (sync_next)
			dma_sync_single_for_device(dev, next_handle,
						   gray4_size, DMA_TO_DEVICE);
		if (sync_prev)
			dma_sync_single_for_device(dev, prev_handle,
						   gray4_size, DMA_TO_DEVICE);
		dma_sync_single_for_device(dev, phase_handle,
					   ctx->phase_size, DMA_TO_DEVICE);

		if (frame) {
			if (!wait_for_completion_timeout(&ebc->display_end,
							 EBC_FRAME_TIMEOUT))
				drm_err(drm, "Frame %d timed out!\n", frame);
		}

		if (list_empty(&areas))
			break;

		regmap_write(ebc->regmap, EBC_WIN_MST2,
			     phase_handle);
		regmap_write(ebc->regmap, EBC_CONFIG_DONE,
			     EBC_CONFIG_DONE_REG_CONFIG_DONE);
		regmap_write(ebc->regmap, EBC_DSP_START,
			     ebc->dsp_start |
			     EBC_DSP_START_DSP_FRM_START);
	}
}

static void rockchip_ebc_refresh(struct rockchip_ebc *ebc,
				 struct rockchip_ebc_ctx *ctx,
				 bool global_refresh,
				 enum drm_epd_waveform waveform)
{
	struct drm_device *drm = &ebc->drm;
	u32 dsp_ctrl = 0, epd_ctrl = 0;
	struct device *dev = drm->dev;
	int ret, temperature;

	/* Resume asynchronously while preparing to refresh. */
	ret = pm_runtime_get(dev);
	if (ret < 0) {
		drm_err(drm, "Failed to request resume: %d\n", ret);
		return;
	}

	ret = iio_read_channel_processed(ebc->temperature_channel, &temperature);
	if (ret < 0) {
		drm_err(drm, "Failed to get temperature: %d\n", ret);
	} else {
		/* Convert from millicelsius to celsius. */
		temperature /= 1000;

		ret = drm_epd_lut_set_temperature(&ebc->lut, temperature);
		if (ret < 0)
			drm_err(drm, "Failed to set LUT temperature: %d\n", ret);
		else if (ret)
			ebc->lut_changed = true;
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

	/*
	 * The hardware has a separate bit for each mode, with some priority
	 * scheme between them. For clarity, only set one bit at a time.
	 */
	if (global_refresh) {
		dsp_ctrl |= EBC_DSP_CTRL_DSP_LUT_MODE;
	} else {
		epd_ctrl |= EBC_EPD_CTRL_DSP_THREE_WIN_MODE;
		if (diff_mode)
			dsp_ctrl |= EBC_DSP_CTRL_DSP_DIFF_MODE;
	}
	regmap_update_bits(ebc->regmap, EBC_EPD_CTRL,
			   EBC_EPD_CTRL_DSP_THREE_WIN_MODE,
			   epd_ctrl);
	regmap_update_bits(ebc->regmap, EBC_DSP_CTRL,
			   EBC_DSP_CTRL_DSP_DIFF_MODE |
			   EBC_DSP_CTRL_DSP_LUT_MODE,
			   dsp_ctrl);

	regmap_write(ebc->regmap, EBC_WIN_MST0,
		     virt_to_phys(ctx->next));
	regmap_write(ebc->regmap, EBC_WIN_MST1,
		     virt_to_phys(ctx->prev));

	if (global_refresh)
		rockchip_ebc_global_refresh(ebc, ctx);
	else
		rockchip_ebc_partial_refresh(ebc, ctx);

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
		 * Initialize the buffers before use. This is deferred to the
		 * kthread to avoid slowing down atomic_check.
		 *
		 * ctx->prev and ctx->next are set to 0xff, all white, because:
		 *  1) the display is set to white by the reset waveform, and
		 *  2) the driver maintains the invariant that the display is
		 *     all white whenever the CRTC is disabled.
		 *
		 * ctx->final is initialized by the first plane update.
		 *
		 * ctx->phase is set to 0xff, the number of the last possible
		 * phase, because the LUT for that phase is known to be all
		 * zeroes. (The last real phase in a waveform is zero in order
		 * to discharge the display, and unused phases in the LUT are
		 * zeroed out.) This prevents undesired driving of the display
		 * in 3-window mode between when the framebuffer is blitted
		 * (and thus prev != next) and when the refresh thread starts
		 * counting phases for that region.
		 */
		memset(ctx->prev, 0xff, ctx->gray4_size);
		memset(ctx->next, 0xff, ctx->gray4_size);
		memset(ctx->phase[0], 0xff, ctx->phase_size);
		memset(ctx->phase[1], 0xff, ctx->phase_size);

		/*
		 * LUTs use both the old and the new pixel values as inputs.
		 * However, the initial contents of the display are unknown.
		 * The special RESET waveform will initialize the display to
		 * known contents (white) regardless of its current contents.
		 */
		if (!ebc->reset_complete) {
			ebc->reset_complete = true;
			rockchip_ebc_refresh(ebc, ctx, true, DRM_EPD_WF_RESET);
		}

		while (!kthread_should_park()) {
			rockchip_ebc_refresh(ebc, ctx, false, default_waveform);

			set_current_state(TASK_IDLE);
			if (list_empty(&ctx->queue))
				schedule();
			__set_current_state(TASK_RUNNING);
		}

		/*
		 * Clear the display before disabling the CRTC. Use the
		 * highest-quality waveform to minimize visible artifacts.
		 */
		memset(ctx->next, 0xff, ctx->gray4_size);
		rockchip_ebc_refresh(ebc, ctx, true, DRM_EPD_WF_GC16);

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
	u16 hact_start, vact_start;
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
	hact_start = sdck.htotal - sdck.hsync_start;
	vact_start = mode.vtotal - mode.vsync_start;

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
		     EBC_DSP_CTRL_DSP_SDCLK_DIV(pixels_per_sdck - 1));
	regmap_write(ebc->regmap, EBC_DSP_HTIMING0,
		     EBC_DSP_HTIMING0_DSP_HTOTAL(sdck.htotal) |
		     /* sync end == sync width */
		     EBC_DSP_HTIMING0_DSP_HS_END(hsync_width));
	regmap_write(ebc->regmap, EBC_DSP_HTIMING1,
		     EBC_DSP_HTIMING1_DSP_HACT_END(hact_start + sdck.hdisplay) |
		     /* minus 1 for fixed delay in timing sequence */
		     EBC_DSP_HTIMING1_DSP_HACT_ST(hact_start - 1));
	regmap_write(ebc->regmap, EBC_DSP_VTIMING0,
		     EBC_DSP_VTIMING0_DSP_VTOTAL(mode.vtotal) |
		     /* sync end == sync width */
		     EBC_DSP_VTIMING0_DSP_VS_END(vsync_width));
	regmap_write(ebc->regmap, EBC_DSP_VTIMING1,
		     EBC_DSP_VTIMING1_DSP_VACT_END(vact_start + mode.vdisplay) |
		     EBC_DSP_VTIMING1_DSP_VACT_ST(vact_start));
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

	/* To keep things simple, always use a window size matching the CRTC. */
	regmap_write(ebc->regmap, EBC_WIN_VIR,
		     EBC_WIN_VIR_WIN_VIR_HEIGHT(mode.vdisplay) |
		     EBC_WIN_VIR_WIN_VIR_WIDTH(mode.hdisplay));
	regmap_write(ebc->regmap, EBC_WIN_ACT,
		     EBC_WIN_ACT_WIN_ACT_HEIGHT(mode.vdisplay) |
		     EBC_WIN_ACT_WIN_ACT_WIDTH(mode.hdisplay));
	regmap_write(ebc->regmap, EBC_WIN_DSP,
		     EBC_WIN_DSP_WIN_DSP_HEIGHT(mode.vdisplay) |
		     EBC_WIN_DSP_WIN_DSP_WIDTH(mode.hdisplay));
	regmap_write(ebc->regmap, EBC_WIN_DSP_ST,
		     EBC_WIN_DSP_ST_WIN_DSP_YST(vact_start) |
		     EBC_WIN_DSP_ST_WIN_DSP_XST(hact_start));
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

		ctx = rockchip_ebc_ctx_alloc(mode->hdisplay, mode->vdisplay);
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
	struct drm_atomic_helper_damage_iter iter;
	struct ebc_plane_state *ebc_plane_state;
	struct drm_plane_state *old_plane_state;
	struct drm_plane_state *plane_state;
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
		area = kmalloc(sizeof(*area), GFP_KERNEL);
		if (!area)
			return -ENOMEM;

		area->frame_begin = EBC_FRAME_PENDING;
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
	src = vaddr + src_clip->y1 * src_pitch + src_clip->x1 * fb->format->cpp[0];

	for (y = src_clip->y1; y < src_clip->y2; y++) {
		const u32 *sbuf = src;
		u8 *dbuf = dst;

		for (x = src_clip->x1; x < src_clip->x2; x += 2) {
			u32 rgb0 = *sbuf++;
			u32 rgb1 = *sbuf++;
			u8 gray;

			/* Truncate the RGB values to 5 bits each. */
			rgb0 &= 0x00f8f8f8U; rgb1 &= 0x00f8f8f8U;
			/* Put the sum 2R+5G+B in bits 24-31. */
			rgb0 *= 0x0020a040U; rgb1 *= 0x0020a040U;
			/* Unbias the value for rounding to 4 bits. */
			rgb0 += 0x07000000U; rgb1 += 0x07000000U;

			gray = rgb0 >> 28 | rgb1 >> 28 << 4;
			changed |= gray ^ *dbuf;
			*dbuf++ = gray;
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
	struct rockchip_ebc_area *area, *next_area;
	struct ebc_plane_state *ebc_plane_state;
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

	list_for_each_entry_safe(area, next_area, &ebc_plane_state->areas, list) {
		struct drm_rect *dst_clip = &area->clip;
		struct drm_rect src_clip = area->clip;
		int adjust;

		/* Convert from plane coordinates to CRTC coordinates. */
		drm_rect_translate(dst_clip, translate_x, translate_y);

		/* Adjust the clips to always process full bytes (2 pixels). */
		adjust = dst_clip->x1 & 1;
		dst_clip->x1 -= adjust;
		src_clip.x1  -= adjust;

		adjust = dst_clip->x2 & 1;
		dst_clip->x2 += adjust;
		src_clip.x2  += adjust;

		if (!rockchip_ebc_blit_fb(ctx, dst_clip, vaddr,
					  plane_state->fb, &src_clip)) {
			/* Drop the area if the FB didn't actually change. */
			list_del(&area->list);
			kfree(area);
		}
	}

	if (list_empty(&ebc_plane_state->areas))
		return;

	spin_lock(&ctx->queue_lock);
	list_splice_tail_init(&ebc_plane_state->areas, &ctx->queue);
	spin_unlock(&ctx->queue_lock);

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
	struct rockchip_ebc_area *area, *next_area;

	list_for_each_entry_safe(area, next_area, &ebc_plane_state->areas, list)
		kfree(area);

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
	DRM_FORMAT_XRGB8888,
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

	drm_fbdev_generic_setup(drm, 0);

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

	ebc->temperature_channel = devm_iio_channel_get(dev, NULL);
	if (IS_ERR(ebc->temperature_channel))
		return dev_err_probe(dev, PTR_ERR(ebc->temperature_channel),
				     "Failed to get temperature I/O channel\n");

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

	ebc->refresh_thread = kthread_create(rockchip_ebc_refresh_thread,
					     ebc, "ebc-refresh/%s",
					     dev_name(dev));
	if (IS_ERR(ebc->refresh_thread)) {
		ret = dev_err_probe(dev, PTR_ERR(ebc->refresh_thread),
				    "Failed to start refresh thread\n");
		goto err_disable_pm;
	}

	kthread_park(ebc->refresh_thread);
	sched_set_fifo(ebc->refresh_thread);

	ret = rockchip_ebc_drm_init(ebc);
	if (ret)
		goto err_stop_kthread;

	return 0;

err_stop_kthread:
	kthread_stop(ebc->refresh_thread);
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
