/*
 * Copyright (c) 2011, Texas Instruments Incorporated
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * *  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * *  Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif


#include "demux.h"
#include "util.h"

char mpeg4head[45] = {0,0,0,0};
int get_esds_offset(const char *filename, struct demux *demux);

static AVFormatContext *
open_file(const char *filename)
{
	AVFormatContext *afc = NULL;
	int err = avformat_open_input(&afc, filename, NULL, NULL);

	if (!err)
		err = avformat_find_stream_info(afc, NULL);

	if (err < 0) {
		ERROR("%s: lavf error %d", filename, err);
		exit(1);
	}

	av_dump_format(afc, 0, filename, 0);

	return afc;
}

static AVStream *
find_stream(AVFormatContext *afc)
{
	AVStream *st = NULL;
	unsigned int i;

	for (i = 0; i < afc->nb_streams; i++) {
		if (afc->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO && !st)
			st = afc->streams[i];
		else
			afc->streams[i]->discard = AVDISCARD_ALL;
	}

	return st;
}

static struct demux * open_stream(const char * filename, int *width, int *height)
{
	AVFormatContext *afc = open_file(filename);
	AVStream *st = find_stream(afc);
	AVCodecContext *cc = st->codec;
	AVBitStreamFilterContext *bsf = NULL;
	struct demux *demux;

	if ((cc->codec_id != AV_CODEC_ID_H264) &&  (cc->codec_id != AV_CODEC_ID_MPEG2VIDEO) && ( cc->codec_id !=  AV_CODEC_ID_MPEG4)){
		ERROR("could not open '%s': unsupported codec %d", filename, cc->codec_id);
		return NULL;
	}

	if (cc->extradata && cc->extradata_size > 0 && cc->extradata[0] == 1) {
		MSG("initializing bitstream filter");
		bsf = av_bitstream_filter_init("h264_mp4toannexb");
		if (!bsf) {
			ERROR("could not open '%s': failed to initialize bitstream filter", filename);
			return NULL;
		}
	}

	*width = cc->width;
	*height = cc->height;

	demux = calloc(1, sizeof(*demux));

	demux->afc = afc;
	demux->cc  = cc;
	demux->st  = st;
	demux->bsf = bsf;
	demux->first_in_buff = 0;

	return demux;
}

struct demux * demux_init(const char * filename, int *width, int *height)
{
	struct demux *demux;

	av_register_all();
	avcodec_register_all();
	demux = open_stream(filename, width, height);
	if ((demux != NULL) && (demux->cc->codec_id ==  AV_CODEC_ID_MPEG4)) {
		if(get_esds_offset(filename, demux))
			return NULL;
	}

	return demux;
}

int get_esds_offset(const char *filename, struct demux *demux) {
       FILE *inputStream;
       int i=0;
       unsigned char *buffer = NULL;
       unsigned long fileLen;
       int esds_index=0;
       inputStream = fopen(filename,"rb");
       if (!inputStream) {
               printf("Unable to open %s\n",filename);
               return -1;
       }
       // Get the length
       fseek(inputStream, 0, SEEK_END);
       fileLen=ftell(inputStream);
       fseek(inputStream, 0, SEEK_SET);
       buffer=malloc(fileLen);
       if (!buffer) {
               printf("Memory error!\n");
               fclose(inputStream);
               return -1;
       }
       // File to buffer
       fread(buffer, fileLen, 1, inputStream);
       fclose(inputStream);
       printf("Buffer starts: %p\n", &buffer[0]);
       printf("Buffer ends: %p\n", &buffer[fileLen]);
       //Determines offset of known_string
       for(i=0; i<fileLen; i++) {
               if((buffer[i] == 0x65) && (buffer[++i] == 0x73))
               if((buffer[++i] == 0x64) && (buffer[i+1] == 0x73)) {
                       printf("data in buffer = %x  %x  %x %d \n",buffer[i],buffer[i+1],buffer[i+2],buffer[i+27]);
                       esds_index = i+27;
                       break;
               }
       }
       demux->esds.length= buffer[esds_index];
       demux->esds.data=malloc(demux->esds.length);
       for(i=1; i<=demux->esds.length; i++) {
               //printf(" index= %x \n",buffer[esds_index+i]);
               demux->esds.data[i-1]=buffer[esds_index+i];
               printf(" index= %x \n",demux->esds.data[i-1]);
       }
       free(buffer);
       return 0;
}



int demux_read(struct demux *demux, char *input, int size)
{
	AVPacket pk = {};

	while (!av_read_frame(demux->afc, &pk)) {
		if (pk.stream_index == demux->st->index) {
			uint8_t *buf;
			int bufsize;

			if (demux->bsf) {
				int ret;
				ret = av_bitstream_filter_filter(demux->bsf, demux->cc,
						NULL, &buf, &bufsize, pk.data, pk.size, 0);
				if (ret < 0) {
					ERROR("bsf error: %d", ret);
					return 0;
				}
			} else {
				buf     = pk.data;
				bufsize = pk.size;
			}

			if (bufsize > size)
				bufsize = size;

                        if(demux->first_in_buff == 1) {
                               memcpy(input, demux->esds.data, demux->esds.length);
                               memcpy(input + demux->esds.length, buf, bufsize);
                               demux->first_in_buff =0;
                               bufsize = bufsize + demux->esds.length;
			}
			else {
                               memcpy(input, buf, bufsize);
			}

			if (demux->bsf)
				av_free(buf);

			av_free_packet(&pk);

			return bufsize;
		}
		av_free_packet(&pk);
	}

	return 0;
}

int demux_rewind(struct demux *demux)
{
	return av_seek_frame(demux->afc, demux->st->index, 0, AVSEEK_FLAG_FRAME);
}

void demux_deinit(struct demux *demux)
{
	avformat_close_input(&demux->afc);
	if (demux->bsf)
		av_bitstream_filter_close(demux->bsf);
	free(demux->esds.data);
	free(demux);
}
