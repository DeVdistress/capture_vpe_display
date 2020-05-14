/*
 * Copyright (C) 2013 Texas Instruments
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


/*
 * This is a drm test app to capture and display frames from a v4l2 device
 * It uses standard single planar V4L2 API to capture progressive frames
 * Displays the buffers via drm in fullscreen
 * Currently only YUYV format is allowed
 * This can be used to test VIP (Video Input Port) on DRA7xx SoC
 * For this, vpdma firmware should be copied in /lib/firmware on target
 */

#include "util.h"

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>

#define NBUF 6

int width = 640, height = 480;

void *buffer_addr[NBUF];
int size[NBUF];

static int xioctl(int fd, int request, void *arg)
{
	int r;

	do r = ioctl (fd, request, arg);
	while (-1 == r && EINTR == errno);

	return r;
}

int init_device(int fd)
{
	char fourcc[5];
	unsigned int i;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	struct v4l2_capability caps;

	/* Check for capture device */
	memset(&caps, 0, sizeof(caps));

	if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &caps)) {
		perror("Setting Pixel Format");
		return 1;
	}
	MSG("Driver: %s\ncaps: %8x", caps.driver, caps.capabilities);
	if(~caps.capabilities & V4L2_CAP_VIDEO_CAPTURE) {
		MSG("Not a capture device");
		return 1;
	}

	/* Set capture format to YUYV */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;

	if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt)) {
		perror("Setting Pixel Format");
		return 1;
	}

	strncpy(fourcc, (char *)&fmt.fmt.pix.pixelformat, 4);
	MSG( "Selected Camera Mode:\n"
		"  Width: %d\n"
		"  Height: %d\n"
		"  PixFmt: %s\n"
		"  Field: %d",
		fmt.fmt.pix.width,
		fmt.fmt.pix.height,
		fourcc,
		fmt.fmt.pix.field);

	/* Currently driver supports only mmap buffers
	 * Request memory mapped buffers */
	memset(&req, 0, sizeof(req));
	req.count = 6;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
		perror("Requesting Buffer");
		return 1;
	}

	for (i = 0; i < req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if(-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf)) {
			perror("Querying Buffer");
			return 1;
		}

		/* Memory map all the buffers and save the addresses */
		buffer_addr[i] = mmap (NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
		size[i] = buf.length;
		MSG("Length: %d\nAddress: %p", buf.length, buffer_addr[i]);
		MSG("Image Length: %d", buf.bytesused);

		/* Queue the buffer for capture */
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		if(-1 == xioctl(fd, VIDIOC_QBUF, &buf)) {
			perror("Queue Buffer");
			return 1;
		}

	}
	if(-1 == xioctl(fd, VIDIOC_STREAMON, &buf.type)) {
		perror("Start Capture");
		return 1;
	}
	return 0;
}

void release_device(int fd)
{
	int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl(fd, VIDIOC_STREAMOFF, &type);
	close(fd);
}

static void usage(char *name) {
	MSG("Usage: %s [OPTION]...", name);
	MSG("V4L2 capture display test");
	MSG("");
	MSG("v4l2capturedisplay options:");
	MSG("\t-h, --help: Print this help and exit.");
	MSG("\t-n:\t Number of frames to capture (0 for infinite)");
	MSG("\t-d:\t Device node to be used as capture device");
	MSG("");
	disp_usage();
}

void copy_buf(struct buffer *buf, void *deqbuf)
{
	int i, height = buf->height, stride = buf->pitches[0];
	int capStride = 2 * width;
	uint8_t *dst, *src;

	dst = omap_bo_map(buf->bo[0]);
	src = deqbuf;

	/* Call this before you start accessing display buffers */
	for (i = 0; i < buf->nbo; i++)
		omap_bo_cpu_prep(buf->bo[i], OMAP_GEM_WRITE);

	/* YUYV format - Only one bo expected
	 * TODO: Change this for all formats */
	for(i=0; i<height; i++) {
		dst += stride;
		src += capStride;
		memcpy(dst, src, capStride);
	}

	/* Call this after you are done with accessing display buffers */
	for (i = 0; i < buf->nbo; i++)
		omap_bo_cpu_fini(buf->bo[i], OMAP_GEM_WRITE);

}

int main(int argc, char **argv)
{
	struct display *disp;
	struct buffer **buffers;
	int ret, i, idx, fd, count = 0;
	char devnode[100] = "/dev/video1";

	/* Parse command line arguments */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h")) {
			argv[i++] = NULL;
			if(sscanf(argv[i], "%d", &height) != 1) {
				ERROR("invalid height: %s", argv[i]);
				return 1;
			}
			argv[i] = NULL;
		} else if (!strcmp(argv[i], "-w")) {
			argv[i++] = NULL;
			if(sscanf(argv[i], "%d", &width) != 1) {
				ERROR("invalid width: %s", argv[i]);
				return 1;
			}
			argv[i] = NULL;
		} else if (!strcmp(argv[i], "-n")) {
			argv[i++] = NULL;
			if(sscanf(argv[i], "%d", &count) != 1) {
				ERROR("invalid count: %s", argv[i]);
				return 1;
			}
			argv[i] = NULL;
		} else if (!strcmp(argv[i], "-d")) {
			argv[i++] = NULL;
			strncpy(devnode, argv[i],100);
			argv[i] = NULL;
		}
	}

	MSG("Opening Video device %s", devnode);
	fd = open(devnode, O_RDWR);
	if (fd == -1) {
		perror("Opening video device");
		return 1;
	}

	ret = init_device(fd);
	if (0 != ret) {
		MSG("Exiting");
		return ret;
	}

	MSG("Opening Display..");
	disp = disp_open(argc, argv);
	if (NULL == disp) {
		usage(argv[0]);
		return 1;
	}

	if (check_args(argc, argv)) {
		/* remaining args.. print usage msg */
		usage(argv[0]);
		return 1;
	}

	/* Request drm to allocate some buffers */
	buffers = disp_get_vid_buffers(disp, NBUF, FOURCC_STR("YUYV"), width, height);
	if (!buffers) {
		return 1;
	}

	for (i = 1; i != count; i++) {
		struct buffer *dispbuf = buffers[i % NBUF];
		struct v4l2_buffer buf;

		/* Dequeue one buffer */
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		if(-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
			perror("Queue Buffer");
			return 1;
		}
		idx = buf.index;

		/* Copy data from dequeued buffer into display buffer */
		copy_buf(dispbuf, buffer_addr[idx]);
		/* Give it to display */
		ret = disp_post_vid_buffer(disp, dispbuf, 0, 0, width, height);
		if (ret) {
			return ret;
		}

		/* Queue it back for next capture */
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = idx;
		if(-1 == xioctl(fd, VIDIOC_QBUF, &buf)) {
			perror("Queue Buffer");
			return 1;
		}
	}

	disp_free_buffers(disp, NBUF);
	disp_close(disp);
	release_device(fd);

	return 0;
}
