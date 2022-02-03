/* SPDX-License-Identifier: GPL-2.0 OR MIT */

#ifndef __DRM_EPD_HELPER_H__
#define __DRM_EPD_HELPER_H__

#define DRM_EPD_DEFAULT_TEMPERATURE	25

struct drm_device;
struct firmware;
struct pvi_wbf_file_header;
struct pvi_wbf_mode_info;

/**
 * enum drm_epd_waveform - Identifiers for waveforms used to drive EPD pixels
 *
 * @DRM_EPD_WF_RESET: Used to initialize the panel, ends with white
 * @DRM_EPD_WF_A2: Fast transitions between black and white only
 * @DRM_EPD_WF_DU: Transitions 16-level grayscale to monochrome
 * @DRM_EPD_WF_DU4: Transitions 16-level grayscale to 4-level grayscale
 * @DRM_EPD_WF_GC16: High-quality but flashy 16-level grayscale
 * @DRM_EPD_WF_GCC16: Less flashy 16-level grayscale
 * @DRM_EPD_WF_GL16: Less flashy 16-level grayscale
 * @DRM_EPD_WF_GLR16: Less flashy 16-level grayscale, plus anti-ghosting
 * @DRM_EPD_WF_GLD16: Less flashy 16-level grayscale, plus anti-ghosting
 */
enum drm_epd_waveform {
	DRM_EPD_WF_RESET,
	DRM_EPD_WF_A2,
	DRM_EPD_WF_DU,
	DRM_EPD_WF_DU4,
	DRM_EPD_WF_GC16,
	DRM_EPD_WF_GCC16,
	DRM_EPD_WF_GL16,
	DRM_EPD_WF_GLR16,
	DRM_EPD_WF_GLD16,
	DRM_EPD_WF_MAX
};

/**
 * enum drm_epd_lut_format - EPD LUT buffer format
 *
 * @DRM_EPD_LUT_4BIT: 4-bit grayscale indexes, 1 byte per element
 * @DRM_EPD_LUT_4BIT_PACKED: 4-bit grayscale indexes, 2 bits per element
 * @DRM_EPD_LUT_5BIT: 5-bit grayscale indexes, 1 byte per element
 * @DRM_EPD_LUT_5BIT_PACKED: 5-bit grayscale indexes, 2 bits per element
 */
enum drm_epd_lut_format {
	DRM_EPD_LUT_4BIT,
	DRM_EPD_LUT_4BIT_PACKED,
	DRM_EPD_LUT_5BIT,
	DRM_EPD_LUT_5BIT_PACKED,
};

/**
 * struct drm_epd_lut_file - Describes a file containing EPD LUTs
 *
 * @dev: The DRM device owning this LUT file
 * @fw: The firmware object holding the raw file contents
 * @header: Vendor-specific LUT file header
 * @mode_info: Vendor-specific information about available waveforms
 */
struct drm_epd_lut_file {
	struct drm_device			*dev;
	const struct firmware			*fw;
	const struct pvi_wbf_file_header	*header;
	const struct pvi_wbf_mode_info		*mode_info;
};

/**
 * struct drm_epd_lut - Describes a particular LUT buffer
 *
 * @buf: The LUT, in the format requested by the driver
 * @file: The file where this LUT was loaded from
 * @format: The LUT buffer format needed by the driver
 * @max_phases: The maximum number of waveform phases supported by the driver
 * @num_phases: The number of waveform phases in the current LUT
 * @mode_index: Private identifier for the current waveform
 * @temp_index: Private identifier for the current temperature
 */
struct drm_epd_lut {
	u8				*buf;
	const struct drm_epd_lut_file	*file;
	enum drm_epd_lut_format		format;
	unsigned int			max_phases;
	unsigned int			num_phases;
	int				mode_index;
	int				temp_index;
};

int drmm_epd_lut_file_init(struct drm_device *dev,
			   struct drm_epd_lut_file *file,
			   const char *file_name);

int drmm_epd_lut_init(struct drm_epd_lut_file *file,
		      struct drm_epd_lut *lut,
		      enum drm_epd_lut_format format,
		      unsigned int max_phases);

int drm_epd_lut_set_temperature(struct drm_epd_lut *lut,
				int temperature);
int drm_epd_lut_set_waveform(struct drm_epd_lut *lut,
			     enum drm_epd_waveform waveform);

#endif /* __DRM_EPD_HELPER_H__ */
