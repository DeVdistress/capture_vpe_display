/*
 * Copyright (C) 2011 Texas Instruments
 * Author: Rob Clark <rob.clark@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "util.h"

#include <xf86drmMode.h>


/* NOTE: healthy dose of recycling from libdrm modetest app.. */

/*
 * Mode setting with the kernel interfaces is a bit of a chore.
 * First you have to find the connector in question and make sure the
 * requested mode is available.
 * Then you need to find the encoder attached to that connector so you
 * can bind it with a free crtc.
 */
struct connector {
	uint32_t id;
	char mode_str[64];
	drmModeModeInfo *mode;
	drmModeEncoder *encoder;
	int crtc;
	int pipe;
};

#define to_display_kms(x) container_of(x, struct display_kms, base)
struct display_kms {
	struct display base;

	uint32_t connectors_count;
	struct connector connector[10];
	drmModePlane *ovr[10];

	int scheduled_flips, completed_flips;
	uint32_t bo_flags;
	drmModeResPtr resources;
	drmModePlaneRes *plane_resources;
	struct buffer *current;
	bool no_master;
	int mastership;
};

#define to_buffer_kms(x) container_of(x, struct buffer_kms, base)
struct buffer_kms {
	struct buffer base;
	uint32_t fb_id;
};

static int global_fd = 0;
static uint32_t used_planes = 0;
static int ndisplays = 0;

static struct omap_bo *
alloc_bo(struct display *disp, uint32_t bpp, uint32_t width, uint32_t height,
		uint32_t *bo_handle, uint32_t *pitch)
{
	struct display_kms *disp_kms = to_display_kms(disp);
	struct omap_bo *bo;
	uint32_t bo_flags = disp_kms->bo_flags;

	if ((bo_flags & OMAP_BO_TILED) == OMAP_BO_TILED) {
		bo_flags &= ~OMAP_BO_TILED;
		if (bpp == 8) {
			bo_flags |= OMAP_BO_TILED_8;
		} else if (bpp == 16) {
			bo_flags |= OMAP_BO_TILED_16;
		} else if (bpp == 32) {
			bo_flags |= OMAP_BO_TILED_32;
		}
	}
	bo_flags |= OMAP_BO_WC;

	if (bo_flags & OMAP_BO_TILED) {
		bo = omap_bo_new_tiled(disp->dev, ALIGN2(width,7), height, bo_flags);
	} else {
		bo = omap_bo_new(disp->dev, width * height * bpp / 8, bo_flags);
	}

	if (bo) {
		*bo_handle = omap_bo_handle(bo);
		*pitch = width * bpp / 8;
		if (bo_flags & OMAP_BO_TILED)
			*pitch = ALIGN2(*pitch, PAGE_SHIFT);
	}

	return bo;
}

static struct buffer *
alloc_buffer(struct display *disp, uint32_t fourcc, uint32_t w, uint32_t h)
{
	struct buffer_kms *buf_kms;
	struct buffer *buf;
	uint32_t bo_handles[4] = {0}, offsets[4] = {0};
	int ret;

	buf_kms = calloc(1, sizeof(*buf_kms));
	if (!buf_kms) {
		ERROR("allocation failed");
		return NULL;
	}
	buf = &buf_kms->base;

	buf->fourcc = fourcc;
	buf->width = w;
	buf->height = h;
	buf->multiplanar = true;

	buf->nbo = 1;

	if (!fourcc)
		fourcc = FOURCC('A','R','2','4');

	switch(fourcc) {
	case FOURCC('A','R','2','4'):
		buf->nbo = 1;
		buf->bo[0] = alloc_bo(disp, 32, buf->width, buf->height,
				&bo_handles[0], &buf->pitches[0]);
		break;
	case FOURCC('U','Y','V','Y'):
	case FOURCC('Y','U','Y','V'):
		buf->nbo = 1;
		buf->bo[0] = alloc_bo(disp, 16, buf->width, buf->height,
				&bo_handles[0], &buf->pitches[0]);
		break;
	case FOURCC('N','V','1','2'):
		if (disp->multiplanar) {
			buf->nbo = 2;
			buf->bo[0] = alloc_bo(disp, 8, buf->width, buf->height,
					&bo_handles[0], &buf->pitches[0]);
			buf->fd[0] = omap_bo_dmabuf(buf->bo[0]);
			buf->bo[1] = alloc_bo(disp, 16, buf->width/2, buf->height/2,
					&bo_handles[1], &buf->pitches[1]);
			buf->fd[1] = omap_bo_dmabuf(buf->bo[1]);
		} else {
			buf->nbo = 1;
			buf->bo[0] = alloc_bo(disp, 8, buf->width, buf->height * 3 / 2,
					&bo_handles[0], &buf->pitches[0]);
			buf->fd[0] = omap_bo_dmabuf(buf->bo[0]);
			bo_handles[1] = bo_handles[0];
			buf->pitches[1] = buf->pitches[0];
			offsets[1] = buf->width * buf->height;
			buf->multiplanar = false;
		}
		break;
	case FOURCC('I','4','2','0'):
		buf->nbo = 3;
		buf->bo[0] = alloc_bo(disp, 8, buf->width, buf->height,
				&bo_handles[0], &buf->pitches[0]);
		buf->bo[1] = alloc_bo(disp, 8, buf->width/2, buf->height/2,
				&bo_handles[1], &buf->pitches[1]);
		buf->bo[2] = alloc_bo(disp, 8, buf->width/2, buf->height/2,
				&bo_handles[2], &buf->pitches[2]);
		break;
	default:
		ERROR("invalid format: 0x%08x", fourcc);
		goto fail;
	}

	ret = drmModeAddFB2(disp->fd, buf->width, buf->height, fourcc,
			bo_handles, buf->pitches, offsets, &buf_kms->fb_id, 0);
	if (ret) {
		ERROR("drmModeAddFB2 failed: %s (%d)", strerror(errno), ret);
		goto fail;
	}

	return buf;

fail:
	// XXX cleanup
	return NULL;
}

void
free_buffers(struct display *disp, uint32_t n)
{
	uint32_t i;
	for (i = 0; i < n; i++) {
                if (disp->buf[i]) {
			close(disp->buf[i]->fd[0]);
			omap_bo_del(disp->buf[i]->bo[0]);
			if(disp->multiplanar){
				close(disp->buf[i]->fd[1]);
				omap_bo_del(disp->buf[i]->bo[1]);
			}
                }
        }
free(disp->buf);
}

static struct buffer **
alloc_buffers(struct display *disp, uint32_t n,
		uint32_t fourcc, uint32_t w, uint32_t h)
{
	struct buffer **bufs;
	uint32_t i = 0;

	bufs = calloc(n, sizeof(*bufs));
	if (!bufs) {
		ERROR("allocation failed");
		goto fail;
	}

	for (i = 0; i < n; i++) {
		bufs[i] = alloc_buffer(disp, fourcc, w, h);
		if (!bufs[i]) {
			ERROR("allocation failed");
			goto fail;
		}
	}
	disp->buf=bufs;
	return bufs;

fail:
	// XXX cleanup
	return NULL;
}

static struct buffer **
get_buffers(struct display *disp, uint32_t n)
{
	return alloc_buffers(disp, n, 0, disp->width, disp->height);
}

static struct buffer **
get_vid_buffers(struct display *disp, uint32_t n,
		uint32_t fourcc, uint32_t w, uint32_t h)
{
	return alloc_buffers(disp, n, fourcc, w, h);
}

static void
page_flip_handler(int fd, unsigned int frame,
		unsigned int sec, unsigned int usec, void *data)
{
	struct display *disp = data;
	struct display_kms *disp_kms = to_display_kms(disp);

	disp_kms->completed_flips++;

	MSG("Page flip: frame=%d, sec=%d, usec=%d, remaining=%d", frame, sec, usec,
			disp_kms->scheduled_flips - disp_kms->completed_flips);
}

static int
post_buffer(struct display *disp, struct buffer *buf)
{
	struct display_kms *disp_kms = to_display_kms(disp);
	struct buffer_kms *buf_kms = to_buffer_kms(buf);
	int ret, last_err = 0, x = 0;
	uint32_t i;

	for (i = 0; i < disp_kms->connectors_count; i++) {
		struct connector *connector = &disp_kms->connector[i];

		if (! connector->mode) {
			continue;
		}

		if (! disp_kms->current) {
			/* first buffer we flip to, setup the mode (since this can't
			 * be done earlier without a buffer to scanout)
			 */
			MSG("Setting mode %s on connector %d, crtc %d",
					connector->mode_str, connector->id, connector->crtc);

			ret = drmModeSetCrtc(disp->fd, connector->crtc, buf_kms->fb_id,
					x, 0, &connector->id, 1, connector->mode);

			x += connector->mode->hdisplay;
		} else {
			ret = drmModePageFlip(disp->fd, connector->crtc, buf_kms->fb_id,
					DRM_MODE_PAGE_FLIP_EVENT, disp);
			disp_kms->scheduled_flips++;
		}

		if (ret) {
			ERROR("Could not post buffer on crtc %d: %s (%d)",
					connector->crtc, strerror(errno), ret);
			last_err = ret;
			/* well, keep trying the reset of the connectors.. */
		}
	}

	/* if we flipped, wait for all flips to complete! */
	while (disp_kms->scheduled_flips > disp_kms->completed_flips) {
		drmEventContext evctx = {
				.version = DRM_EVENT_CONTEXT_VERSION,
				.page_flip_handler = page_flip_handler,
		};
		struct timeval timeout = {
				.tv_sec = 3,
				.tv_usec = 0,
		};
		fd_set fds;

		FD_ZERO(&fds);
		FD_SET(disp->fd, &fds);

		ret = select(disp->fd + 1, &fds, NULL, NULL, &timeout);
		if (ret <= 0) {
			if (errno == EAGAIN) {
				continue;    /* keep going */
			} else {
				ERROR("Timeout waiting for flip complete: %s (%d)",
						strerror(errno), ret);
				last_err = ret;
				break;
			}
		}

		drmHandleEvent(disp->fd, &evctx);
	}

	disp_kms->current = buf;

	return last_err;
}

int
get_overlay_plane(struct display *disp, struct buffer *buf)
{
	struct display_kms *disp_kms = to_display_kms(disp);
	uint32_t i;
	int ret;

	for (i = 0; i < disp_kms->connectors_count; i++) {
		disp_kms->ovr[i] = drmModeGetPlane(disp->fd,
					disp_kms->plane_resources->planes[1]);

		ret = drmModeObjectSetProperty(disp->fd,
			disp_kms->ovr[i]->plane_id, DRM_MODE_OBJECT_PLANE, 7, 3);
		if (ret < 0) {
			MSG("Could not set Z order for plane");
			return ret;
		}
	}
	return 0;
}

static int
post_vid_buffer(struct display *disp, struct buffer *buf,
		uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	struct display_kms *disp_kms = to_display_kms(disp);
	struct buffer_kms *buf_kms = to_buffer_kms(buf);
	drmModePlane *ovr;
	int ret = 0;
	uint32_t i, j;

	/* ensure we have the overlay setup: */
	for (i = 0; i < disp_kms->connectors_count; i++) {
		struct connector *connector = &disp_kms->connector[i];
		drmModeModeInfo *mode = connector->mode;

		if (! mode) {
			continue;
		}

		if (! disp_kms->ovr[i]) {

			for (j = 0; j < disp_kms->plane_resources->count_planes; j++) {
				if (used_planes & (1 << j)) {
					continue;
				}
				ovr = drmModeGetPlane(disp->fd,
						disp_kms->plane_resources->planes[j]);
				if (ovr->possible_crtcs & (1 << connector->pipe)) {
					disp_kms->ovr[i] = ovr;
					used_planes |= (1 << j);
					break;
				}
			}
		}

		if (! disp_kms->ovr[i]) {
			MSG("Could not find plane for crtc %d", connector->crtc);
			ret = -1;
			/* carry on and see if we can find at least one usable plane */
			continue;
		}

		if(buf->noScale) {
			ret = drmModeSetPlane(disp->fd, disp_kms->ovr[i]->plane_id,
				connector->crtc, buf_kms->fb_id, 0,
				/* Use x and y as co-ordinates of overlay */
				x, y, buf->width, buf->height,
				/* Consider source x and y is 0 always */
				0, 0, w << 16, h << 16);
		} else {
			ret = drmModeSetPlane(disp->fd, disp_kms->ovr[i]->plane_id,
				connector->crtc, buf_kms->fb_id, 0,
				/* make video fullscreen: */
				0, 0, mode->hdisplay, mode->vdisplay,
				/* source/cropping coordinates are given in Q16 */
				x << 16, y << 16, w << 16, h << 16);
		}
		if (ret) {
			ERROR("failed to enable plane %d: %s",
					disp_kms->ovr[i]->plane_id, strerror(errno));
		}
	}

	if (disp_kms->no_master && disp_kms->mastership) {
		/* Drop mastership after the first buffer on each plane is
		 * displayed. This will lock these planes for us and allow
		 * others to consume remaining
		 */
		disp_kms->mastership = 0;
		drmDropMaster(disp->fd);
	}

	return ret;
}

static void
close_kms(struct display *disp)
{
	omap_device_del(disp->dev);
	disp->dev = NULL;
	if (used_planes) {
		used_planes >>= 1;
	}
	if (--ndisplays == 0) {
		close(global_fd);
	}
}

static void
connector_find_mode(struct display *disp, struct connector *c)
{
	struct display_kms *disp_kms = to_display_kms(disp);
	drmModeConnector *connector;
	int i, j;

	/* First, find the connector & mode */
	c->mode = NULL;
	for (i = 0; i < disp_kms->resources->count_connectors; i++) {
		connector = drmModeGetConnector(disp->fd,
				disp_kms->resources->connectors[i]);

		if (!connector) {
			ERROR("could not get connector %i: %s",
					disp_kms->resources->connectors[i], strerror(errno));
			drmModeFreeConnector(connector);
			continue;
		}

		if (!connector->count_modes) {
			drmModeFreeConnector(connector);
			continue;
		}

		if (connector->connector_id != c->id) {
			drmModeFreeConnector(connector);
			continue;
		}

		for (j = 0; j < connector->count_modes; j++) {
			c->mode = &connector->modes[j];
			if (!strcmp(c->mode->name, c->mode_str))
				break;
		}

		/* Found it, break out */
		if (c->mode)
			break;

		drmModeFreeConnector(connector);
	}

	if (!c->mode) {
		ERROR("failed to find mode \"%s\"", c->mode_str);
		return;
	}

	/* Now get the encoder */
	for (i = 0; i < connector->count_encoders; i++) {
		c->encoder = drmModeGetEncoder(disp->fd,
				connector->encoders[i]);

		if (!c->encoder) {
			ERROR("could not get encoder %i: %s",
					disp_kms->resources->encoders[i], strerror(errno));
			continue;
		}

		/* Take the fisrt one, if none is assigned */
		if (!connector->encoder_id)
		{
			connector->encoder_id = c->encoder->encoder_id;
		}

		if (c->encoder->encoder_id  == connector->encoder_id) {
			/* find the first valid CRTC if not assigned */
			if (!c->encoder->crtc_id)
			{
				int k;
				for (k = 0; k < disp_kms->resources->count_crtcs; ++k) {
					/* check whether this CRTC works with the encoder */
					if (!(c->encoder->possible_crtcs & (1 << k)))
						continue;

					c->encoder->crtc_id = disp_kms->resources->crtcs[k];
					break;
				}

				if (!c->encoder->crtc_id)
				{
					ERROR("Encoder(%d): no CRTC found!\n", c->encoder->encoder_id);
					drmModeFreeEncoder(c->encoder);
					continue;
				}
			}
			break;
		}
		drmModeFreeEncoder(c->encoder);
	}

	if (c->crtc == -1)
		c->crtc = c->encoder->crtc_id;

	/* and figure out which crtc index it is: */
	for (i = 0; i < disp_kms->resources->count_crtcs; i++) {
		if (c->crtc == (int)disp_kms->resources->crtcs[i]) {
			c->pipe = i;
			break;
		}
	}
}

void
disp_kms_usage(void)
{
	MSG("KMS Display Options:");
	MSG("\t-1 \t\tforce single-plane buffers");
	MSG("\t-t <tiled-mode>\t8, 16, 32, or auto");
	MSG("\t-s <connector_id>:<mode>\tset a mode");
	MSG("\t-s <connector_id>@<crtc_id>:<mode>\tset a mode");
}

struct display *
disp_kms_open(int argc, char **argv)
{
	struct display_kms *disp_kms = NULL;
	struct display *disp;
	struct buffer **bufs;
	int i;

	disp_kms = calloc(1, sizeof(*disp_kms));
	if (!disp_kms) {
		ERROR("allocation failed");
		goto fail;
	}
	disp = &disp_kms->base;

	if (!global_fd) {
		global_fd = drmOpen("omapdrm", NULL);
		if (global_fd < 0) {
			ERROR("could not open drm device: %s (%d)", strerror(errno), errno);
			goto fail;
		}
	}

	disp->fd = global_fd;
	ndisplays++;  /* increment the number of displays counter */

	disp->dev = omap_device_new(disp->fd);
	if (!disp->dev) {
		ERROR("couldn't create device");
		goto fail;
	}

	disp->get_buffers = get_buffers;
	disp->get_vid_buffers = get_vid_buffers;
	disp->post_buffer = post_buffer;
	disp->post_vid_buffer = post_vid_buffer;
	disp->close = close_kms;
	disp->disp_free_buf = free_buffers ;
	disp_kms->resources = drmModeGetResources(disp->fd);
	if (!disp_kms->resources) {
		ERROR("drmModeGetResources failed: %s", strerror(errno));
		goto fail;
	}

	disp_kms->plane_resources = drmModeGetPlaneResources(disp->fd);
	if (!disp_kms->plane_resources) {
		ERROR("drmModeGetPlaneResources failed: %s", strerror(errno));
		goto fail;
	}

	disp->multiplanar = true;

	/* note: set args to NULL after we've parsed them so other modules know
	 * that it is already parsed (since the arg parsing is decentralized)
	 */
	for (i = 1; i < argc; i++) {
		if (!argv[i]) {
			continue;
		}
		if (!strcmp("-1", argv[i])) {
			disp->multiplanar = false;
		} else if (!strcmp("-t", argv[i])) {
			int n;
			argv[i++] = NULL;
			if (!strcmp(argv[i], "auto")) {
				n = 0;
			} else if (sscanf(argv[i], "%d", &n) != 1) {
				ERROR("invalid arg: %s", argv[i]);
				goto fail;
			}

			disp_kms->bo_flags &= ~OMAP_BO_TILED;

			if (n == 8) {
				disp_kms->bo_flags |= OMAP_BO_TILED_8;
			} else if (n == 16) {
				disp_kms->bo_flags |= OMAP_BO_TILED_16;
			} else if (n == 32) {
				disp_kms->bo_flags |= OMAP_BO_TILED_32;
			} else if (n == 0) {
				disp_kms->bo_flags |= OMAP_BO_TILED;
			} else {
				ERROR("invalid arg: %s", argv[i]);
				goto fail;
			}
		} else if (!strcmp("-s", argv[i])) {
			struct connector *connector =
					&disp_kms->connector[disp_kms->connectors_count++];
			connector->crtc = -1;
			argv[i++] = NULL;
			if (sscanf(argv[i], "%d:%64s",
				   &connector->id,
				   connector->mode_str) != 2 &&
			    sscanf(argv[i], "%d@%d:%64s",
				   &connector->id,
				   &connector->crtc,
				   connector->mode_str) != 3) {
				// TODO: we could support connector specified as a name too, I suppose
				ERROR("invalid arg: %s", argv[i]);
				goto fail;
			}
			disp_kms->bo_flags |= OMAP_BO_SCANOUT;
		} else if (!strcmp("-nm", argv[i])) {
			disp_kms->no_master = true;
			disp_kms->mastership = 1;
		} else {
			/* ignore */
			continue;
		}
		argv[i] = NULL;
	}

	disp->width = 0;
	disp->height = 0;
	for (i = 0; i < (int)disp_kms->connectors_count; i++) {
		struct connector *c = &disp_kms->connector[i];
		connector_find_mode(disp, c);
		if (c->mode == NULL)
			continue;
		/* setup side-by-side virtual display */
		disp->width += c->mode->hdisplay;
		if (disp->height < c->mode->vdisplay) {
			disp->height = c->mode->vdisplay;
		}
	}

	MSG("using %d connectors, %dx%d display, multiplanar: %d",
			disp_kms->connectors_count, disp->width, disp->height, disp->multiplanar);

	bufs = disp_get_buffers(disp, 1);
	disp_post_buffer(disp, bufs[0]);

	return disp;

fail:
	// XXX cleanup
	return NULL;
}
