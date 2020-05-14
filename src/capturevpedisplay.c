/*
 *  Copyright (c) 2013-2014, Texas Instruments Incorporated
 *  Author: alaganraj <alaganraj.s@ti.com>
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *  *  Neither the name of Texas Instruments Incorporated nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 *  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 *  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 *  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 *  OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 *  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 *  EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  Contact information for paper mail:
 *  Texas Instruments
 *  Post Office Box 655303
 *  Dallas, Texas 75265
 *  Contact information:
 *  http://www-k.ext.ti.com/sc/technical-support/product-information-centers.htm?
 *  DCMP=TIHomeTracking&HQS=Other+OT+home_d_contact
 *  ============================================================================
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>

#include <sys/mman.h>
#include <sys/ioctl.h>

#include <xf86drm.h>
#include <omap_drm.h>
#include <omap_drmif.h>

#include "util.h"

#include "vpe-common.c"

#define NUMBUF	6 //to be removed

/** VIP file descriptor */
static int vipfd  = -1;
static int doOnce = 0;

struct buffer **shared_bufs;

/**
 *****************************************************************************
 * @brief:  set format for vip
 *
 * @param:  width  int
 * @param:  height int
 * @param:  fourcc int
 *
 * @return: 0 on success 
 *****************************************************************************
*/
int vip_set_format(int width, int height, int fourcc)
{
	int ret;
	struct v4l2_format fmt;

	memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.pixelformat = fourcc;
	fmt.fmt.pix.field = V4L2_FIELD_ALTERNATE;

	// to change the parameters
	ret = ioctl(vipfd, VIDIOC_S_FMT, &fmt);
	if (ret < 0)
		pexit( "vip: S_FMT failed: %s\n", strerror(errno));

	// to query the current parameters
	ret = ioctl(vipfd, VIDIOC_G_FMT, &fmt);
	if (ret < 0)
		pexit( "vip: G_FMT after set format failed: %s\n", strerror(errno));

	printf("vip: G_FMT(start): width = %u, height = %u, 4cc = %.4s\n",
			fmt.fmt.pix.width, fmt.fmt.pix.height,
			(char*)&fmt.fmt.pix.pixelformat);

	return 0;
}

/**
 *****************************************************************************
 * @brief:  request buffer for vip
 *  just configures the driver into DMABUF
 * @return: 0 on success 
 *****************************************************************************
*/
int vip_reqbuf(void)
{
	int ret;
	struct v4l2_requestbuffers rqbufs;

	memset(&rqbufs, 0, sizeof(rqbufs));
	rqbufs.count = NUMBUF;
	rqbufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rqbufs.memory = V4L2_MEMORY_DMABUF;

	ret = ioctl(vipfd, VIDIOC_REQBUFS, &rqbufs);
	if (ret < 0)
		pexit( "vip: REQBUFS failed: %s\n", strerror(errno));

	dprintf("vip: allocated buffers = %d\n", rqbufs.count);

	return 0;
}

/**
 *****************************************************************************
 * @brief:  allocates shared buffer for vip and vpe
 *
 * @param:  vpe struct vpe pointer
 *
 * @return: 0 on success 
 *****************************************************************************
*/
int allocate_shared_buffers(struct vpe *vpe)
{
	int i;

	shared_bufs = disp_get_vid_buffers(vpe->disp, NUMBUF, vpe->src.fourcc,
					   vpe->src.width, vpe->src.height);
	if (!shared_bufs)
		pexit("allocating shared buffer failed\n");

    	for (i = 0; i < NUMBUF; i++) {
		/** Get DMABUF fd for corresponding buffer object */
		vpe->input_buf_dmafd[i] = omap_bo_dmabuf(shared_bufs[i]->bo[0]);
		shared_bufs[i]->fd[0] = vpe->input_buf_dmafd[i];
		dprintf("vpe->input_buf_dmafd[%d] = %d\n", i, vpe->input_buf_dmafd[i]);
	}

	return 0;
}

/**
 *****************************************************************************
 * @brief:  queue shared buffer to vip
 *
 * @param:  vpe struct vpe pointer
 * @param:  index int
 *
 * @return: 0 on success 
 *****************************************************************************
*/
int vip_qbuf(struct vpe *vpe, int index)
{
	int ret;
	struct v4l2_buffer buf;

	dprintf("vip buffer queue\n");

	memset(&buf, 0, sizeof buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_DMABUF;
	buf.index = index;
	buf.m.fd = vpe->input_buf_dmafd[index];

	ret = ioctl(vipfd, VIDIOC_QBUF, &buf);
	if (ret < 0)
		pexit( "vip: QBUF failed: %s, index = %d\n", strerror(errno), index);

	return 0;
}

/**
 *****************************************************************************
 * @brief:  dequeue shared buffer from vip
 *
 * @return: buf.index int 
 *****************************************************************************
*/
int vip_dqbuf(struct vpe * vpe)
{
	int ret;
	struct v4l2_buffer buf;
	
	dprintf("vip dequeue buffer\n");
	
	memset(&buf, 0, sizeof buf);

	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_DMABUF;
	ret = ioctl(vipfd, VIDIOC_DQBUF, &buf);
	if (ret < 0)
		pexit("vip: DQBUF failed: %s\n", strerror(errno));

	dprintf("vip: DQBUF idx = %d, field = %s\n", buf.index,
		buf.field == V4L2_FIELD_TOP? "Top" : "Bottom");
	vpe->field = buf.field;

	return buf.index;
}

int main(int argc, char *argv[])
{
	int i, index = -1, count = 0 ;

	struct	vpe *vpe;

	if (argc != 11) {
		printf (
		"USAGE : <SRCWidth> <SRCHeight> <SRCFormat> "
			"<DSTWidth> <DSTHeight> <DSTformat> "
			"<interlace> <translen> -s <connector_id>:<mode>\n");

		return 1;
	}

	/** Open the device */
	vpe = vpe_open();

	vpe->src.width	= atoi (argv[1]);
	vpe->src.height	= atoi (argv[2]);
	describeFormat (argv[3], &vpe->src);

	/* Force input format to be single plane */
	vpe->src.coplanar = 0;

	vpe->dst.width	= atoi (argv[4]);
	vpe->dst.height = atoi (argv[5]);
	describeFormat (argv[6], &vpe->dst);

	vpe->deint = atoi (argv[7]);
	vpe->translen = atoi (argv[8]);

	dprintf ("Input  @ %d = %d x %d , %d\nOutput = %d x %d , %d\n",
		fin,  vpe->src.width, vpe->src.height, vpe->src.fourcc,
		vpe->dst.width, vpe->dst.height, vpe->dst.fourcc);

	if (	vpe->src.height < 0 || vpe->src.width < 0 || vpe->src.fourcc < 0 || \
		vpe->dst.height < 0 || vpe->dst.width < 0 || vpe->dst.fourcc < 0) {
		pexit("Invalid parameters\n");
	}

	vipfd = open ("/dev/video1",O_RDWR);
	if (vipfd < 0)
		pexit("Can't open camera: /dev/video1\n");
	
	printf("vip open success!!!\n");

        vpe->disp = disp_open(argc, argv);
	if(!vpe->disp)
		pexit("Can't open display\n");

	vpe->disp->multiplanar = false;

	dprintf("display open success!!!\n");

	vip_set_format(vpe->src.width, vpe->src.height, vpe->src.fourcc);

	vip_reqbuf();

	vpe_input_init(vpe);

	allocate_shared_buffers(vpe);

	vpe_output_init(vpe);

	for (i = 0; i < NUMBUF; i++)
		vip_qbuf(vpe, i);

	for (i = 0; i < NUMBUF; i++)
		vpe_output_qbuf(vpe, i);

        /*************************************
                Data is ready Now
        *************************************/

	stream_ON(vipfd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	stream_ON(vpe->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

	vpe->field = V4L2_FIELD_ANY;
	while (1)
	{
		index = vip_dqbuf(vpe);

		vpe_input_qbuf(vpe, index);

		if (!doOnce) {
			count ++;
			for (i = 1; i <= NUMBUF; i++) {
				/** To star deinterlace, minimum 3 frames needed */
				if (vpe->deint && count != 3) {
					index = vip_dqbuf(vpe);
					vpe_input_qbuf(vpe, index);
				} else {
					stream_ON(vpe->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
					doOnce = 1;
					printf("streaming started...\n");
					break;
				}
				count ++;
			}
		}

		index = vpe_output_dqbuf(vpe);
		display_buffer(vpe, index);
		vpe_output_qbuf(vpe, index);

		index = vpe_input_dqbuf(vpe);
		vip_qbuf(vpe, index);
	}
	
	/** Driver cleanup */
	stream_OFF(vipfd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	stream_OFF(vpe->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	stream_OFF(vpe->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

	disp_close(vpe->disp);
	vpe_close(vpe);
	close(vipfd);
	
	return 0;
}
