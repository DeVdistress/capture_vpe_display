/*
 * Copyright (C) 2011 Texas Instruments
 * Author: Nikhil Devshatwar <nikhil.nd@ti.com>
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
#include <pthread.h>
#include <xf86drmMode.h>
#include <wayland-client.h>
#include <wayland-drm-client-protocol.h>
#include <viewporter-client-protocol.h>

/* NOTE: healthy dose of recycling from libdrm modetest app.. */


#define to_display_wl(x) container_of(x, struct display_wl, base)
struct display_wl {
	struct display base;

	uint32_t bo_flags;
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_surface *surface;
	struct wl_shell *shell;
	struct wl_drm *drm;
	struct wl_callback *callback;
	struct wp_viewport *viewport;
	struct wp_viewporter *viewporter;
	pthread_t threadID;
};

#define to_buffer_wl(x) container_of(x, struct buffer_wl, base)
struct buffer_wl {
	struct buffer base;

	struct wl_buffer *wl_buf;
	uint32_t name;
};

static int global_fd = 0;
static uint32_t used_planes = 0;
static int ndisplays = 0;

int wayland_process_events(struct display_wl *disp_wl)
{

	wl_display_dispatch(disp_wl->display);

	return 0;
}
void* wayland_ev_task(void *arg)
{
	int ret;
	while(1) {
		ret = wayland_process_events((struct display_wl *)arg);
	}

	return NULL;
}

static struct omap_bo *
alloc_bo(struct display *disp, uint32_t bpp, uint32_t width, uint32_t height,
		uint32_t *bo_handle, uint32_t *pitch)
{
	struct display_wl *disp_wl = to_display_wl(disp);
	struct omap_bo *bo;
	uint32_t bo_flags = disp_wl->bo_flags;

	bo_flags |= OMAP_BO_WC;
	bo = omap_bo_new(disp->dev, width * height * bpp / 8, bo_flags);

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
	struct display_wl *disp_wl = to_display_wl(disp);
	struct buffer_wl *buf_wl;
	struct buffer *buf;
	uint32_t bo_handles[4] = {0};
	uint32_t wl_fmt = WL_DRM_FORMAT_NV12;

	buf_wl = calloc(1, sizeof(*buf_wl));
	if (!buf_wl) {
		ERROR("allocation failed");
		return NULL;
	}
	buf = &buf_wl->base;

	buf->fourcc = fourcc;
	buf->width = w;
	buf->height = h;
	buf->multiplanar = false;

	buf->nbo = 1;

	if (!fourcc)
		fourcc = FOURCC('A','R','2','4');

	switch (fourcc) {
	case FOURCC('A','R','2','4'):
		wl_fmt = WL_DRM_FORMAT_ARGB8888;
		buf->nbo = 1;
		buf->bo[0] = alloc_bo(disp, 32, buf->width, buf->height,
				&bo_handles[0], &buf->pitches[0]);
		omap_bo_get_name(buf->bo[0], &buf_wl->name);
		buf->fd[0] = omap_bo_dmabuf(buf->bo[0]);

		/* ARGB: stride is four times width */
		buf_wl->wl_buf = wl_drm_create_buffer(disp_wl->drm,
			buf_wl->name, buf->width, buf->height,
			buf->width * 4, wl_fmt);
		break;
	case FOURCC('U','Y','V','Y'):
		wl_fmt = WL_DRM_FORMAT_UYVY;
		buf->nbo = 1;
		buf->bo[0] = alloc_bo(disp, 16, buf->width, buf->height,
				&bo_handles[0], &buf->pitches[0]);

		omap_bo_get_name(buf->bo[0], &buf_wl->name);
		buf->fd[0] = omap_bo_dmabuf(buf->bo[0]);
		/* YUYV; stride is double width */
		buf_wl->wl_buf = wl_drm_create_buffer(disp_wl->drm,
			buf_wl->name, buf->width, buf->height,
			buf->width * 2, wl_fmt);
		break;
	case FOURCC('Y','U','Y','V'):
		wl_fmt = WL_DRM_FORMAT_YUYV;
		buf->nbo = 1;
		buf->bo[0] = alloc_bo(disp, 16, buf->width, buf->height,
				&bo_handles[0], &buf->pitches[0]);

		omap_bo_get_name(buf->bo[0], &buf_wl->name);
		buf->fd[0] = omap_bo_dmabuf(buf->bo[0]);
		/* YUYV; stride is double width */
		buf_wl->wl_buf = wl_drm_create_buffer(disp_wl->drm,
			buf_wl->name, buf->width, buf->height,
			buf->width * 2, wl_fmt);
		break;
	case FOURCC('N','V','1','2'):
		wl_fmt = WL_DRM_FORMAT_NV12;
		buf->nbo = 1;
		buf->bo[0] = alloc_bo(disp, 8, buf->width, buf->height * 3 / 2,
				&bo_handles[0], &buf->pitches[0]);

		omap_bo_get_name(buf->bo[0], &buf_wl->name);
		buf->fd[0] = omap_bo_dmabuf(buf->bo[0]);
		/* NV12: Create a planar buffer */
		buf_wl->wl_buf = wl_drm_create_planar_buffer(disp_wl->drm,
			buf_wl->name, buf->width, buf->height, wl_fmt,
			0, buf->width,
			buf->width * buf->height, buf->width,
			0, 0);
		break;
	default:
		ERROR("invalid format: 0x%08x", fourcc);
		goto fail;
	}

	return buf;

fail:
	// XXX cleanup
	return NULL;
}

static void
free_buffers(struct display *disp, uint32_t n)
{
	uint32_t i;
	for (i = 0; i < n; i++) {
		if (disp->buf[i]) {
			close(disp->buf[i]->fd[0]);
			omap_bo_del(disp->buf[i]->bo[0]);
			if (disp->multiplanar) {
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
	disp->buf = bufs;
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

static int
post_buffer(struct display *disp, struct buffer *buf)
{
	return -1;
}
static void redraw(void *data, struct wl_callback *callback, uint32_t time) {
       struct display_wl *disp_wl = (struct display_wl *) data;

       wl_callback_destroy(disp_wl->callback);
       disp_wl->callback = NULL;
}

static const struct wl_callback_listener frame_listener = { redraw };

static int
post_vid_buffer(struct display *disp, struct buffer *buf,
		uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	struct display_wl *disp_wl = to_display_wl(disp);
	struct buffer_wl *buf_wl = to_buffer_wl(buf);
	int ret = 0;

	wl_surface_attach(disp_wl->surface, buf_wl->wl_buf, 0, 0);
	wl_surface_damage(disp_wl->surface,
			0, 0, disp->width, disp->height);
	wp_viewport_set_source(disp_wl->viewport,
			wl_fixed_from_int(x), wl_fixed_from_int(y),
			wl_fixed_from_int(w), wl_fixed_from_int(h));

	if (disp_wl->callback)
		wl_callback_destroy(disp_wl->callback);
	disp_wl->callback = wl_surface_frame(disp_wl->surface);

	wl_callback_add_listener(disp_wl->callback, &frame_listener, disp_wl);

	wl_surface_commit(disp_wl->surface);
	wl_display_flush(disp_wl->display);

	return ret;
}

static void
close_kms(struct display *disp)
{
	struct display_wl *disp_wl = to_display_wl(disp);
	pthread_cancel(disp_wl->threadID);

	omap_device_del(disp->dev);
	disp->dev = NULL;
	if (used_planes) {
		used_planes >>= 1;
	}
	if (--ndisplays == 0) {
		close(global_fd);
	}
}

void
disp_wayland_usage(void)
{
	MSG("WAYLAND Display Options:");
	MSG("\t-w <width>x<height>\tset the dimensions of client window");
}

static void
registry_handle_global(void *data, struct wl_registry *registry, uint32_t id,
	const char *interface, uint32_t version)
{
	struct display_wl *disp_wl = (struct display_wl *) data;

	if (strcmp(interface, "wl_compositor") == 0) {
		disp_wl->compositor = wl_registry_bind(registry, id,
			&wl_compositor_interface, 3);
	} else if (strcmp(interface, "wl_shell") == 0) {
		disp_wl->shell = wl_registry_bind(registry, id,
			&wl_shell_interface, 1);
	} else if (strcmp(interface, "wl_drm") == 0) {
		disp_wl->drm = wl_registry_bind(registry, id,
			&wl_drm_interface, 1);
	} else if (strcmp(interface, "wp_viewporter") == 0) {
		disp_wl->viewporter = wl_registry_bind(registry, id,
			&wp_viewporter_interface, 1);
	}
}

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name)
{
}
static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};


struct display *
disp_wayland_open(int argc, char **argv)
{
	struct display_wl *disp_wl = NULL;
	struct display *disp;
	int i, enabled = 0, width, height;

	struct wl_registry *registry;
	struct wl_shell_surface *shell_surface;
	struct wl_region *region;
	int ret;

	/* note: set args to NULL after we've parsed them so other modules know
	 * that it is already parsed (since the arg parsing is decentralized)
	 */
	for (i = 1; i < argc; i++) {
		if (!argv[i]) {
			continue;
		}
		if (!strcmp("-w", argv[i])) {
			argv[i++] = NULL;
			if (sscanf(argv[i], "%dx%d", &width, &height) != 2) {
				ERROR("invalid arg: -w %s", argv[i]);
				goto fail;
			}
			enabled = 1;
		} else {
			/* ignore */
			continue;
		}
		argv[i] = NULL;
	}
	// If not explicitely enabled from command line, fail.
	if (!enabled)
		goto fail;

	disp_wl = calloc(1, sizeof(*disp_wl));
	if (!disp_wl) {
		ERROR("allocation failed");
		goto fail;
	}
	disp = &disp_wl->base;

	if (!global_fd) {
		global_fd = drmOpen("omapdrm", NULL);
		if (global_fd < 0) {
			ERROR("could not open drm device: %s (%d)",
				strerror(errno), errno);
			goto fail;
		}
	}

	disp->fd = global_fd;
	ndisplays++;

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
	disp->disp_free_buf = free_buffers;

	disp->multiplanar = false;
	disp->width = width;
	disp->height = height;

	disp_wl->bo_flags = OMAP_BO_SCANOUT|OMAP_BO_WC;

	disp_wl->display = wl_display_connect(NULL);
	if (disp_wl->display == NULL) {
		ERROR("failed to connect to Wayland display: %m\n");
		goto fail;
	} else {
		MSG("wayland display opened\n");
	}

	/* Find out what interfaces are implemented, and initialize */
	registry = wl_display_get_registry(disp_wl->display);
	wl_registry_add_listener(registry, &registry_listener, disp_wl);
	wl_display_roundtrip(disp_wl->display);
	MSG("wayland registries obtained\n");

	disp_wl->surface = wl_compositor_create_surface(disp_wl->compositor);
	shell_surface = wl_shell_get_shell_surface(disp_wl->shell,
						disp_wl->surface);
	wl_shell_surface_set_toplevel(shell_surface);

	region = wl_compositor_create_region(disp_wl->compositor);
	wl_region_add(region, 0, 0, disp->width, disp->height);

	disp_wl->viewport = wp_viewporter_get_viewport(disp_wl->viewporter,
						disp_wl->surface);
	wp_viewport_set_destination(disp_wl->viewport, width, height);

	ret = pthread_create(&disp_wl->threadID, NULL, wayland_ev_task, disp_wl);
	if(ret) {
		MSG("could not create task for wayland event processing");
        }

	return disp;

fail:
	// XXX cleanup
	return NULL;
}
