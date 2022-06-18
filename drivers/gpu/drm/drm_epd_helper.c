// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Electrophoretic Display Helper Library
 *
 * Copyright (C) 2022 Samuel Holland <samuel@sholland.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/vmalloc.h>

#include <drm/drm_device.h>
#include <drm/drm_epd_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_print.h>

/**
 * DOC: overview
 *
 * This library provides functions for working with the lookup tables (LUTs)
 * used by electrophoretic displays (EPDs). It fills in a LUT buffer based on
 * the selected waveform, the panel temperature, and the buffer format needed
 * by the driver.
 */

struct pvi_wbf_offset {
	u8			b[3];
};

struct pvi_wbf_pointer {
	struct pvi_wbf_offset	offset;
	u8			checksum;
};

struct pvi_wbf_file_header {
	__le32			checksum;		// 0x00
	__le32			file_size;		// 0x04
	__le32			serial;			// 0x08
	u8			run_type;		// 0x0c
	u8			fpl_platform;		// 0x0d
	__le16			fpl_lot;		// 0x0e
	u8			mode_version;		// 0x10
	u8			wf_version;		// 0x11
	u8			wf_subversion;		// 0x12
	u8			wf_type;		// 0x13
	u8			panel_size;		// 0x14
	u8			amepd_part_number;	// 0x15
	u8			wf_rev;			// 0x16
	u8			frame_rate_bcd;		// 0x17
	u8			frame_rate_hex;		// 0x18
	u8			vcom_offset;		// 0x19
	u8			unknown[2];		// 0x1a
	struct pvi_wbf_offset	xwia;			// 0x1c
	u8			cs1;			// 0x1f
	struct pvi_wbf_offset	wmta;			// 0x20
	u8			fvsn;			// 0x23
	u8			luts;			// 0x24
	u8			mode_count;		// 0x25
	u8			temp_range_count;	// 0x26
	u8			advanced_wf_flags;	// 0x27
	u8			eb;			// 0x28
	u8			sb;			// 0x29
	u8			reserved[5];		// 0x2a
	u8			cs2;			// 0x2f
	u8			temp_range_table[];	// 0x30
};
static_assert(sizeof(struct pvi_wbf_file_header) == 0x30);

struct pvi_wbf_mode_info {
	u8			versions[2];
	u8			format;
	u8			modes[DRM_EPD_WF_MAX];
};

static const struct pvi_wbf_mode_info pvi_wbf_mode_info_table[] = {
	{
		.versions = {
			0x09,
		},
		.format = DRM_EPD_LUT_4BIT_PACKED,
		.modes = {
			[DRM_EPD_WF_RESET]	= 0,
			[DRM_EPD_WF_DU]		= 1,
			[DRM_EPD_WF_DU4]	= 1,
			[DRM_EPD_WF_GC16]	= 2,
			[DRM_EPD_WF_GL16]	= 3,
			[DRM_EPD_WF_GLR16]	= 3,
			[DRM_EPD_WF_GLD16]	= 3,
			[DRM_EPD_WF_A2]		= 4,
			[DRM_EPD_WF_GCC16]	= 3,
		},
	},
	{
		.versions = {
			0x12,
		},
		.format = DRM_EPD_LUT_4BIT_PACKED,
		.modes = {
			[DRM_EPD_WF_RESET]	= 0,
			[DRM_EPD_WF_DU]		= 1,
			[DRM_EPD_WF_DU4]	= 7,
			[DRM_EPD_WF_GC16]	= 3,
			[DRM_EPD_WF_GL16]	= 3,
			[DRM_EPD_WF_GLR16]	= 5,
			[DRM_EPD_WF_GLD16]	= 6,
			[DRM_EPD_WF_A2]		= 4,
			[DRM_EPD_WF_GCC16]	= 5,
		},
	},
	{
		.versions = {
			0x16,
		},
		.format = DRM_EPD_LUT_5BIT_PACKED,
		.modes = {
			[DRM_EPD_WF_RESET]	= 0,
			[DRM_EPD_WF_DU]		= 1,
			[DRM_EPD_WF_DU4]	= 1,
			[DRM_EPD_WF_GC16]	= 2,
			[DRM_EPD_WF_GL16]	= 3,
			[DRM_EPD_WF_GLR16]	= 4,
			[DRM_EPD_WF_GLD16]	= 4,
			[DRM_EPD_WF_A2]		= 6,
			[DRM_EPD_WF_GCC16]	= 5,
		},
	},
	{
		.versions = {
			0x18,
			0x20,
		},
		.format = DRM_EPD_LUT_5BIT_PACKED,
		.modes = {
			[DRM_EPD_WF_RESET]	= 0,
			[DRM_EPD_WF_DU]		= 1,
			[DRM_EPD_WF_DU4]	= 1,
			[DRM_EPD_WF_GC16]	= 2,
			[DRM_EPD_WF_GL16]	= 3,
			[DRM_EPD_WF_GLR16]	= 4,
			[DRM_EPD_WF_GLD16]	= 5,
			[DRM_EPD_WF_A2]		= 6,
			[DRM_EPD_WF_GCC16]	= 4,
		},
	},
	{
		.versions = {
			0x19,
			0x43,
		},
		.format = DRM_EPD_LUT_5BIT_PACKED,
		.modes = {
			[DRM_EPD_WF_RESET]	= 0,
			[DRM_EPD_WF_DU]		= 1,
			[DRM_EPD_WF_DU4]	= 7,
			[DRM_EPD_WF_GC16]	= 2,
			[DRM_EPD_WF_GL16]	= 3,
			[DRM_EPD_WF_GLR16]	= 4,
			[DRM_EPD_WF_GLD16]	= 5,
			[DRM_EPD_WF_A2]		= 6,
			[DRM_EPD_WF_GCC16]	= 4,
		},
	},
	{
		.versions = {
			0x23,
		},
		.format = DRM_EPD_LUT_4BIT_PACKED,
		.modes = {
			[DRM_EPD_WF_RESET]	= 0,
			[DRM_EPD_WF_DU]		= 1,
			[DRM_EPD_WF_DU4]	= 5,
			[DRM_EPD_WF_GC16]	= 2,
			[DRM_EPD_WF_GL16]	= 3,
			[DRM_EPD_WF_GLR16]	= 3,
			[DRM_EPD_WF_GLD16]	= 3,
			[DRM_EPD_WF_A2]		= 4,
			[DRM_EPD_WF_GCC16]	= 3,
		},
	},
	{
		.versions = {
			0x54,
		},
		.format = DRM_EPD_LUT_4BIT_PACKED,
		.modes = {
			[DRM_EPD_WF_RESET]	= 0,
			[DRM_EPD_WF_DU]		= 1,
			[DRM_EPD_WF_DU4]	= 1,
			[DRM_EPD_WF_GC16]	= 2,
			[DRM_EPD_WF_GL16]	= 3,
			[DRM_EPD_WF_GLR16]	= 4,
			[DRM_EPD_WF_GLD16]	= 4,
			[DRM_EPD_WF_A2]		= 5,
			[DRM_EPD_WF_GCC16]	= 4,
		},
	},
};

static const void *pvi_wbf_apply_offset(const struct drm_epd_lut_file *file,
					const struct pvi_wbf_offset *offset)
{
	u32 bytes = offset->b[0] | offset->b[1] << 8 | offset->b[2] << 16;

	if (bytes >= file->fw->size)
		return NULL;

	return (const void *)file->header + bytes;
}

static const void *pvi_wbf_dereference(const struct drm_epd_lut_file *file,
				       const struct pvi_wbf_pointer *ptr)
{
	u8 sum = ptr->offset.b[0] + ptr->offset.b[1] + ptr->offset.b[2];

	if (ptr->checksum != sum)
		return NULL;

	return pvi_wbf_apply_offset(file, &ptr->offset);
}

static int pvi_wbf_get_mode_index(const struct drm_epd_lut_file *file,
				  enum drm_epd_waveform waveform)
{
	if (waveform >= DRM_EPD_WF_MAX)
		return -EINVAL;

	return file->mode_info->modes[waveform];
}

static int pvi_wbf_get_mode_info(struct drm_epd_lut_file *file)
{
	u8 mode_version = file->header->mode_version;
	const struct pvi_wbf_mode_info *mode_info;
	int i, v;

	for (i = 0; i < ARRAY_SIZE(pvi_wbf_mode_info_table); i++) {
		mode_info = &pvi_wbf_mode_info_table[i];

		for (v = 0; v < ARRAY_SIZE(mode_info->versions); v++) {
			if (mode_info->versions[v] == mode_version) {
				file->mode_info = mode_info;
				return 0;
			}
		}
	}

	drm_err(file->dev, "Unknown PVI waveform version 0x%02x\n",
		mode_version);

	return -EOPNOTSUPP;
}

static int pvi_wbf_get_temp_index(const struct drm_epd_lut_file *file,
				  int temperature)
{
	const struct pvi_wbf_file_header *header = file->header;
	int i;

	for (i = 0; i < header->temp_range_count; i++)
		if (temperature < header->temp_range_table[i])
			return i - 1;

	return header->temp_range_count - 1;
}

static int pvi_wbf_validate_header(struct drm_epd_lut_file *file)
{
	const struct pvi_wbf_file_header *header = file->header;
	int ret;

	if (le32_to_cpu(header->file_size) > file->fw->size)
		return -EINVAL;

	ret = pvi_wbf_get_mode_info(file);
	if (ret)
		return ret;

	drm_info(file->dev, "Loaded %d-bit PVI waveform version 0x%02x\n",
		 file->mode_info->format == DRM_EPD_LUT_5BIT_PACKED ? 5 : 4,
		 header->mode_version);

	return 0;
}

static void drm_epd_lut_file_free(struct drm_device *dev, void *res)
{
	struct drm_epd_lut_file *file = res;

	release_firmware(file->fw);
}

/**
 * drmm_epd_lut_file_init - Initialize a managed EPD LUT file
 *
 * @dev: The DRM device owning this LUT file
 * @file: The LUT file to initialize
 * @file_name: The filesystem name of the LUT file
 *
 * Return: negative errno on failure, 0 otherwise
 */
int drmm_epd_lut_file_init(struct drm_device *dev,
			   struct drm_epd_lut_file *file,
			   const char *file_name)
{
	int ret;

	ret = request_firmware(&file->fw, file_name, dev->dev);
	if (ret)
		return ret;

	ret = drmm_add_action_or_reset(dev, drm_epd_lut_file_free, file);
	if (ret)
		return ret;

	file->dev = dev;
	file->header = (const void *)file->fw->data;

	ret = pvi_wbf_validate_header(file);
	if (ret)
		return ret;

	/* Only 5-bit waveform files are supported by drm_epd_lut_convert. */
	if (file->mode_info->format != DRM_EPD_LUT_5BIT_PACKED)
		return -EOPNOTSUPP;

	return 0;
}
EXPORT_SYMBOL(drmm_epd_lut_file_init);

/**
 * drm_epd_lut_size_shift - Get the size of a LUT phase in power-of-2 bytes
 *
 * @format: One of the LUT buffer formats
 *
 * Return: buffer size shift amount
 */
static int drm_epd_lut_size_shift(enum drm_epd_lut_format format)
{
	switch (format) {
	case DRM_EPD_LUT_4BIT:
		/* (4 bits/pixel)^2 / 1 pixel/byte */
		return 4 + 4;
	case DRM_EPD_LUT_4BIT_PACKED:
		/* (4 bits/pixel)^2 / 4 pixels/byte */
		return 4 + 4 - 2;
	case DRM_EPD_LUT_5BIT:
		/* (5 bits/pixel)^2 / 1 pixel/byte */
		return 5 + 5;
	case DRM_EPD_LUT_5BIT_PACKED:
		/* (5 bits/pixel)^2 / 4 pixels/byte */
		return 5 + 5 - 2;
	}

	unreachable();
}

static int pvi_wbf_decode_lut(struct drm_epd_lut *lut, const u8 *lut_data)
{

	unsigned int copies, max_bytes, size_shift, state, total_bytes;
	struct drm_device *dev = lut->file->dev;
	const u8 *in = lut_data;
	u8 *out = lut->buf;
	u8 token;

	size_shift = drm_epd_lut_size_shift(lut->file->mode_info->format);
	max_bytes = lut->max_phases << size_shift;
	total_bytes = 0;
	state = 1;

	/* Read tokens until reaching the end-of-input marker. */
	while ((token = *in++) != 0xff) {
		/* Special handling for the state switch token. */
		if (token == 0xfc) {
			state = !state;
			token = *in++;
		}

		/*
		 * State 0 is a sequence of data bytes.
		 * State 1 is a sequence of [data byte, extra copies] pairs.
		 */
		copies = 1 + (state ? *in++ : 0);

		total_bytes += copies;
		if (total_bytes > max_bytes) {
			drm_err(dev, "LUT contains too many phases\n");
			lut->num_phases = 0;
			return -EILSEQ;
		}

		while (copies--)
			*out++ = token;
	}

	lut->num_phases = total_bytes >> size_shift;
	if (total_bytes != lut->num_phases << size_shift) {
		drm_err(dev, "LUT contains a partial phase\n");
		lut->num_phases = 0;
		return -EILSEQ;
	}

	drm_dbg_core(dev, "LUT contains %d phases (%ld => %ld bytes)\n",
		     lut->num_phases, in - lut_data, out - lut->buf);

	return 0;
}

static int pvi_wbf_get_lut(struct drm_epd_lut *lut,
			   int mode_index, int temp_index)
{
	const struct pvi_wbf_pointer *mode_table, *temp_table;
	const struct drm_epd_lut_file *file = lut->file;
	const u8 *lut_data;
	int ret;

	mode_table = pvi_wbf_apply_offset(file, &file->header->wmta);
	if (!mode_table)
		return -EFAULT;

	temp_table = pvi_wbf_dereference(file, &mode_table[mode_index]);
	if (!temp_table)
		return -EFAULT;

	lut_data = pvi_wbf_dereference(file, &temp_table[temp_index]);
	if (!lut_data)
		return -EFAULT;

	ret = pvi_wbf_decode_lut(lut, lut_data);
	if (ret)
		return ret;

	return 0;
}

static void drm_epd_lut_convert(const struct drm_epd_lut *lut)
{
	enum drm_epd_lut_format from = lut->file->mode_info->format;
	enum drm_epd_lut_format to = lut->format;
	u8 *buf = lut->buf;
	size_t x, y;

	if (to == from)
		return;

	switch (to) {
	case DRM_EPD_LUT_4BIT:
		for (y = 0; y < 16 * lut->num_phases; ++y) {
			for (x = 8; x--;) {
				u8 byte = buf[16 * y + x];

				buf[16 * y + 2 * x + 0] = (byte >> 0) & 0x03;
				buf[16 * y + 2 * x + 1] = (byte >> 4) & 0x03;
			}
		}
		for (; y < 16 * lut->max_phases; ++y) {
			for (x = 8; x--;) {
				buf[16 * y + 2 * x + 0] = 0;
				buf[16 * y + 2 * x + 1] = 0;
			}
		}
		break;
	case DRM_EPD_LUT_4BIT_PACKED:
		for (y = 0; y < 16 * lut->num_phases; ++y) {
			for (x = 4; x--;) {
				u8 lo_byte = buf[16 * y + 2 * x + 0] & 0x33;
				u8 hi_byte = buf[16 * y + 2 * x + 1] & 0x33;

				/* Copy bits 4:5 => bits 2:3. */
				lo_byte |= lo_byte >> 2;
				hi_byte |= hi_byte >> 2;

				buf[4 * y + x] = (lo_byte & 0xf) |
						 (hi_byte << 4);
			}
		}
		for (; y < 16 * lut->max_phases; ++y) {
			for (x = 4; x--;)
				buf[4 * y + x] = 0;
		}
		break;
	case DRM_EPD_LUT_5BIT:
		memset(buf + 256 * lut->num_phases, 0,
		       256 * (lut->max_phases - lut->num_phases));
		for (x = 256 * lut->num_phases; x--;) {
			u8 byte = buf[x];

			buf[4 * x + 0] = (byte >> 0) & 0x03;
			buf[4 * x + 1] = (byte >> 2) & 0x03;
			buf[4 * x + 2] = (byte >> 4) & 0x03;
			buf[4 * x + 3] = (byte >> 6) & 0x03;
		}
		break;
	case DRM_EPD_LUT_5BIT_PACKED:
		/* Nothing to do. */
		break;
	}
}

static int drm_epd_lut_update(struct drm_epd_lut *lut,
			      int mode_index, int temp_index)
{
	int ret;

	ret = pvi_wbf_get_lut(lut, mode_index, temp_index);
	if (ret)
		return ret;

	drm_epd_lut_convert(lut);

	return 0;
}

/**
 * drm_epd_lut_set_temperature - Update the LUT due to panel temperature change
 *
 * @lut: The LUT structure
 * @temperateure: The current panel temperature in degrees Celsius
 *
 * Return: negative errno on failure, 1 if LUT was changed, 0 otherwise
 */
int drm_epd_lut_set_temperature(struct drm_epd_lut *lut,
				int temperature)
{
	int temp_index;
	int ret;

	temp_index = pvi_wbf_get_temp_index(lut->file, temperature);
	if (temp_index < 0)
		return -ENOENT;

	if (temp_index == lut->temp_index)
		return 0;

	drm_dbg_core(lut->file->dev, "LUT temperature changed (%d)\n",
		     temperature);

	ret = drm_epd_lut_update(lut, lut->mode_index, temp_index);
	if (ret)
		return ret;

	lut->temp_index = temp_index;

	return 1;
}
EXPORT_SYMBOL(drm_epd_lut_set_temperature);

/**
 * drm_epd_lut_set_waveform - Update the LUT due to waveform selection change
 *
 * @lut: The LUT structure
 * @waveform: The desired waveform
 *
 * Return: negative errno on failure, 1 if LUT was changed, 0 otherwise
 */
int drm_epd_lut_set_waveform(struct drm_epd_lut *lut,
			     enum drm_epd_waveform waveform)
{
	int mode_index;
	int ret;

	mode_index = pvi_wbf_get_mode_index(lut->file, waveform);
	if (mode_index < 0)
		return -ENOENT;

	if (mode_index == lut->mode_index)
		return 0;

	drm_dbg_core(lut->file->dev, "LUT waveform changed (%d)\n",
		     waveform);

	ret = drm_epd_lut_update(lut, mode_index, lut->temp_index);
	if (ret)
		return ret;

	lut->mode_index = mode_index;

	return 1;
}
EXPORT_SYMBOL(drm_epd_lut_set_waveform);

static void drm_epd_lut_free(struct drm_device *dev, void *res)
{
	struct drm_epd_lut *lut = res;

	vfree(lut->buf);
}

/**
 * drmm_epd_lut_init - Initialize a managed EPD LUT from a LUT file
 *
 * @dev: The DRM device owning this LUT
 * @lut: The LUT to initialize
 * @file_name: The file name of the waveform firmware
 * @format: The LUT buffer format needed by the driver
 * @max_phases: The maximum number of waveform phases supported by the driver
 *
 * Return: negative errno on failure, 0 otherwise
 */
int drmm_epd_lut_init(struct drm_epd_lut_file *file,
		      struct drm_epd_lut *lut,
		      enum drm_epd_lut_format format,
		      unsigned int max_phases)
{
	size_t max_order;
	int ret;

	/* Allocate a buffer large enough to convert the LUT in place. */
	max_order = max(drm_epd_lut_size_shift(file->mode_info->format),
			drm_epd_lut_size_shift(format));
	lut->buf = vmalloc(max_phases << max_order);
	if (!lut->buf)
		return -ENOMEM;

	ret = drmm_add_action_or_reset(file->dev, drm_epd_lut_free, lut);
	if (ret)
		return ret;

	lut->file	= file;
	lut->format	= format;
	lut->max_phases	= max_phases;
	lut->num_phases	= 0;

	/* Set sane initial values for the waveform and temperature. */
	lut->mode_index = pvi_wbf_get_mode_index(file, DRM_EPD_WF_RESET);
	if (lut->mode_index < 0)
		return -ENOENT;

	lut->temp_index = pvi_wbf_get_temp_index(file, 25);
	if (lut->temp_index < 0)
		return -ENOENT;

	ret = drm_epd_lut_update(lut, lut->mode_index, lut->temp_index);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL(drmm_epd_lut_init);

MODULE_AUTHOR("Samuel Holland <samuel@sholland.org>");
MODULE_DESCRIPTION("DRM EPD waveform LUT library");
MODULE_LICENSE("Dual MIT/GPL");
