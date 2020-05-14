/*
 * Copyright (C) 2012 Texas Instruments
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
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <libdce.h>
#include <xf86drm.h>
#include <omap_drm.h>
#include <omap_drmif.h>
#include <signal.h>
#include <unistd.h>

#include <pthread.h>

#include "util.h"
#include "demux.h"

/* Padding for width as per Codec Requirement (for h264) */
#define PADX  32
/* Padding for height as per Codec requirement (for h264)*/
#define PADY  24
/* Padding for height as per Codec requirement (for MPEG4)*/
#define PADY_MPEG4  32
/* omap drm device handle */
struct omap_device *dev = NULL;

struct decoder {
	struct display *disp;
	struct demux *demux;
	struct buffer *framebuf;
	Engine_Handle engine;
	VIDDEC3_Handle codec;
	VIDDEC3_Params *params;
	VIDDEC3_DynamicParams *dynParams;
	VIDDEC3_Status *status;
	XDM2_BufDesc *inBufs;
	XDM2_BufDesc *outBufs;
	VIDDEC3_InArgs *inArgs;
	VIDDEC3_OutArgs *outArgs;
	char *input;
	struct omap_bo *input_bo;
	int input_sz, uv_offset;
	int padded_width;
	int padded_height;
	int num_outBuf;
	size_t *outBuf_fd;
	suseconds_t tdisp;
	int id;
	struct buffer *lastOutBuf;
	int outBufsInUseFlag;
};


struct decoder *decoders[8] = {};
int ndecoders = 0;

/* When true, do not actually call VIDDEC3_process. For benchmarking. */
static int no_process = 0;
static int inloop = 0;

/* When true, loop at end of playback. */
static int loop = 0;

static void
usage(char *name)
{
	MSG("Usage: %s [OPTIONS] INFILE", name);
	MSG("Test of viddec3 decoder.");
	MSG("");
	MSG("viddec3test options:");
	MSG("\t-h, --help: Print this help and exit.");
	MSG("\t--loop\tRestart playback at end of stream.");
	MSG("\t--inloop\tRestart playback at end of stream along with decoder reinitialization.");
	MSG("\t--no-process\tDo not actually call VIDDEC3_process method. For benchmarking.");
	MSG("");
	disp_usage();
}

static void
decoder_close(struct decoder *decoder)
{
	if(!decoder) return;
	/* free output buffers allocated by display */
	if(inloop < 2 && decoder->disp) disp_free_buffers(decoder->disp,decoder->num_outBuf);

	if (decoder->status)         dce_free(decoder->status);
	if (decoder->params)         dce_free(decoder->params);
	if (decoder->dynParams)      dce_free(decoder->dynParams);
	if (decoder->inBufs) {
		dce_buf_unlock(1, &(decoder->inBufs->descs[0].buf));
		close(decoder->inBufs->descs[0].buf);
		dce_free(decoder->inBufs);
	}
	if (decoder->outBufs)        dce_free(decoder->outBufs);
	if (decoder->inArgs)         dce_free(decoder->inArgs);
	if (decoder->outArgs)        dce_free(decoder->outArgs);
	if (decoder->codec)          VIDDEC3_delete(decoder->codec);
	if (decoder->engine)         Engine_close(decoder->engine);
    if (decoder->input_bo)       omap_bo_del(decoder->input_bo);
	if (decoder->outBuf_fd)	     free(decoder->outBuf_fd);
	if(inloop < 2) {
		if (dev)		             dce_deinit(dev);
		if (decoder->demux)          demux_deinit(decoder->demux);
		if (decoder->disp)           disp_close(decoder->disp);
		if(decoder) {
		  free(decoder);
		 }
	}
}

static struct decoder *
decoder_open(int argc, char **argv)
{
	static struct decoder *decoder = NULL;
	char *infile = NULL;
	int i;
	static int width, height, padded_width, padded_height;
	Engine_Error ec;
	XDAS_Int32 err;

    if(inloop < 2) {
		decoder = calloc(1, sizeof(*decoder));
		if (!decoder)
			return NULL;

		MSG("%p: Opening Display..", decoder);
		decoder->disp = disp_open(argc, argv);
		if (!decoder->disp)
			goto usage;

	    /* loop thru args, find input file.. */
		for (i = 1; i < argc; i++) {
			int fd;
			    if (!argv[i]) {
				    continue;
			}
			fd = open(argv[i], 0);
			if (fd > 0) {
				infile = argv[i];
				argv[i] = NULL;
				close(fd);
				break;
			}
			break;
		}
		if (check_args(argc, argv) || !infile)
			goto usage;
		MSG("%p: Opening Demuxer..", decoder);
		decoder->demux = demux_init(infile, &width, &height);
		if (!decoder->demux) {
			ERROR("%p: could not open demuxer", decoder);
			goto fail;
		}
		MSG("%p: infile=%s, width=%d, height=%d", decoder, infile, width, height);

		/* calculate output buffer parameters: */
		width  = ALIGN2 (width, 4);        /* round up to macroblocks */
		height = ALIGN2 (height, 4);       /* round up to macroblocks */
		if (decoder->demux->cc->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
			padded_width = width;
			padded_height= height;
		} else if (decoder->demux->cc->codec_id == AV_CODEC_ID_MPEG4) {
			padded_width  = ALIGN2 (width + PADX, 7);
			padded_height = height + PADY_MPEG4;
		} else {
			padded_width  = ALIGN2 (width + (2*PADX), 7);
			padded_height = height + 4*PADY;
		}
		decoder->num_outBuf   = MIN(16, 32768 / ((width/16) * (height/16))) + 3;
		decoder->padded_width = padded_width;
		decoder->padded_height = padded_height;
		MSG("%p: padded_width=%d, padded_height=%d, num_buffers=%d",
			decoder, padded_width, padded_height, decoder->num_outBuf);
		dce_set_fd(decoder->disp->fd);
		dev = dce_init();
		if(dev == NULL) {
			ERROR("%p: dce init failed", dev);
			goto fail;
		}
		decoder->framebuf = disp_get_fb(decoder->disp);
		if (! disp_get_vid_buffers(decoder->disp, decoder->num_outBuf,
				FOURCC_STR("NV12"), decoder->padded_width, decoder->padded_height)) {
			ERROR("%p: could not allocate buffers", decoder);
			goto fail;
		}
		if(inloop) inloop = 2; /*Don't bother about looping if not asked to*/
    }

	if (!decoder->disp->multiplanar) {
		decoder->uv_offset = padded_width * padded_height;
		decoder->outBuf_fd = malloc(sizeof(int)*decoder->num_outBuf);
		MSG("%p: uv_offset=%d", decoder, decoder->uv_offset);
	}
	else{
		decoder->outBuf_fd = malloc(sizeof(int)*(decoder->num_outBuf*2));
	}

	decoder->input_sz = width * height;
	decoder->input_bo = omap_bo_new(decoder->disp->dev,
			decoder->input_sz, OMAP_BO_WC);
	decoder->input = omap_bo_map(decoder->input_bo);



	MSG("%p: Opening Engine..", decoder);
	decoder->engine = Engine_open("ivahd_vidsvr", NULL, &ec);
	if (!decoder->engine) {
		ERROR("%p: could not open engine", decoder);
		goto fail;
	}

	decoder->params = dce_alloc(sizeof(IVIDDEC3_Params));
	decoder->params->size = sizeof(IVIDDEC3_Params);

	decoder->params->maxWidth         = width;
	decoder->params->maxHeight        = height;
	decoder->params->maxFrameRate     = 30000;
	decoder->params->maxBitRate       = 10000000;
	decoder->params->dataEndianness   = XDM_BYTE;
	decoder->params->forceChromaFormat= XDM_YUV_420SP;
	decoder->params->operatingMode    = IVIDEO_DECODE_ONLY;
	decoder->params->displayDelay     = IVIDDEC3_DISPLAY_DELAY_AUTO;
	decoder->params->displayBufsMode  = IVIDDEC3_DISPLAYBUFS_EMBEDDED;
	MSG("displayBufsMode: %d", decoder->params->displayBufsMode);
	decoder->params->inputDataMode    = IVIDEO_ENTIREFRAME;
	decoder->params->metadataType[0]  = IVIDEO_METADATAPLANE_NONE;
	decoder->params->metadataType[1]  = IVIDEO_METADATAPLANE_NONE;
	decoder->params->metadataType[2]  = IVIDEO_METADATAPLANE_NONE;
	decoder->params->numInputDataUnits= 0;
	decoder->params->outputDataMode   = IVIDEO_ENTIREFRAME;
	decoder->params->numOutputDataUnits = 0;
	decoder->params->errorInfoMode    = IVIDEO_ERRORINFO_OFF;

	if (decoder->demux->cc->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
            decoder->codec = VIDDEC3_create(decoder->engine,
                                        "ivahd_mpeg2vdec", decoder->params);
        }
        else if (decoder->demux->cc->codec_id == AV_CODEC_ID_H264) {
            decoder->codec = VIDDEC3_create(decoder->engine,
                                        "ivahd_h264dec", decoder->params);
        }
	else if (decoder->demux->cc->codec_id == AV_CODEC_ID_MPEG4) {
		decoder->demux->first_in_buff = 1;
		decoder->codec = VIDDEC3_create(decoder->engine,
                                        "ivahd_mpeg4dec", decoder->params);
	}

	if (!decoder->codec) {
		ERROR("%p: could not create codec", decoder);
		goto fail;
	}

	decoder->dynParams = dce_alloc(sizeof(IVIDDEC3_DynamicParams));
	decoder->dynParams->size = sizeof(IVIDDEC3_DynamicParams);

	decoder->dynParams->decodeHeader  = XDM_DECODE_AU;

	/*Not Supported: Set default*/
	decoder->dynParams->displayWidth  = 0;
	decoder->dynParams->frameSkipMode = IVIDEO_NO_SKIP;
	decoder->dynParams->newFrameFlag  = XDAS_TRUE;

	decoder->status = dce_alloc(sizeof(IVIDDEC3_Status));
	decoder->status->size = sizeof(IVIDDEC3_Status);

	err = VIDDEC3_control(decoder->codec, XDM_SETPARAMS,
			decoder->dynParams, decoder->status);
	if (err) {
		ERROR("%p: fail: %d", decoder, err);
		goto fail;
	}

	/* not entirely sure why we need to call this here.. just copying omx.. */
	err = VIDDEC3_control(decoder->codec, XDM_GETBUFINFO,
			decoder->dynParams, decoder->status);
	if (err) {
		ERROR("%p: fail: %d", decoder, err);
		goto fail;
	}

	decoder->inBufs = dce_alloc(sizeof(XDM2_BufDesc));
	decoder->inBufs->numBufs = 1;
	decoder->inBufs->descs[0].buf =	(XDAS_Int8 *)omap_bo_dmabuf(decoder->input_bo);
	dce_buf_lock(1, &(decoder->inBufs->descs[0].buf));
	decoder->inBufs->descs[0].bufSize.bytes = omap_bo_size(decoder->input_bo);
	decoder->inBufs->descs[0].memType = XDM_MEMTYPE_RAW;


	decoder->outBufs = dce_alloc(sizeof(XDM2_BufDesc));
	decoder->outBufs->numBufs = 2;
	decoder->outBufs->descs[0].memType = XDM_MEMTYPE_RAW;
	decoder->outBufs->descs[1].memType = XDM_MEMTYPE_RAW;

	decoder->inArgs = dce_alloc(sizeof(IVIDDEC3_InArgs));
	decoder->inArgs->size = sizeof(IVIDDEC3_InArgs);

	decoder->outArgs = dce_alloc(sizeof(IVIDDEC3_OutArgs));
	decoder->outArgs->size = sizeof(IVIDDEC3_OutArgs);

	decoder->outBufsInUseFlag = XDAS_FALSE;

	decoder->tdisp = mark(NULL);

	return decoder;

usage:
	usage(argv[0]);
fail:
	if(inloop) inloop = 1; /*Error case: delete everything*/
	if (decoder)
		decoder_close(decoder);
	return NULL;
}

static int
decoder_process(struct decoder *decoder)
{
	XDM2_BufDesc *inBufs = decoder->inBufs;
	XDM2_BufDesc *outBufs = decoder->outBufs;
	VIDDEC3_InArgs *inArgs = decoder->inArgs;
	VIDDEC3_OutArgs *outArgs = decoder->outArgs;
	struct buffer *buf;
	int freeBufCount =0;
	int i, n;
	XDAS_Int32 err;
	int eof = 0; /* end of file flag */

	/* demux; in loop mode, we can do two tries at the end of the stream. */
	for (i = 0; i < 2; i++) {
		n = demux_read(decoder->demux, decoder->input, decoder->input_sz);
		if (n) {
			if(decoder->outBufsInUseFlag == XDAS_FALSE){
				buf = disp_get_vid_buffer(decoder->disp);
				if (!buf) {
					ERROR("%p: fail: out of buffers", decoder);
					return -1;
				}
				decoder->lastOutBuf = buf;

                                inArgs->inputID = (XDAS_Int32)buf;
                                outBufs->descs[0].buf = buf->fd[0];
                                outBufs->descs[1].buf = (buf->multiplanar) ?buf->fd[1]:(XDAS_Int8 *)((outBufs->descs[0].buf));

                                if(buf->multiplanar){
                                    decoder->outBuf_fd[0] = buf->fd[0];
                                    decoder->outBuf_fd[1] = buf->fd[1];
                                    dce_buf_lock(2,decoder->outBuf_fd);
                                }
                                else{
                                    decoder->outBuf_fd[0] = buf->fd[0];
                                    dce_buf_lock(1,decoder->outBuf_fd);
                                }
                                decoder->outBufs->descs[0].bufSize.bytes =decoder->padded_width*decoder->padded_height;
                                decoder->outBufs->descs[1].bufSize.bytes = decoder->padded_width* (decoder->padded_height/2);
			}
			else{
				/*For second field decoding, send last output buffer
				  decoder uses buffer which was sent for first field
				  decoding. No need to pass the buffer. This decision is
				  taken based on outArgs->outBufsInUseFlag
				 */
				buf = decoder->lastOutBuf;
			}
			inBufs->descs[0].bufSize.bytes = n;
			inArgs->numBytes = n;
			DBG("%p: push: %d bytes (%p)", decoder, n, buf);
		} else {
			/* end of input.. do we need to flush? */
			MSG("%p: end of input", decoder);

			/* In loop mode: rewind and retry once. */
			if (loop && i == 0) {
				int err = demux_rewind(decoder->demux);
				if (err < 0) {
					ERROR("%p: demux_rewind returned error: %d", decoder, err);
					return -1;
				}
				MSG("%p: rewound.", decoder);
				if(!inloop) continue;
			}
			eof = 1; /* set the flag for end of file to 1 */
			/* Control call call with XDM_FLUSH command */
			err = VIDDEC3_control(decoder->codec, XDM_FLUSH,
					decoder->dynParams, decoder->status);
			inBufs->numBufs = 0;
			outBufs->numBufs = 0;
			inArgs->inputID = 0;
		}
		break;
	}


	do {
		if (no_process) {
			/* Do not process. This is for benchmarking. We need to "fake"
			 * the outArgs. */
			outArgs->outputID[0] = 0;
			outArgs->freeBufID[0] = 0;
			if(!eof) {
				outArgs->outputID[0] = buf;
				outArgs->freeBufID[0] = buf;
			}
			outArgs->outputID[1] = NULL;
			outArgs->freeBufID[1] = NULL;
			outArgs->outBufsInUseFlag = 0;

		} else {
			suseconds_t tproc;
			tproc = mark(NULL);
			err = VIDDEC3_process(decoder->codec, inBufs, outBufs, inArgs, outArgs);
			DBG("%p: processed returned in: %ldus", decoder, (long int)mark(&tproc));
			if (err) {
				ERROR("%p: process returned error: %d", decoder, err);
				ERROR("%p: extendedError: %08x", decoder, outArgs->extendedError);
				if (XDM_ISFATALERROR(outArgs->extendedError) || ( err == DCE_EXDM_UNSUPPORTED ) || ( err == DCE_EIPC_CALL_FAIL ) || ( err == DCE_EINVALID_INPUT ))
					return -1;
			}
		}

		for (i = 0; outArgs->outputID[i]; i++) {
			/* calculate offset to region of interest */
			XDM_Rect *r = &(outArgs->displayBufs.bufDesc[0].activeFrameRegion);

			/* get the output buffer and write it to file */
			buf = (struct buffer *)outArgs->outputID[i];
			if(!no_process)
                               disp_post_vid_buffer(decoder->disp, buf,
					r->topLeft.x, r->topLeft.y,
					r->bottomRight.x - r->topLeft.x,
					r->bottomRight.y - r->topLeft.y);
		}

		for (i = 0; outArgs->freeBufID[i]; i++) {
			buf = (struct buffer *)outArgs->freeBufID[i];
			disp_put_vid_buffer(decoder->disp, buf);

			if(buf->multiplanar){
				decoder->outBuf_fd[freeBufCount++] = buf->fd[0];
				decoder->outBuf_fd[freeBufCount++] = buf->fd[1];
			}
			else{
				decoder->outBuf_fd[freeBufCount++] = buf->fd[0];
				DBG("FreeBufID: %p fd:%d\n", outArgs->freeBufID[i], buf->fd[0]);
			}
		}

		if(freeBufCount){
			dce_buf_unlock(freeBufCount,decoder->outBuf_fd);
			freeBufCount =0;
		}

		decoder->outBufsInUseFlag = outArgs->outBufsInUseFlag;

	} while ((err == 0) && eof && !no_process);

	return (inBufs->numBufs > 0) ? 0 : -1;
}

/* Returns 1 (true) if the mutex is unlocked, which is the
 * thread's signal to terminate.
  */

pthread_mutex_t mtx;

int needQuit()
 {
   switch(pthread_mutex_trylock(&mtx)) {
    case 0: /* if we got the lock, unlock and return 1 (true) */
	      pthread_mutex_unlock(&mtx);
          return 1;
    case EBUSY: /* return 0 (false) if the mutex was locked */
	      return 0;
   }
  return 1;
  }

void *decode_stream(void *decoderHandle)
{
	int ret = 0;
	struct decoder *decoder = (struct decoder*)decoderHandle;
    int n = 0;
    if(!decoder) goto exit;

    while((ret = decoder_process(decoder)) == 0) {
	   if(needQuit()){
              inloop = 1;
	      break;
	    }
	 }
    if((ret != -1 && ret != 0) && inloop) inloop = 1; /*Assuming Good case. Otherwise logic gets messy*/
    int i = decoder->id;
	decoder_close(decoder);
	decoders[i]=NULL;
exit:
	return NULL;
}


static void sig_handler(int signo)
{
  if (signo == SIGINT) {
	  int i=0;
	  pthread_mutex_unlock(&mtx);
	  sleep(1);
	  exit(0);
  }
}


int
main(int argc, char **argv)
{

   int i, first = 0;

   struct sigaction sa;

   sa.sa_handler = sig_handler;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = 0;

   if (sigaction(SIGINT, &sa, NULL) == -1) {
     ERROR ("\nDid not catch  SIGINT\n");
   }
   signal(SIGINT,sig_handler);

   pthread_mutex_init(&mtx,NULL);
   pthread_mutex_lock(&mtx);

    for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(argv[0]);
			exit(0);

		} else if (!strcmp(argv[i], "--loop")) {
			loop = 1;
			argv[i] = NULL;

		} else if (!strcmp(argv[i], "--no-process")) {
			no_process = 1;
			argv[i] = NULL;

		} else if (!strcmp(argv[i], "--inloop")) {
			inloop = 1; // this means inloop is detected
			DBG("detected inloop = %d\n", inloop);
			loop = 1; //we want rewind as well
			argv[i] = NULL;
		} else if (!strcmp(argv[i], "--")) {
			argv[first] = argv[0];
			decoders[ndecoders] = decoder_open(i - first, &argv[first]);
			decoders[ndecoders]->id = ndecoders;
			ndecoders++;
			first = i;
		}
	}

	argv[first] = argv[0];
	argc = i - first;

	if(ndecoders) {
	    decoders[ndecoders] = decoder_open(argc ,&argv[first]);
		decoders[ndecoders]->id = ndecoders;
		ndecoders++;
	}

	if (ndecoders > 1) {
		pthread_t threadIds[8];

		for (i = 0; i < ndecoders; i++) {
			if (decoders[i]) {
				int ret = pthread_create(&threadIds[i], NULL,
						&decode_stream, decoders[i]);
				if (ret != 0)
					ERROR("%p: creation of pthread, error: %d",
							decoders[i], ret);
			}
		}

		for (i = 0; i < ndecoders; i++) {
			pthread_join(threadIds[i], NULL);
		}
	}
	else {
		int itr = 0;
		do {
			decoders[0] = decoder_open(argc, &argv[first]);
			decoders[0]->id = 0;
			ndecoders++;
			decode_stream(decoders[0]);
			if (inloop) {
				MSG("=================Iteration %d complete =============== %d\n", ++itr);
			}
		}while(inloop);
	}

	return 0;
}
