/*
 * Copyright (C) 2016 Broadcom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/**
 * DOC: VC4 firmware KMS module.
 *
 * As a hack to get us from the current closed source driver world
 * toward a totally open stack, implement KMS on top of the Raspberry
 * Pi's firmware display stack.
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include <linux/component.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/module.h>

#include <soc/bcm2835/raspberrypi-firmware.h>

#include "vc4_drv.h"
#include "vc4_regs.h"
#include "vc_image_types.h"

int fkms_max_refresh_rate = 85;
module_param(fkms_max_refresh_rate, int, 0644);
MODULE_PARM_DESC(fkms_max_refresh_rate, "Max supported refresh rate");

struct get_display_cfg {
	u32  max_pixel_clock[2];  //Max pixel clock for each display
};

struct vc4_fkms {
	struct get_display_cfg cfg;
	bool bcm2711;
};

#define PLANES_PER_CRTC		3

struct set_plane {
	u8 display;
	u8 plane_id;
	u8 vc_image_type;
	s8 layer;

	u16 width;
	u16 height;

	u16 pitch;
	u16 vpitch;

	u32 src_x;	/* 16p16 */
	u32 src_y;	/* 16p16 */

	u32 src_w;	/* 16p16 */
	u32 src_h;	/* 16p16 */

	s16 dst_x;
	s16 dst_y;

	u16 dst_w;
	u16 dst_h;

	u8 alpha;
	u8 num_planes;
	u8 is_vu;
	u8 color_encoding;

	u32 planes[4];  /* DMA address of each plane */

	u32 transform;
};

/* Values for the transform field */
#define TRANSFORM_NO_ROTATE	0
#define TRANSFORM_ROTATE_180	BIT(1)
#define TRANSFORM_FLIP_HRIZ	BIT(16)
#define TRANSFORM_FLIP_VERT	BIT(17)

struct mailbox_set_plane {
	struct rpi_firmware_property_tag_header tag;
	struct set_plane plane;
};

struct mailbox_blank_display {
	struct rpi_firmware_property_tag_header tag1;
	u32 display;
	struct rpi_firmware_property_tag_header tag2;
	u32 blank;
};

struct mailbox_display_pwr {
	struct rpi_firmware_property_tag_header tag1;
	u32 display;
	u32 state;
};

struct mailbox_get_edid {
	struct rpi_firmware_property_tag_header tag1;
	u32 block;
	u32 display_number;
	u8 edid[128];
};

struct set_timings {
	u8 display;
	u8 padding;
	u16 video_id_code;

	u32 clock;		/* in kHz */

	u16 hdisplay;
	u16 hsync_start;

	u16 hsync_end;
	u16 htotal;

	u16 hskew;
	u16 vdisplay;

	u16 vsync_start;
	u16 vsync_end;

	u16 vtotal;
	u16 vscan;

	u16 vrefresh;
	u16 padding2;

	u32 flags;
#define  TIMINGS_FLAGS_H_SYNC_POS	BIT(0)
#define  TIMINGS_FLAGS_H_SYNC_NEG	0
#define  TIMINGS_FLAGS_V_SYNC_POS	BIT(1)
#define  TIMINGS_FLAGS_V_SYNC_NEG	0
#define  TIMINGS_FLAGS_INTERLACE	BIT(2)

#define TIMINGS_FLAGS_ASPECT_MASK	GENMASK(7, 4)
#define TIMINGS_FLAGS_ASPECT_NONE	(0 << 4)
#define TIMINGS_FLAGS_ASPECT_4_3	(1 << 4)
#define TIMINGS_FLAGS_ASPECT_16_9	(2 << 4)
#define TIMINGS_FLAGS_ASPECT_64_27	(3 << 4)
#define TIMINGS_FLAGS_ASPECT_256_135	(4 << 4)

/* Limited range RGB flag. Not set corresponds to full range. */
#define TIMINGS_FLAGS_RGB_LIMITED	BIT(8)
/* DVI monitor, therefore disable infoframes. Not set corresponds to HDMI. */
#define TIMINGS_FLAGS_DVI		BIT(9)
/* Double clock */
#define TIMINGS_FLAGS_DBL_CLK		BIT(10)
};

struct mailbox_set_mode {
	struct rpi_firmware_property_tag_header tag1;
	struct set_timings timings;
};

static const struct vc_image_format {
	u32 drm;	/* DRM_FORMAT_* */
	u32 vc_image;	/* VC_IMAGE_* */
	u32 is_vu;
} vc_image_formats[] = {
	{
		.drm = DRM_FORMAT_XRGB8888,
		.vc_image = VC_IMAGE_XRGB8888,
	},
	{
		.drm = DRM_FORMAT_ARGB8888,
		.vc_image = VC_IMAGE_ARGB8888,
	},
/*
 *	FIXME: Need to resolve which DRM format goes to which vc_image format
 *	for the remaining RGBA and RGBX formats.
 *	{
 *		.drm = DRM_FORMAT_ABGR8888,
 *		.vc_image = VC_IMAGE_RGBA8888,
 *	},
 *	{
 *		.drm = DRM_FORMAT_XBGR8888,
 *		.vc_image = VC_IMAGE_RGBA8888,
 *	},
 */
	{
		.drm = DRM_FORMAT_RGB565,
		.vc_image = VC_IMAGE_RGB565,
	},
	{
		.drm = DRM_FORMAT_RGB888,
		.vc_image = VC_IMAGE_BGR888,
	},
	{
		.drm = DRM_FORMAT_BGR888,
		.vc_image = VC_IMAGE_RGB888,
	},
	{
		.drm = DRM_FORMAT_YUV422,
		.vc_image = VC_IMAGE_YUV422PLANAR,
	},
	{
		.drm = DRM_FORMAT_YUV420,
		.vc_image = VC_IMAGE_YUV420,
	},
	{
		.drm = DRM_FORMAT_YVU420,
		.vc_image = VC_IMAGE_YUV420,
		.is_vu = 1,
	},
	{
		.drm = DRM_FORMAT_NV12,
		.vc_image = VC_IMAGE_YUV420SP,
	},
	{
		.drm = DRM_FORMAT_NV21,
		.vc_image = VC_IMAGE_YUV420SP,
		.is_vu = 1,
	},
	{
		.drm = DRM_FORMAT_P030,
		.vc_image = VC_IMAGE_YUV10COL,
	},
};

static const struct vc_image_format *vc4_get_vc_image_fmt(u32 drm_format)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vc_image_formats); i++) {
		if (vc_image_formats[i].drm == drm_format)
			return &vc_image_formats[i];
	}

	return NULL;
}

/* The firmware delivers a vblank interrupt to us through the SMI
 * hardware, which has only this one register.
 */
#define SMICS 0x0
#define SMIDSW0 0x14
#define SMIDSW1 0x1C
#define SMICS_INTERRUPTS (BIT(9) | BIT(10) | BIT(11))

/* Flag to denote that the firmware is giving multiple display callbacks */
#define SMI_NEW 0xabcd0000

#define vc4_crtc vc4_kms_crtc
#define to_vc4_crtc to_vc4_kms_crtc
struct vc4_crtc {
	struct drm_crtc base;
	struct drm_encoder *encoder;
	struct drm_connector *connector;
	void __iomem *regs;

	struct drm_pending_vblank_event *event;
	bool vblank_enabled;
	u32 display_number;
	u32 display_type;
};

static inline struct vc4_crtc *to_vc4_crtc(struct drm_crtc *crtc)
{
	return container_of(crtc, struct vc4_crtc, base);
}

struct fkms_crtc_state {
	struct drm_crtc_state base;

	struct {
		unsigned int left;
		unsigned int right;
		unsigned int top;
		unsigned int bottom;
	} margins;
};

static inline struct fkms_crtc_state *
to_fkms_crtc_state(struct drm_crtc_state *crtc_state)
{
	return (struct fkms_crtc_state *)crtc_state;
}

struct vc4_fkms_encoder {
	struct drm_encoder base;
	bool hdmi_monitor;
	bool rgb_range_selectable;
	int display_num;
};

static inline struct vc4_fkms_encoder *
to_vc4_fkms_encoder(struct drm_encoder *encoder)
{
	return container_of(encoder, struct vc4_fkms_encoder, base);
}

/* "Broadcast RGB" property.
 * Allows overriding of HDMI full or limited range RGB
 */
#define VC4_BROADCAST_RGB_AUTO 0
#define VC4_BROADCAST_RGB_FULL 1
#define VC4_BROADCAST_RGB_LIMITED 2

/* VC4 FKMS connector KMS struct */
struct vc4_fkms_connector {
	struct drm_connector base;

	/* Since the connector is attached to just the one encoder,
	 * this is the reference to it so we can do the best_encoder()
	 * hook.
	 */
	struct drm_encoder *encoder;
	struct vc4_dev *vc4_dev;
	u32 display_number;
	u32 display_type;

	struct drm_property *broadcast_rgb_property;
};

static inline struct vc4_fkms_connector *
to_vc4_fkms_connector(struct drm_connector *connector)
{
	return container_of(connector, struct vc4_fkms_connector, base);
}

/* VC4 FKMS connector state */
struct vc4_fkms_connector_state {
	struct drm_connector_state base;

	int broadcast_rgb;
};

#define to_vc4_fkms_connector_state(x) \
			container_of(x, struct vc4_fkms_connector_state, base)

static u32 vc4_get_display_type(u32 display_number)
{
	const u32 display_types[] = {
		/* The firmware display (DispmanX) IDs map to specific types in
		 * a fixed manner.
		 */
		DRM_MODE_ENCODER_DSI,	/* MAIN_LCD - DSI or DPI */
		DRM_MODE_ENCODER_DSI,	/* AUX_LCD */
		DRM_MODE_ENCODER_TMDS,	/* HDMI0 */
		DRM_MODE_ENCODER_TVDAC,	/* VEC */
		DRM_MODE_ENCODER_NONE,	/* FORCE_LCD */
		DRM_MODE_ENCODER_NONE,	/* FORCE_TV */
		DRM_MODE_ENCODER_NONE,	/* FORCE_OTHER */
		DRM_MODE_ENCODER_TMDS,	/* HDMI1 */
		DRM_MODE_ENCODER_NONE,	/* FORCE_TV2 */
	};
	return display_number > ARRAY_SIZE(display_types) - 1 ?
			DRM_MODE_ENCODER_NONE : display_types[display_number];
}

/* Firmware's structure for making an FB mbox call. */
struct fbinfo_s {
	u32 xres, yres, xres_virtual, yres_virtual;
	u32 pitch, bpp;
	u32 xoffset, yoffset;
	u32 base;
	u32 screen_size;
	u16 cmap[256];
};

struct vc4_fkms_plane {
	struct drm_plane base;
	struct fbinfo_s *fbinfo;
	dma_addr_t fbinfo_bus_addr;
	u32 pitch;
	struct mailbox_set_plane mb;
};

static inline struct vc4_fkms_plane *to_vc4_fkms_plane(struct drm_plane *plane)
{
	return (struct vc4_fkms_plane *)plane;
}

static int vc4_plane_set_blank(struct drm_plane *plane, bool blank)
{
	struct vc4_dev *vc4 = to_vc4_dev(plane->dev);
	struct vc4_fkms_plane *vc4_plane = to_vc4_fkms_plane(plane);
	struct mailbox_set_plane blank_mb = {
		.tag = { RPI_FIRMWARE_SET_PLANE, sizeof(struct set_plane), 0 },
		.plane = {
			.display = vc4_plane->mb.plane.display,
			.plane_id = vc4_plane->mb.plane.plane_id,
		}
	};
	static const char * const plane_types[] = {
							"overlay",
							"primary",
							"cursor"
						  };
	int ret;

	DRM_DEBUG_ATOMIC("[PLANE:%d:%s] %s plane %s",
			 plane->base.id, plane->name, plane_types[plane->type],
			 blank ? "blank" : "unblank");

	if (blank)
		ret = rpi_firmware_property_list(vc4->firmware, &blank_mb,
						 sizeof(blank_mb));
	else
		ret = rpi_firmware_property_list(vc4->firmware, &vc4_plane->mb,
						 sizeof(vc4_plane->mb));

	WARN_ONCE(ret, "%s: firmware call failed. Please update your firmware",
		  __func__);
	return ret;
}

static void vc4_fkms_crtc_get_margins(struct drm_crtc_state *state,
				      unsigned int *left, unsigned int *right,
				      unsigned int *top, unsigned int *bottom)
{
	struct fkms_crtc_state *vc4_state = to_fkms_crtc_state(state);
	struct drm_connector_state *conn_state;
	struct drm_connector *conn;
	int i;

	*left = vc4_state->margins.left;
	*right = vc4_state->margins.right;
	*top = vc4_state->margins.top;
	*bottom = vc4_state->margins.bottom;

	/* We have to interate over all new connector states because
	 * vc4_fkms_crtc_get_margins() might be called before
	 * vc4_fkms_crtc_atomic_check() which means margins info in
	 * fkms_crtc_state might be outdated.
	 */
	for_each_new_connector_in_state(state->state, conn, conn_state, i) {
		if (conn_state->crtc != state->crtc)
			continue;

		*left = conn_state->tv.margins.left;
		*right = conn_state->tv.margins.right;
		*top = conn_state->tv.margins.top;
		*bottom = conn_state->tv.margins.bottom;
		break;
	}
}

static int vc4_fkms_margins_adj(struct drm_plane_state *pstate,
				struct set_plane *plane)
{
	unsigned int left, right, top, bottom;
	int adjhdisplay, adjvdisplay;
	struct drm_crtc_state *crtc_state;

	crtc_state = drm_atomic_get_new_crtc_state(pstate->state,
						   pstate->crtc);

	vc4_fkms_crtc_get_margins(crtc_state, &left, &right, &top, &bottom);

	if (!left && !right && !top && !bottom)
		return 0;

	if (left + right >= crtc_state->mode.hdisplay ||
	    top + bottom >= crtc_state->mode.vdisplay)
		return -EINVAL;

	adjhdisplay = crtc_state->mode.hdisplay - (left + right);
	plane->dst_x = DIV_ROUND_CLOSEST(plane->dst_x * adjhdisplay,
					 (int)crtc_state->mode.hdisplay);
	plane->dst_x += left;
	if (plane->dst_x > (int)(crtc_state->mode.hdisplay - left))
		plane->dst_x = crtc_state->mode.hdisplay - left;

	adjvdisplay = crtc_state->mode.vdisplay - (top + bottom);
	plane->dst_y = DIV_ROUND_CLOSEST(plane->dst_y * adjvdisplay,
					 (int)crtc_state->mode.vdisplay);
	plane->dst_y += top;
	if (plane->dst_y > (int)(crtc_state->mode.vdisplay - top))
		plane->dst_y = crtc_state->mode.vdisplay - top;

	plane->dst_w = DIV_ROUND_CLOSEST(plane->dst_w * adjhdisplay,
					 crtc_state->mode.hdisplay);
	plane->dst_h = DIV_ROUND_CLOSEST(plane->dst_h * adjvdisplay,
					 crtc_state->mode.vdisplay);

	if (!plane->dst_w || !plane->dst_h)
		return -EINVAL;

	return 0;
}

static void vc4_plane_atomic_update(struct drm_plane *plane,
				    struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = plane->state;

	/*
	 * Do NOT set now, as we haven't checked if the crtc is active or not.
	 * Set from vc4_plane_set_blank instead.
	 *
	 * If the CRTC is on (or going to be on) and we're enabled,
	 * then unblank.  Otherwise, stay blank until CRTC enable.
	 */
	if (state->crtc->state->active)
		vc4_plane_set_blank(plane, false);
}

static void vc4_plane_atomic_disable(struct drm_plane *plane,
				     struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = plane->state;
	struct vc4_fkms_plane *vc4_plane = to_vc4_fkms_plane(plane);

	DRM_DEBUG_ATOMIC("[PLANE:%d:%s] plane disable %dx%d@%d +%d,%d\n",
			 plane->base.id, plane->name,
			 state->crtc_w,
			 state->crtc_h,
			 vc4_plane->mb.plane.vc_image_type,
			 state->crtc_x,
			 state->crtc_y);
	vc4_plane_set_blank(plane, true);
}

static bool plane_enabled(struct drm_plane_state *state)
{
	return state->fb && state->crtc;
}

static int vc4_plane_to_mb(struct drm_plane *plane,
			   struct mailbox_set_plane *mb,
			   struct drm_plane_state *state)
{
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_cma_object *bo = drm_fb_cma_get_gem_obj(fb, 0);
	const struct drm_format_info *drm_fmt = fb->format;
	const struct vc_image_format *vc_fmt =
					vc4_get_vc_image_fmt(drm_fmt->format);
	int num_planes = fb->format->num_planes;
	unsigned int rotation;

	mb->plane.vc_image_type = vc_fmt->vc_image;
	mb->plane.width = fb->width;
	mb->plane.height = fb->height;
	mb->plane.pitch = fb->pitches[0];
	mb->plane.src_w = state->src_w;
	mb->plane.src_h = state->src_h;
	mb->plane.src_x = state->src_x;
	mb->plane.src_y = state->src_y;
	mb->plane.dst_w = state->crtc_w;
	mb->plane.dst_h = state->crtc_h;
	mb->plane.dst_x = state->crtc_x;
	mb->plane.dst_y = state->crtc_y;
	mb->plane.alpha = state->alpha >> 8;
	mb->plane.layer = state->normalized_zpos ?
					state->normalized_zpos : -127;
	mb->plane.num_planes = num_planes;
	mb->plane.is_vu = vc_fmt->is_vu;
	mb->plane.planes[0] = bo->paddr + fb->offsets[0];

	rotation = drm_rotation_simplify(state->rotation,
					 DRM_MODE_ROTATE_0 |
					 DRM_MODE_REFLECT_X |
					 DRM_MODE_REFLECT_Y);

	mb->plane.transform = TRANSFORM_NO_ROTATE;
	if (rotation & DRM_MODE_REFLECT_X)
		mb->plane.transform |= TRANSFORM_FLIP_HRIZ;
	if (rotation & DRM_MODE_REFLECT_Y)
		mb->plane.transform |= TRANSFORM_FLIP_VERT;

	vc4_fkms_margins_adj(state, &mb->plane);

	if (num_planes > 1) {
		/* Assume this must be YUV */
		/* Makes assumptions on the stride for the chroma planes as we
		 * can't easily plumb in non-standard pitches.
		 */
		mb->plane.planes[1] = bo->paddr + fb->offsets[1];
		if (num_planes > 2)
			mb->plane.planes[2] = bo->paddr + fb->offsets[2];
		else
			mb->plane.planes[2] = 0;

		/* Special case the YUV420 with U and V as line interleaved
		 * planes as we have special handling for that case.
		 */
		if (num_planes == 3 &&
		    (fb->offsets[2] - fb->offsets[1]) == fb->pitches[1])
			mb->plane.vc_image_type = VC_IMAGE_YUV420_S;

		switch (state->color_encoding) {
		default:
		case DRM_COLOR_YCBCR_BT601:
			if (state->color_range == DRM_COLOR_YCBCR_LIMITED_RANGE)
				mb->plane.color_encoding =
						VC_IMAGE_YUVINFO_CSC_ITUR_BT601;
			else
				mb->plane.color_encoding =
						VC_IMAGE_YUVINFO_CSC_JPEG_JFIF;
			break;
		case DRM_COLOR_YCBCR_BT709:
			/* Currently no support for a full range BT709 */
			mb->plane.color_encoding =
						VC_IMAGE_YUVINFO_CSC_ITUR_BT709;
			break;
		case DRM_COLOR_YCBCR_BT2020:
			/* Currently no support for a full range BT2020 */
			mb->plane.color_encoding =
					VC_IMAGE_YUVINFO_CSC_REC_2020;
			break;
		}
	} else {
		mb->plane.planes[1] = 0;
		mb->plane.planes[2] = 0;
	}
	mb->plane.planes[3] = 0;

	switch (fourcc_mod_broadcom_mod(fb->modifier)) {
	case DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED:
		switch (mb->plane.vc_image_type) {
		case VC_IMAGE_XRGB8888:
			mb->plane.vc_image_type = VC_IMAGE_TF_RGBX32;
			break;
		case VC_IMAGE_ARGB8888:
			mb->plane.vc_image_type = VC_IMAGE_TF_RGBA32;
			break;
		case VC_IMAGE_RGB565:
			mb->plane.vc_image_type = VC_IMAGE_TF_RGB565;
			break;
		}
		break;
	case DRM_FORMAT_MOD_BROADCOM_SAND128:
		switch (mb->plane.vc_image_type) {
		case VC_IMAGE_YUV420SP:
			mb->plane.vc_image_type = VC_IMAGE_YUV_UV;
			break;
		/* VC_IMAGE_YUV10COL could be included in here, but it is only
		 * valid as a SAND128 format, so the table at the top will have
		 * already set the correct format.
		 */
		}
		/* Note that the column pitch is passed across in lines, not
		 * bytes.
		 */
		mb->plane.pitch = fourcc_mod_broadcom_param(fb->modifier);
		break;
	}

	DRM_DEBUG_ATOMIC("[PLANE:%d:%s] plane update %dx%d@%d +dst(%d,%d, %d,%d) +src(%d,%d, %d,%d) 0x%08x/%08x/%08x/%d, alpha %u zpos %u\n",
			 plane->base.id, plane->name,
			 mb->plane.width,
			 mb->plane.height,
			 mb->plane.vc_image_type,
			 state->crtc_x,
			 state->crtc_y,
			 state->crtc_w,
			 state->crtc_h,
			 mb->plane.src_x,
			 mb->plane.src_y,
			 mb->plane.src_w,
			 mb->plane.src_h,
			 mb->plane.planes[0],
			 mb->plane.planes[1],
			 mb->plane.planes[2],
			 fb->pitches[0],
			 state->alpha,
			 state->normalized_zpos);

	return 0;
}

static int vc4_plane_atomic_check(struct drm_plane *plane,
				  struct drm_plane_state *state)
{
	struct vc4_fkms_plane *vc4_plane = to_vc4_fkms_plane(plane);

	if (!plane_enabled(state))
		return 0;

	return vc4_plane_to_mb(plane, &vc4_plane->mb, state);

}

/* Called during init to allocate the plane's atomic state. */
static void vc4_plane_reset(struct drm_plane *plane)
{
	struct vc4_plane_state *vc4_state;

	WARN_ON(plane->state);

	vc4_state = kzalloc(sizeof(*vc4_state), GFP_KERNEL);
	if (!vc4_state)
		return;

	__drm_atomic_helper_plane_reset(plane, &vc4_state->base);
}

static void vc4_plane_destroy(struct drm_plane *plane)
{
	drm_plane_cleanup(plane);
}

static bool vc4_fkms_format_mod_supported(struct drm_plane *plane,
					  uint32_t format,
					  uint64_t modifier)
{
	/* Support T_TILING for RGB formats only. */
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_RGB565:
		switch (modifier) {
		case DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED:
		case DRM_FORMAT_MOD_LINEAR:
			return true;
		default:
			return false;
		}
	case DRM_FORMAT_NV12:
		switch (fourcc_mod_broadcom_mod(modifier)) {
		case DRM_FORMAT_MOD_LINEAR:
		case DRM_FORMAT_MOD_BROADCOM_SAND128:
			return true;
		default:
			return false;
		}
	case DRM_FORMAT_P030:
		switch (fourcc_mod_broadcom_mod(modifier)) {
		case DRM_FORMAT_MOD_BROADCOM_SAND128:
			return true;
		default:
			return false;
		}
	case DRM_FORMAT_NV21:
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_BGR888:
	case DRM_FORMAT_YUV422:
	case DRM_FORMAT_YUV420:
	case DRM_FORMAT_YVU420:
	default:
		return (modifier == DRM_FORMAT_MOD_LINEAR);
	}
}

static struct drm_plane_state *vc4_plane_duplicate_state(struct drm_plane *plane)
{
	struct vc4_plane_state *vc4_state;

	if (WARN_ON(!plane->state))
		return NULL;

	vc4_state = kzalloc(sizeof(*vc4_state), GFP_KERNEL);
	if (!vc4_state)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &vc4_state->base);

	return &vc4_state->base;
}

static const struct drm_plane_funcs vc4_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = vc4_plane_destroy,
	.set_property = NULL,
	.reset = vc4_plane_reset,
	.atomic_duplicate_state = vc4_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
	.format_mod_supported = vc4_fkms_format_mod_supported,
};

static const struct drm_plane_helper_funcs vc4_plane_helper_funcs = {
	.prepare_fb = drm_gem_fb_prepare_fb,
	.cleanup_fb = NULL,
	.atomic_check = vc4_plane_atomic_check,
	.atomic_update = vc4_plane_atomic_update,
	.atomic_disable = vc4_plane_atomic_disable,
};

static struct drm_plane *vc4_fkms_plane_init(struct drm_device *dev,
					     enum drm_plane_type type,
					     u8 display_num,
					     u8 plane_id)
{
	struct drm_plane *plane = NULL;
	struct vc4_fkms_plane *vc4_plane;
	u32 formats[ARRAY_SIZE(vc_image_formats)];
	unsigned int default_zpos = 0;
	u32 num_formats = 0;
	int ret = 0;
	static const uint64_t modifiers[] = {
		DRM_FORMAT_MOD_LINEAR,
		/* VC4_T_TILED should come after linear, because we
		 * would prefer to scan out linear (less bus traffic).
		 */
		DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED,
		DRM_FORMAT_MOD_BROADCOM_SAND128,
		DRM_FORMAT_MOD_INVALID,
	};
	int i;

	vc4_plane = devm_kzalloc(dev->dev, sizeof(*vc4_plane),
				 GFP_KERNEL);
	if (!vc4_plane) {
		ret = -ENOMEM;
		goto fail;
	}

	for (i = 0; i < ARRAY_SIZE(vc_image_formats); i++)
		formats[num_formats++] = vc_image_formats[i].drm;

	plane = &vc4_plane->base;
	ret = drm_universal_plane_init(dev, plane, 0xff,
				       &vc4_plane_funcs,
				       formats, num_formats, modifiers,
				       type, NULL);

	/* FIXME: Do we need to be checking return values from all these calls?
	 */
	drm_plane_helper_add(plane, &vc4_plane_helper_funcs);

	drm_plane_create_alpha_property(plane);
	drm_plane_create_rotation_property(plane, DRM_MODE_ROTATE_0,
					   DRM_MODE_ROTATE_0 |
					   DRM_MODE_ROTATE_180 |
					   DRM_MODE_REFLECT_X |
					   DRM_MODE_REFLECT_Y);
	drm_plane_create_color_properties(plane,
					  BIT(DRM_COLOR_YCBCR_BT601) |
					  BIT(DRM_COLOR_YCBCR_BT709) |
					  BIT(DRM_COLOR_YCBCR_BT2020),
					  BIT(DRM_COLOR_YCBCR_LIMITED_RANGE) |
					  BIT(DRM_COLOR_YCBCR_FULL_RANGE),
					  DRM_COLOR_YCBCR_BT709,
					  DRM_COLOR_YCBCR_LIMITED_RANGE);

	/*
	 * Default frame buffer setup is with FB on -127, and raspistill etc
	 * tend to drop overlays on layer 2. Cursor plane was on layer +127.
	 *
	 * For F-KMS the mailbox call allows for a s8.
	 * Remap zpos 0 to -127 for the background layer, but leave all the
	 * other layers as requested by KMS.
	 */
	switch (type) {
	default:
	case DRM_PLANE_TYPE_PRIMARY:
		default_zpos = 0;
		break;
	case DRM_PLANE_TYPE_OVERLAY:
		default_zpos = 1;
		break;
	case DRM_PLANE_TYPE_CURSOR:
		default_zpos = 2;
		break;
	}
	drm_plane_create_zpos_property(plane, default_zpos, 0, 127);

	/* Prepare the static elements of the mailbox structure */
	vc4_plane->mb.tag.tag = RPI_FIRMWARE_SET_PLANE;
	vc4_plane->mb.tag.buf_size = sizeof(struct set_plane);
	vc4_plane->mb.tag.req_resp_size = 0;
	vc4_plane->mb.plane.display = display_num;
	vc4_plane->mb.plane.plane_id = plane_id;
	vc4_plane->mb.plane.layer = default_zpos ? default_zpos : -127;

	return plane;
fail:
	if (plane)
		vc4_plane_destroy(plane);

	return ERR_PTR(ret);
}

static void vc4_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	struct vc4_fkms_encoder *vc4_encoder =
					to_vc4_fkms_encoder(vc4_crtc->encoder);
	struct mailbox_set_mode mb = {
		.tag1 = { RPI_FIRMWARE_SET_TIMING,
			  sizeof(struct set_timings), 0},
	};
	union hdmi_infoframe frame;
	int ret;

	ret = drm_hdmi_avi_infoframe_from_display_mode(&frame.avi, vc4_crtc->connector, mode);
	if (ret < 0) {
		DRM_ERROR("couldn't fill AVI infoframe\n");
		return;
	}

	DRM_DEBUG_KMS("Setting mode for display num %u mode name %s, clk %d, h(disp %d, start %d, end %d, total %d, skew %d) v(disp %d, start %d, end %d, total %d, scan %d), vrefresh %d, par %u, flags 0x%04x\n",
		      vc4_crtc->display_number, mode->name, mode->clock,
		      mode->hdisplay, mode->hsync_start, mode->hsync_end,
		      mode->htotal, mode->hskew, mode->vdisplay,
		      mode->vsync_start, mode->vsync_end, mode->vtotal,
		      mode->vscan, drm_mode_vrefresh(mode),
		      mode->picture_aspect_ratio, mode->flags);
	mb.timings.display = vc4_crtc->display_number;

	mb.timings.clock = mode->clock;
	mb.timings.hdisplay = mode->hdisplay;
	mb.timings.hsync_start = mode->hsync_start;
	mb.timings.hsync_end = mode->hsync_end;
	mb.timings.htotal = mode->htotal;
	mb.timings.hskew = mode->hskew;
	mb.timings.vdisplay = mode->vdisplay;
	mb.timings.vsync_start = mode->vsync_start;
	mb.timings.vsync_end = mode->vsync_end;
	mb.timings.vtotal = mode->vtotal;
	mb.timings.vscan = mode->vscan;
	mb.timings.vrefresh = drm_mode_vrefresh(mode);
	mb.timings.flags = 0;
	if (mode->flags & DRM_MODE_FLAG_PHSYNC)
		mb.timings.flags |= TIMINGS_FLAGS_H_SYNC_POS;
	if (mode->flags & DRM_MODE_FLAG_PVSYNC)
		mb.timings.flags |= TIMINGS_FLAGS_V_SYNC_POS;

	switch (frame.avi.picture_aspect) {
	default:
	case HDMI_PICTURE_ASPECT_NONE:
		mb.timings.flags |= TIMINGS_FLAGS_ASPECT_NONE;
		break;
	case HDMI_PICTURE_ASPECT_4_3:
		mb.timings.flags |= TIMINGS_FLAGS_ASPECT_4_3;
		break;
	case HDMI_PICTURE_ASPECT_16_9:
		mb.timings.flags |= TIMINGS_FLAGS_ASPECT_16_9;
		break;
	case HDMI_PICTURE_ASPECT_64_27:
		mb.timings.flags |= TIMINGS_FLAGS_ASPECT_64_27;
		break;
	case HDMI_PICTURE_ASPECT_256_135:
		mb.timings.flags |= TIMINGS_FLAGS_ASPECT_256_135;
		break;
	}

	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		mb.timings.flags |= TIMINGS_FLAGS_INTERLACE;
	if (mode->flags & DRM_MODE_FLAG_DBLCLK)
		mb.timings.flags |= TIMINGS_FLAGS_DBL_CLK;

	mb.timings.video_id_code = frame.avi.video_code;

	if (!vc4_encoder->hdmi_monitor) {
		mb.timings.flags |= TIMINGS_FLAGS_DVI;
	} else {
		struct vc4_fkms_connector_state *conn_state =
			to_vc4_fkms_connector_state(vc4_crtc->connector->state);

		if (conn_state->broadcast_rgb == VC4_BROADCAST_RGB_AUTO) {
			/* See CEA-861-E - 5.1 Default Encoding Parameters */
			if (drm_default_rgb_quant_range(mode) ==
					HDMI_QUANTIZATION_RANGE_LIMITED)
				mb.timings.flags |= TIMINGS_FLAGS_RGB_LIMITED;
		} else {
			if (conn_state->broadcast_rgb ==
						VC4_BROADCAST_RGB_LIMITED)
				mb.timings.flags |= TIMINGS_FLAGS_RGB_LIMITED;

			/* If not using the default range, then do not provide
			 * a VIC as the HDMI spec requires that we do not
			 * signal the opposite of the defined range in the AVI
			 * infoframe.
			 */
			if (!!(mb.timings.flags & TIMINGS_FLAGS_RGB_LIMITED) !=
			    (drm_default_rgb_quant_range(mode) ==
					HDMI_QUANTIZATION_RANGE_LIMITED))
				mb.timings.video_id_code = 0;
		}
	}

	/*
	FIXME: To implement
	switch(mode->flag & DRM_MODE_FLAG_3D_MASK) {
	case DRM_MODE_FLAG_3D_NONE:
	case DRM_MODE_FLAG_3D_FRAME_PACKING:
	case DRM_MODE_FLAG_3D_FIELD_ALTERNATIVE:
	case DRM_MODE_FLAG_3D_LINE_ALTERNATIVE:
	case DRM_MODE_FLAG_3D_SIDE_BY_SIDE_FULL:
	case DRM_MODE_FLAG_3D_L_DEPTH:
	case DRM_MODE_FLAG_3D_L_DEPTH_GFX_GFX_DEPTH:
	case DRM_MODE_FLAG_3D_TOP_AND_BOTTOM:
	case DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF:
	}
	*/

	ret = rpi_firmware_property_list(vc4->firmware, &mb, sizeof(mb));
}

static void vc4_crtc_disable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	struct drm_device *dev = crtc->dev;
	struct drm_plane *plane;

	DRM_DEBUG_KMS("[CRTC:%d] vblanks off.\n",
		      crtc->base.id);
	drm_crtc_vblank_off(crtc);

	/* Always turn the planes off on CRTC disable. In DRM, planes
	 * are enabled/disabled through the update/disable hooks
	 * above, and the CRTC enable/disable independently controls
	 * whether anything scans out at all, but the firmware doesn't
	 * give us a CRTC-level control for that.
	 */

	drm_atomic_crtc_for_each_plane(plane, crtc)
		vc4_plane_atomic_disable(plane, plane->state);

	/*
	 * Make sure we issue a vblank event after disabling the CRTC if
	 * someone was waiting it.
	 */
	if (crtc->state->event) {
		unsigned long flags;

		spin_lock_irqsave(&dev->event_lock, flags);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
		spin_unlock_irqrestore(&dev->event_lock, flags);
	}
}

static void vc4_crtc_consume_event(struct drm_crtc *crtc)
{
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct drm_device *dev = crtc->dev;
	unsigned long flags;

	if (!crtc->state->event)
		return;

	crtc->state->event->pipe = drm_crtc_index(crtc);

	WARN_ON(drm_crtc_vblank_get(crtc) != 0);

	spin_lock_irqsave(&dev->event_lock, flags);
	vc4_crtc->event = crtc->state->event;
	crtc->state->event = NULL;
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

static void vc4_crtc_enable(struct drm_crtc *crtc, struct drm_crtc_state *old_state)
{
	struct drm_plane *plane;

	DRM_DEBUG_KMS("[CRTC:%d] vblanks on.\n",
		      crtc->base.id);
	drm_crtc_vblank_on(crtc);
	vc4_crtc_consume_event(crtc);

	/* Unblank the planes (if they're supposed to be displayed). */
	drm_atomic_crtc_for_each_plane(plane, crtc)
		if (plane->state->fb)
			vc4_plane_set_blank(plane, plane->state->visible);
}

static enum drm_mode_status
vc4_crtc_mode_valid(struct drm_crtc *crtc, const struct drm_display_mode *mode)
{
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
	struct drm_device *dev = crtc->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_fkms *fkms = vc4->fkms;

	/* Do not allow doublescan modes from user space */
	if (mode->flags & DRM_MODE_FLAG_DBLSCAN) {
		DRM_DEBUG_KMS("[CRTC:%d] Doublescan mode rejected.\n",
			      crtc->base.id);
		return MODE_NO_DBLESCAN;
	}

	/* Disable refresh rates > defined threshold (default 85Hz) as limited
	 * gain from them
	 */
	if (drm_mode_vrefresh(mode) > fkms_max_refresh_rate)
		return MODE_BAD_VVALUE;

	/* Limit the pixel clock based on the HDMI clock limits from the
	 * firmware
	 */
	switch (vc4_crtc->display_number) {
	case 2:	/* HDMI0 */
		if (fkms->cfg.max_pixel_clock[0] &&
		    mode->clock > fkms->cfg.max_pixel_clock[0])
			return MODE_CLOCK_HIGH;
		break;
	case 7:	/* HDMI1 */
		if (fkms->cfg.max_pixel_clock[1] &&
		    mode->clock > fkms->cfg.max_pixel_clock[1])
			return MODE_CLOCK_HIGH;
		break;
	}

	/* Pi4 can't generate odd horizontal timings on HDMI, so reject modes
	 * that would set them.
	 */
	if (fkms->bcm2711 &&
	    (vc4_crtc->display_number == 2 || vc4_crtc->display_number == 7) &&
	    !(mode->flags & DRM_MODE_FLAG_DBLCLK) &&
	    ((mode->hdisplay |				/* active */
	      (mode->hsync_start - mode->hdisplay) |	/* front porch */
	      (mode->hsync_end - mode->hsync_start) |	/* sync pulse */
	      (mode->htotal - mode->hsync_end)) & 1))	/* back porch */ {
		DRM_DEBUG_KMS("[CRTC:%d] Odd timing rejected %u %u %u %u.\n",
			      crtc->base.id, mode->hdisplay, mode->hsync_start,
			      mode->hsync_end, mode->htotal);
		return MODE_H_ILLEGAL;
	}

	return MODE_OK;
}

static int vc4_crtc_atomic_check(struct drm_crtc *crtc,
				 struct drm_crtc_state *state)
{
	struct fkms_crtc_state *vc4_state = to_fkms_crtc_state(state);
	struct drm_connector *conn;
	struct drm_connector_state *conn_state;
	int i;

	DRM_DEBUG_KMS("[CRTC:%d] crtc_atomic_check.\n", crtc->base.id);

	for_each_new_connector_in_state(state->state, conn, conn_state, i) {
		if (conn_state->crtc != crtc)
			continue;

		vc4_state->margins.left = conn_state->tv.margins.left;
		vc4_state->margins.right = conn_state->tv.margins.right;
		vc4_state->margins.top = conn_state->tv.margins.top;
		vc4_state->margins.bottom = conn_state->tv.margins.bottom;
		break;
	}
	return 0;
}

static void vc4_crtc_atomic_flush(struct drm_crtc *crtc,
				  struct drm_crtc_state *old_state)
{
	DRM_DEBUG_KMS("[CRTC:%d] crtc_atomic_flush.\n",
		      crtc->base.id);
	if (crtc->state->active && old_state->active && crtc->state->event)
		vc4_crtc_consume_event(crtc);
}

static void vc4_crtc_handle_page_flip(struct vc4_crtc *vc4_crtc)
{
	struct drm_crtc *crtc = &vc4_crtc->base;
	struct drm_device *dev = crtc->dev;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	if (vc4_crtc->event) {
		drm_crtc_send_vblank_event(crtc, vc4_crtc->event);
		vc4_crtc->event = NULL;
		drm_crtc_vblank_put(crtc);
	}
	spin_unlock_irqrestore(&dev->event_lock, flags);
}

static irqreturn_t vc4_crtc_irq_handler(int irq, void *data)
{
	struct vc4_crtc **crtc_list = data;
	int i;
	u32 stat = readl(crtc_list[0]->regs + SMICS);
	irqreturn_t ret = IRQ_NONE;
	u32 chan;

	if (stat & SMICS_INTERRUPTS) {
		writel(0, crtc_list[0]->regs + SMICS);

		chan = readl(crtc_list[0]->regs + SMIDSW0);

		if ((chan & 0xFFFF0000) != SMI_NEW) {
			/* Older firmware. Treat the one interrupt as vblank/
			 * complete for all crtcs.
			 */
			for (i = 0; crtc_list[i]; i++) {
				if (crtc_list[i]->vblank_enabled)
					drm_crtc_handle_vblank(&crtc_list[i]->base);
				vc4_crtc_handle_page_flip(crtc_list[i]);
			}
		} else {
			if (chan & 1) {
				writel(SMI_NEW, crtc_list[0]->regs + SMIDSW0);
				if (crtc_list[0]->vblank_enabled)
					drm_crtc_handle_vblank(&crtc_list[0]->base);
				vc4_crtc_handle_page_flip(crtc_list[0]);
			}

			if (crtc_list[1]) {
				/* Check for the secondary display too */
				chan = readl(crtc_list[0]->regs + SMIDSW1);

				if (chan & 1) {
					writel(SMI_NEW, crtc_list[0]->regs + SMIDSW1);

					if (crtc_list[1]->vblank_enabled)
						drm_crtc_handle_vblank(&crtc_list[1]->base);
					vc4_crtc_handle_page_flip(crtc_list[1]);
				}
			}
		}

		ret = IRQ_HANDLED;
	}

	return ret;
}

static int vc4_page_flip(struct drm_crtc *crtc,
			 struct drm_framebuffer *fb,
			 struct drm_pending_vblank_event *event,
			 uint32_t flags, struct drm_modeset_acquire_ctx *ctx)
{
	if (flags & DRM_MODE_PAGE_FLIP_ASYNC) {
		DRM_ERROR("Async flips aren't allowed\n");
		return -EINVAL;
	}

	return drm_atomic_helper_page_flip(crtc, fb, event, flags, ctx);
}

static struct drm_crtc_state *
vc4_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct fkms_crtc_state *vc4_state, *old_vc4_state;

	vc4_state = kzalloc(sizeof(*vc4_state), GFP_KERNEL);
	if (!vc4_state)
		return NULL;

	old_vc4_state = to_fkms_crtc_state(crtc->state);
	vc4_state->margins = old_vc4_state->margins;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &vc4_state->base);
	return &vc4_state->base;
}

static void
vc4_crtc_reset(struct drm_crtc *crtc)
{
	if (crtc->state)
		__drm_atomic_helper_crtc_destroy_state(crtc->state);

	crtc->state = kzalloc(sizeof(*crtc->state), GFP_KERNEL);
	if (crtc->state)
		crtc->state->crtc = crtc;
}

static int vc4_fkms_enable_vblank(struct drm_crtc *crtc)
{
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);

	DRM_DEBUG_KMS("[CRTC:%d] enable_vblank.\n",
		      crtc->base.id);
	vc4_crtc->vblank_enabled = true;

	return 0;
}

static void vc4_fkms_disable_vblank(struct drm_crtc *crtc)
{
	struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);

	DRM_DEBUG_KMS("[CRTC:%d] disable_vblank.\n",
		      crtc->base.id);
	vc4_crtc->vblank_enabled = false;
}

static const struct drm_crtc_funcs vc4_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.destroy = drm_crtc_cleanup,
	.page_flip = vc4_page_flip,
	.set_property = NULL,
	.cursor_set = NULL, /* handled by drm_mode_cursor_universal */
	.cursor_move = NULL, /* handled by drm_mode_cursor_universal */
	.reset = vc4_crtc_reset,
	.atomic_duplicate_state = vc4_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = vc4_fkms_enable_vblank,
	.disable_vblank = vc4_fkms_disable_vblank,
};

static const struct drm_crtc_helper_funcs vc4_crtc_helper_funcs = {
	.mode_set_nofb = vc4_crtc_mode_set_nofb,
	.mode_valid = vc4_crtc_mode_valid,
	.atomic_check = vc4_crtc_atomic_check,
	.atomic_flush = vc4_crtc_atomic_flush,
	.atomic_enable = vc4_crtc_enable,
	.atomic_disable = vc4_crtc_disable,
};

static const struct of_device_id vc4_firmware_kms_dt_match[] = {
	{ .compatible = "raspberrypi,rpi-firmware-kms" },
	{ .compatible = "raspberrypi,rpi-firmware-kms-2711",
	  .data = (void *)1 },
	{}
};

static enum drm_connector_status
vc4_fkms_connector_detect(struct drm_connector *connector, bool force)
{
	DRM_DEBUG_KMS("connector detect.\n");
	return connector_status_connected;
}

/* Queries the firmware to populate a drm_mode structure for this display */
static int vc4_fkms_get_fw_mode(struct vc4_fkms_connector *fkms_connector,
				struct drm_display_mode *mode)
{
	struct vc4_dev *vc4 = fkms_connector->vc4_dev;
	struct set_timings timings = { 0 };
	int ret;

	timings.display = fkms_connector->display_number;

	ret = rpi_firmware_property(vc4->firmware,
				    RPI_FIRMWARE_GET_DISPLAY_TIMING, &timings,
				    sizeof(timings));
	if (ret || !timings.clock)
		/* No mode returned - abort */
		return -1;

	/* Equivalent to DRM_MODE macro. */
	memset(mode, 0, sizeof(*mode));
	strncpy(mode->name, "FIXED_MODE", sizeof(mode->name));
	mode->status = 0;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	mode->clock = timings.clock;
	mode->hdisplay = timings.hdisplay;
	mode->hsync_start = timings.hsync_start;
	mode->hsync_end = timings.hsync_end;
	mode->htotal = timings.htotal;
	mode->hskew = 0;
	mode->vdisplay = timings.vdisplay;
	mode->vsync_start = timings.vsync_start;
	mode->vsync_end = timings.vsync_end;
	mode->vtotal = timings.vtotal;
	mode->vscan = timings.vscan;

	if (timings.flags & TIMINGS_FLAGS_H_SYNC_POS)
		mode->flags |= DRM_MODE_FLAG_PHSYNC;
	else
		mode->flags |= DRM_MODE_FLAG_NHSYNC;

	if (timings.flags & TIMINGS_FLAGS_V_SYNC_POS)
		mode->flags |= DRM_MODE_FLAG_PVSYNC;
	else
		mode->flags |= DRM_MODE_FLAG_NVSYNC;

	if (timings.flags & TIMINGS_FLAGS_INTERLACE)
		mode->flags |= DRM_MODE_FLAG_INTERLACE;

	return 0;
}

static int vc4_fkms_get_edid_block(void *data, u8 *buf, unsigned int block,
				   size_t len)
{
	struct vc4_fkms_connector *fkms_connector =
					(struct vc4_fkms_connector *)data;
	struct vc4_dev *vc4 = fkms_connector->vc4_dev;
	struct mailbox_get_edid mb = {
		.tag1 = { RPI_FIRMWARE_GET_EDID_BLOCK_DISPLAY,
			  128 + 8, 0 },
		.block = block,
		.display_number = fkms_connector->display_number,
	};
	int ret = 0;

	ret = rpi_firmware_property_list(vc4->firmware, &mb, sizeof(mb));

	if (!ret)
		memcpy(buf, mb.edid, len);

	return ret;
}

static int vc4_fkms_connector_get_modes(struct drm_connector *connector)
{
	struct vc4_fkms_connector *fkms_connector =
					to_vc4_fkms_connector(connector);
	struct drm_encoder *encoder = fkms_connector->encoder;
	struct vc4_fkms_encoder *vc4_encoder = to_vc4_fkms_encoder(encoder);
	struct drm_display_mode fw_mode;
	struct drm_display_mode *mode;
	struct edid *edid;
	int num_modes;

	if (!vc4_fkms_get_fw_mode(fkms_connector, &fw_mode)) {
		drm_mode_debug_printmodeline(&fw_mode);
		mode = drm_mode_duplicate(connector->dev,
					  &fw_mode);
		drm_mode_probed_add(connector, mode);
		num_modes = 1;	/* 1 mode */
	} else {
		edid = drm_do_get_edid(connector, vc4_fkms_get_edid_block,
				       fkms_connector);

		/* FIXME: Can we do CEC?
		 * cec_s_phys_addr_from_edid(vc4->hdmi->cec_adap, edid);
		 * if (!edid)
		 *	return -ENODEV;
		 */

		vc4_encoder->hdmi_monitor = drm_detect_hdmi_monitor(edid);

		drm_connector_update_edid_property(connector, edid);
		num_modes = drm_add_edid_modes(connector, edid);
		kfree(edid);
	}

	return num_modes;
}

/* This is the DSI panel resolution. Use this as a default should the firmware
 * not respond to our request for the timings.
 */
static const struct drm_display_mode lcd_mode = {
	DRM_MODE("800x480", DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
		 25979400 / 1000,
		 800, 800 + 1, 800 + 1 + 2, 800 + 1 + 2 + 46, 0,
		 480, 480 + 7, 480 + 7 + 2, 480 + 7 + 2 + 21, 0,
		 0)
};

static int vc4_fkms_lcd_connector_get_modes(struct drm_connector *connector)
{
	struct vc4_fkms_connector *fkms_connector =
					to_vc4_fkms_connector(connector);
	struct drm_display_mode *mode;
	struct drm_display_mode fw_mode;

	if (!vc4_fkms_get_fw_mode(fkms_connector, &fw_mode) && fw_mode.clock)
		mode = drm_mode_duplicate(connector->dev,
					  &fw_mode);
	else
		mode = drm_mode_duplicate(connector->dev,
					  &lcd_mode);

	if (!mode) {
		DRM_ERROR("Failed to create a new display mode\n");
		return -ENOMEM;
	}

	drm_mode_probed_add(connector, mode);

	/* We have one mode */
	return 1;
}

static struct drm_encoder *
vc4_fkms_connector_best_encoder(struct drm_connector *connector)
{
	struct vc4_fkms_connector *fkms_connector =
		to_vc4_fkms_connector(connector);
	DRM_DEBUG_KMS("best_connector.\n");
	return fkms_connector->encoder;
}

static void vc4_fkms_connector_destroy(struct drm_connector *connector)
{
	DRM_DEBUG_KMS("[CONNECTOR:%d] destroy.\n",
		      connector->base.id);
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

/**
 * vc4_connector_duplicate_state - duplicate connector state
 * @connector: digital connector
 *
 * Allocates and returns a copy of the connector state (both common and
 * digital connector specific) for the specified connector.
 *
 * Returns: The newly allocated connector state, or NULL on failure.
 */
struct drm_connector_state *
vc4_connector_duplicate_state(struct drm_connector *connector)
{
	struct vc4_fkms_connector_state *state;

	state = kmemdup(connector->state, sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_connector_duplicate_state(connector, &state->base);
	return &state->base;
}

/**
 * vc4_connector_atomic_get_property - hook for connector->atomic_get_property.
 * @connector: Connector to get the property for.
 * @state: Connector state to retrieve the property from.
 * @property: Property to retrieve.
 * @val: Return value for the property.
 *
 * Returns the atomic property value for a digital connector.
 */
int vc4_connector_atomic_get_property(struct drm_connector *connector,
				      const struct drm_connector_state *state,
				      struct drm_property *property,
				      uint64_t *val)
{
	struct vc4_fkms_connector *fkms_connector =
					to_vc4_fkms_connector(connector);
	struct vc4_fkms_connector_state *vc4_conn_state =
					to_vc4_fkms_connector_state(state);

	if (property == fkms_connector->broadcast_rgb_property) {
		*val = vc4_conn_state->broadcast_rgb;
	} else {
		DRM_DEBUG_ATOMIC("Unknown property [PROP:%d:%s]\n",
				 property->base.id, property->name);
		return -EINVAL;
	}

	return 0;
}

/**
 * vc4_connector_atomic_set_property - hook for connector->atomic_set_property.
 * @connector: Connector to set the property for.
 * @state: Connector state to set the property on.
 * @property: Property to set.
 * @val: New value for the property.
 *
 * Sets the atomic property value for a digital connector.
 */
int vc4_connector_atomic_set_property(struct drm_connector *connector,
				      struct drm_connector_state *state,
				      struct drm_property *property,
				      uint64_t val)
{
	struct vc4_fkms_connector *fkms_connector =
					to_vc4_fkms_connector(connector);
	struct vc4_fkms_connector_state *vc4_conn_state =
					to_vc4_fkms_connector_state(state);

	if (property == fkms_connector->broadcast_rgb_property) {
		vc4_conn_state->broadcast_rgb = val;
		return 0;
	}

	DRM_DEBUG_ATOMIC("Unknown property [PROP:%d:%s]\n",
			 property->base.id, property->name);
	return -EINVAL;
}

static void vc4_hdmi_connector_reset(struct drm_connector *connector)
{
	drm_atomic_helper_connector_reset(connector);
	drm_atomic_helper_connector_tv_reset(connector);
}

static const struct drm_connector_funcs vc4_fkms_connector_funcs = {
	.detect = vc4_fkms_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = vc4_fkms_connector_destroy,
	.reset = vc4_hdmi_connector_reset,
	.atomic_duplicate_state = vc4_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
	.atomic_get_property = vc4_connector_atomic_get_property,
	.atomic_set_property = vc4_connector_atomic_set_property,
};

static const struct drm_connector_helper_funcs vc4_fkms_connector_helper_funcs = {
	.get_modes = vc4_fkms_connector_get_modes,
	.best_encoder = vc4_fkms_connector_best_encoder,
};

static const struct drm_connector_helper_funcs vc4_fkms_lcd_conn_helper_funcs = {
	.get_modes = vc4_fkms_lcd_connector_get_modes,
	.best_encoder = vc4_fkms_connector_best_encoder,
};

static const struct drm_prop_enum_list broadcast_rgb_names[] = {
	{ VC4_BROADCAST_RGB_AUTO, "Automatic" },
	{ VC4_BROADCAST_RGB_FULL, "Full" },
	{ VC4_BROADCAST_RGB_LIMITED, "Limited 16:235" },
};

static void
vc4_attach_broadcast_rgb_property(struct vc4_fkms_connector *fkms_connector)
{
	struct drm_device *dev = fkms_connector->base.dev;
	struct drm_property *prop;

	prop = fkms_connector->broadcast_rgb_property;
	if (!prop) {
		prop = drm_property_create_enum(dev, DRM_MODE_PROP_ENUM,
						"Broadcast RGB",
						broadcast_rgb_names,
						ARRAY_SIZE(broadcast_rgb_names));
		if (!prop)
			return;

		fkms_connector->broadcast_rgb_property = prop;
	}

	drm_object_attach_property(&fkms_connector->base.base, prop, 0);
}

static struct drm_connector *
vc4_fkms_connector_init(struct drm_device *dev, struct drm_encoder *encoder,
			u32 display_num)
{
	struct drm_connector *connector = NULL;
	struct vc4_fkms_connector *fkms_connector;
	struct vc4_fkms_connector_state *conn_state = NULL;
	struct vc4_dev *vc4_dev = to_vc4_dev(dev);
	int ret = 0;

	DRM_DEBUG_KMS("connector_init, display_num %u\n", display_num);

	fkms_connector = devm_kzalloc(dev->dev, sizeof(*fkms_connector),
				      GFP_KERNEL);
	if (!fkms_connector) {
		return ERR_PTR(-ENOMEM);
	}

	/*
	 * Allocate enough memory to hold vc4_fkms_connector_state,
	 */
	conn_state = kzalloc(sizeof(*conn_state), GFP_KERNEL);
	if (!conn_state) {
		kfree(fkms_connector);
		return ERR_PTR(-ENOMEM);
	}

	connector = &fkms_connector->base;

	fkms_connector->encoder = encoder;
	fkms_connector->display_number = display_num;
	fkms_connector->display_type = vc4_get_display_type(display_num);
	fkms_connector->vc4_dev = vc4_dev;

	__drm_atomic_helper_connector_reset(connector,
					    &conn_state->base);

	if (fkms_connector->display_type == DRM_MODE_ENCODER_DSI) {
		drm_connector_init(dev, connector, &vc4_fkms_connector_funcs,
				   DRM_MODE_CONNECTOR_DSI);
		drm_connector_helper_add(connector,
					 &vc4_fkms_lcd_conn_helper_funcs);
		connector->interlace_allowed = 0;
	} else if (fkms_connector->display_type == DRM_MODE_ENCODER_TVDAC) {
		drm_connector_init(dev, connector, &vc4_fkms_connector_funcs,
				   DRM_MODE_CONNECTOR_Composite);
		drm_connector_helper_add(connector,
					 &vc4_fkms_lcd_conn_helper_funcs);
		connector->interlace_allowed = 1;
	} else {
		drm_connector_init(dev, connector, &vc4_fkms_connector_funcs,
				   DRM_MODE_CONNECTOR_HDMIA);
		drm_connector_helper_add(connector,
					 &vc4_fkms_connector_helper_funcs);
		connector->interlace_allowed = 1;
	}

	ret = drm_mode_create_tv_margin_properties(dev);
	if (ret)
		goto fail;

	drm_connector_attach_tv_margin_properties(connector);

	connector->polled = (DRM_CONNECTOR_POLL_CONNECT |
			     DRM_CONNECTOR_POLL_DISCONNECT);

	connector->doublescan_allowed = 0;

	vc4_attach_broadcast_rgb_property(fkms_connector);

	drm_connector_attach_encoder(connector, encoder);

	return connector;

 fail:
	if (connector)
		vc4_fkms_connector_destroy(connector);

	return ERR_PTR(ret);
}

static void vc4_fkms_encoder_destroy(struct drm_encoder *encoder)
{
	DRM_DEBUG_KMS("Encoder_destroy\n");
	drm_encoder_cleanup(encoder);
}

static const struct drm_encoder_funcs vc4_fkms_encoder_funcs = {
	.destroy = vc4_fkms_encoder_destroy,
};

static void vc4_fkms_display_power(struct drm_encoder *encoder, bool power)
{
	struct vc4_fkms_encoder *vc4_encoder = to_vc4_fkms_encoder(encoder);
	struct vc4_dev *vc4 = to_vc4_dev(encoder->dev);

	struct mailbox_display_pwr pwr = {
		.tag1 = {RPI_FIRMWARE_SET_DISPLAY_POWER, 8, 0, },
		.display = vc4_encoder->display_num,
		.state = power ? 1 : 0,
	};

	rpi_firmware_property_list(vc4->firmware, &pwr, sizeof(pwr));
}

static void vc4_fkms_encoder_enable(struct drm_encoder *encoder)
{
	vc4_fkms_display_power(encoder, true);
	DRM_DEBUG_KMS("Encoder_enable\n");
}

static void vc4_fkms_encoder_disable(struct drm_encoder *encoder)
{
	vc4_fkms_display_power(encoder, false);
	DRM_DEBUG_KMS("Encoder_disable\n");
}

static const struct drm_encoder_helper_funcs vc4_fkms_encoder_helper_funcs = {
	.enable = vc4_fkms_encoder_enable,
	.disable = vc4_fkms_encoder_disable,
};

static int vc4_fkms_create_screen(struct device *dev, struct drm_device *drm,
				  int display_idx, int display_ref,
				  struct vc4_crtc **ret_crtc)
{
	struct vc4_dev *vc4 = to_vc4_dev(drm);
	struct vc4_crtc *vc4_crtc;
	struct vc4_fkms_encoder *vc4_encoder;
	struct drm_crtc *crtc;
	struct drm_plane *primary_plane, *overlay_plane, *cursor_plane;
	struct drm_plane *destroy_plane, *temp;
	struct mailbox_blank_display blank = {
		.tag1 = {RPI_FIRMWARE_FRAMEBUFFER_SET_DISPLAY_NUM, 4, 0, },
		.display = display_idx,
		.tag2 = { RPI_FIRMWARE_FRAMEBUFFER_BLANK, 4, 0, },
		.blank = 1,
	};
	int ret;

	vc4_crtc = devm_kzalloc(dev, sizeof(*vc4_crtc), GFP_KERNEL);
	if (!vc4_crtc)
		return -ENOMEM;
	crtc = &vc4_crtc->base;

	vc4_crtc->display_number = display_ref;
	vc4_crtc->display_type = vc4_get_display_type(display_ref);

	/* Blank the firmware provided framebuffer */
	rpi_firmware_property_list(vc4->firmware, &blank, sizeof(blank));

	primary_plane = vc4_fkms_plane_init(drm, DRM_PLANE_TYPE_PRIMARY,
					    display_ref,
					    0 + (display_idx * PLANES_PER_CRTC)
					   );
	if (IS_ERR(primary_plane)) {
		dev_err(dev, "failed to construct primary plane\n");
		ret = PTR_ERR(primary_plane);
		goto err;
	}

	overlay_plane = vc4_fkms_plane_init(drm, DRM_PLANE_TYPE_OVERLAY,
					    display_ref,
					    1 + (display_idx * PLANES_PER_CRTC)
					   );
	if (IS_ERR(overlay_plane)) {
		dev_err(dev, "failed to construct overlay plane\n");
		ret = PTR_ERR(overlay_plane);
		goto err;
	}

	cursor_plane = vc4_fkms_plane_init(drm, DRM_PLANE_TYPE_CURSOR,
					   display_ref,
					   2 + (display_idx * PLANES_PER_CRTC)
					  );
	if (IS_ERR(cursor_plane)) {
		dev_err(dev, "failed to construct cursor plane\n");
		ret = PTR_ERR(cursor_plane);
		goto err;
	}

	drm_crtc_init_with_planes(drm, crtc, primary_plane, cursor_plane,
				  &vc4_crtc_funcs, NULL);
	drm_crtc_helper_add(crtc, &vc4_crtc_helper_funcs);

	vc4_encoder = devm_kzalloc(dev, sizeof(*vc4_encoder), GFP_KERNEL);
	if (!vc4_encoder)
		return -ENOMEM;
	vc4_crtc->encoder = &vc4_encoder->base;

	vc4_encoder->display_num = display_ref;
	vc4_encoder->base.possible_crtcs |= drm_crtc_mask(crtc) ;

	drm_encoder_init(drm, &vc4_encoder->base, &vc4_fkms_encoder_funcs,
			 vc4_crtc->display_type, NULL);
	drm_encoder_helper_add(&vc4_encoder->base,
			       &vc4_fkms_encoder_helper_funcs);

	vc4_crtc->connector = vc4_fkms_connector_init(drm, &vc4_encoder->base,
						      display_ref);
	if (IS_ERR(vc4_crtc->connector)) {
		ret = PTR_ERR(vc4_crtc->connector);
		goto err_destroy_encoder;
	}

	*ret_crtc = vc4_crtc;

	return 0;

err_destroy_encoder:
	vc4_fkms_encoder_destroy(vc4_crtc->encoder);
	list_for_each_entry_safe(destroy_plane, temp,
				 &drm->mode_config.plane_list, head) {
		if (destroy_plane->possible_crtcs == 1 << drm_crtc_index(crtc))
		    destroy_plane->funcs->destroy(destroy_plane);
	}
err:
	return ret;
}

static int vc4_fkms_bind(struct device *dev, struct device *master, void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *drm = dev_get_drvdata(master);
	struct vc4_dev *vc4 = to_vc4_dev(drm);
	struct device_node *firmware_node;
	const struct of_device_id *match;
	struct vc4_crtc **crtc_list;
	u32 num_displays, display_num;
	struct vc4_fkms *fkms;
	int ret;
	u32 display_id;

	vc4->firmware_kms = true;

	fkms = devm_kzalloc(dev, sizeof(*fkms), GFP_KERNEL);
	if (!fkms)
		return -ENOMEM;

	match = of_match_device(vc4_firmware_kms_dt_match, dev);
	if (!match)
		return -ENODEV;
	if (match->data)
		fkms->bcm2711 = true;

	/* firmware kms doesn't have precise a scanoutpos implementation, so
	 * we can't do the precise vblank timestamp mode.
	 */
	drm->driver->get_scanout_position = NULL;
	drm->driver->get_vblank_timestamp = NULL;

	firmware_node = of_parse_phandle(dev->of_node, "brcm,firmware", 0);
	vc4->firmware = rpi_firmware_get(firmware_node);
	if (!vc4->firmware) {
		DRM_DEBUG("Failed to get Raspberry Pi firmware reference.\n");
		return -EPROBE_DEFER;
	}
	of_node_put(firmware_node);

	ret = rpi_firmware_property(vc4->firmware,
				    RPI_FIRMWARE_FRAMEBUFFER_GET_NUM_DISPLAYS,
				    &num_displays, sizeof(u32));

	/* If we fail to get the number of displays, then
	 * assume old firmware that doesn't have the mailbox call, so just
	 * set one display
	 */
	if (ret) {
		num_displays = 1;
		DRM_WARN("Unable to determine number of displays - assuming 1\n");
		ret = 0;
	}

	ret = rpi_firmware_property(vc4->firmware,
				    RPI_FIRMWARE_GET_DISPLAY_CFG,
				    &fkms->cfg, sizeof(fkms->cfg));

	if (ret)
		return -EINVAL;
	/* The firmware works in Hz. This will be compared against kHz, so div
	 * 1000 now rather than multiple times later.
	 */
	fkms->cfg.max_pixel_clock[0] /= 1000;
	fkms->cfg.max_pixel_clock[1] /= 1000;

	/* Allocate a list, with space for a NULL on the end */
	crtc_list = devm_kzalloc(dev, sizeof(crtc_list) * (num_displays + 1),
				 GFP_KERNEL);
	if (!crtc_list)
		return -ENOMEM;

	for (display_num = 0; display_num < num_displays; display_num++) {
		display_id = display_num;
		ret = rpi_firmware_property(vc4->firmware,
					    RPI_FIRMWARE_FRAMEBUFFER_GET_DISPLAY_ID,
					    &display_id, sizeof(display_id));
		/* FIXME: Determine the correct error handling here.
		 * Should we fail to create the one "screen" but keep the
		 * others, or fail the whole thing?
		 */
		if (ret)
			DRM_ERROR("Failed to get display id %u\n", display_num);

		ret = vc4_fkms_create_screen(dev, drm, display_num, display_id,
					     &crtc_list[display_num]);
		if (ret)
			DRM_ERROR("Oh dear, failed to create display %u\n",
				  display_num);
	}

	if (num_displays > 0) {
		/* Map the SMI interrupt reg */
		crtc_list[0]->regs = vc4_ioremap_regs(pdev, 0);
		if (IS_ERR(crtc_list[0]->regs))
			DRM_ERROR("Oh dear, failed to map registers\n");

		writel(0, crtc_list[0]->regs + SMICS);
		ret = devm_request_irq(dev, platform_get_irq(pdev, 0),
				       vc4_crtc_irq_handler, 0,
				       "vc4 firmware kms", crtc_list);
		if (ret)
			DRM_ERROR("Oh dear, failed to register IRQ\n");
	} else {
		DRM_WARN("No displays found. Consider forcing hotplug if HDMI is attached\n");
	}

	vc4->fkms = fkms;

	platform_set_drvdata(pdev, crtc_list);

	return 0;
}

static void vc4_fkms_unbind(struct device *dev, struct device *master,
			    void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct vc4_crtc **crtc_list = dev_get_drvdata(dev);
	int i;

	for (i = 0; crtc_list[i]; i++) {
		vc4_fkms_connector_destroy(crtc_list[i]->connector);
		vc4_fkms_encoder_destroy(crtc_list[i]->encoder);
		drm_crtc_cleanup(&crtc_list[i]->base);
	}

	platform_set_drvdata(pdev, NULL);
}

static const struct component_ops vc4_fkms_ops = {
	.bind   = vc4_fkms_bind,
	.unbind = vc4_fkms_unbind,
};

static int vc4_fkms_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &vc4_fkms_ops);
}

static int vc4_fkms_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &vc4_fkms_ops);
	return 0;
}

struct platform_driver vc4_firmware_kms_driver = {
	.probe = vc4_fkms_probe,
	.remove = vc4_fkms_remove,
	.driver = {
		.name = "vc4_firmware_kms",
		.of_match_table = vc4_firmware_kms_dt_match,
	},
};
