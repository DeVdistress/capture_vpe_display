/*
 * Copyright (c) 2013, Texas Instruments Incorporated
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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <signal.h>


#include <omap_drm.h>
#include <omap_drmif.h>
#include "libdce.h"

#include <ti/sdo/ce/Engine.h>
#include <ti/sdo/ce/video2/videnc2.h>
#include <ti/sdo/codecs/h264enc/ih264enc.h>
#include <ti/sdo/codecs/mpeg4enc/impeg4enc.h>
#include <ti/sdo/codecs/jpegvenc/ijpegenc.h>


//#define PRINT_DEBUG

#define ERROR(FMT, ...)  printf("%s:%d:\t%s\terror: " FMT "\n", __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__)
// enable below to print debug information
#ifdef PRINT_DEBUG
#define MSG(FMT, ...)  printf("%s:%d:\t%s\tdebug: " FMT "\n", __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#else
#define MSG(FMT, ...)
#endif
#define INFO(FMT, ...)  printf("%s:%d:\t%s\tinfo: " FMT "\n", __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__)
#define MIN(a, b)        (((a) < (b)) ? (a) : (b))

/* align x to next highest multiple of 2^n */
#define ALIGN2(x, n)   (((x) + ((1 << (n)) - 1)) & ~((1 << (n)) - 1))

// Profile the init and encode calls
//#define PROFILE_TIME

// Getting codec version through XDM_GETVERSION
#define GETVERSION

enum {
    IVAHD_H264_ENCODE,
    IVAHD_MPEG4_ENCODE,
    IVAHD_H263_ENCODE,
    IVAHD_JPEG_ENCODE
};

enum {
	DCE_ENC_TEST_H264  = 1,
	DCE_ENC_TEST_MPEG4 = 2,
	DCE_ENC_TEST_H263 = 3,
        DCE_ENC_TEST_JPEG = 4
};

enum {
	MEMTYPE_INVALID = -1,
	MEMTYPE_OMAPDRM = 0,
};

typedef struct _bufferDesc{
	void *y_bo; //buffer object for y pointer
	void *uv_bo; //buffer object for uv pointer
	int fdy; // y dma buf
	int fduv; // uv dma buf
	int sizey; // size of y
	int sizeuv; // size of uv
	void *ybuff; // Virtual addresses of y
	void *uvbuff; // virtual address of uv
}bufferdesc;

typedef struct encoderObj{
	int drmfd; //DRM device fd
	void *dev; // Device handle
	char *in_pattern; //Input filename with full path
	char *out_pattern; //Output filename with full path
	FILE *fin; // Input stream
	FILE *fout; // output stream
	int width; //Frame width
	int height; // Frame height
	int codectype; // H264 or MPEG4 or H263
	int profile;
	int level;
	int nframes; // Number of frames to write
	int memtype; // Memory allocation scheme (OMAPDRM)
	int fps; //Frame rate
	int bps; //bitrate
	bufferdesc buf; // Holds input buffer address
	struct omap_bo *output_bo;
	unsigned char *cdata;
	struct omap_bo *mv_bo;

	int padded_width;
	int padded_height;
	int num_buffers;
	Engine_Handle            engine;
	VIDENC2_Handle           codec;
	VIDENC2_Params          *params;
	VIDENC2_DynamicParams   *dynParams;
	VIDENC2_Status          *status;
	VIDENC2_DynamicParams   *dynParams1;
	VIDENC2_Status          *status1;
	IVIDEO2_BufDesc         *inBufs;
	XDM2_BufDesc            *outBufs;
	VIDENC2_InArgs          *inArgs;
	VIDENC2_OutArgs         *outArgs;

	// H.264 specific
	IH264ENC_InArgs          *h264enc_inArgs;
	IH264ENC_OutArgs         *h264enc_outArgs;
	IH264ENC_Params          *h264enc_params;
	IH264ENC_DynamicParams   *h264enc_dynParams;
	IH264ENC_Status          *h264enc_status;

	// MPEG4/H.263 specific
	IMPEG4ENC_InArgs          *mpeg4enc_inArgs;
	IMPEG4ENC_OutArgs         *mpeg4enc_outArgs;
	IMPEG4ENC_Params          *mpeg4enc_params;
	IMPEG4ENC_DynamicParams   *mpeg4enc_dynParams;
	IMPEG4ENC_Status          *mpeg4enc_status;

        // JPEG specific
        IJPEGVENC_InArgs          *jpegenc_inArgs;
        IJPEGVENC_OutArgs         *jpegenc_outArgs;
        IJPEGVENC_Params          *jpegenc_params;
        IJPEGVENC_DynamicParams   *jpegenc_dynParams;
        IJPEGVENC_Status          *jpegenc_status;

}encoder;
/*
 * A very simple VIDENC2 client which will encode raw (unstrided) NV12 YUV frames
 * and write out to either h264, MPEG4, or H.263 format.
 */

static int encoder_init(encoder *enc);
static int encoder_deinit(encoder *enc);
static void usage(char **argv);


static int read_NV12frame(encoder *enc)
{
	int numbytes =  0;
	if(!enc->fin){
		enc->fin = fopen(enc->in_pattern, "r+");
		if(!enc->fin){
			ERROR("Could not open input file %s\n", enc->in_pattern);
			goto bail;
		}
	}
	numbytes = fread(enc->buf.ybuff, 1, enc->width * enc->height, enc->fin);
	numbytes += fread(enc->buf.uvbuff, 1, (enc->width * enc->height) / 2, enc->fin);

	return numbytes;
bail:
	return -1;
}


/* helper to write one frame of output */
int write_output(encoder *enc, int bytesToWrite)
{
	int nbytes = -1;
	if(!enc->fout){
		enc->fout = fopen(enc->out_pattern, "wb+");
		if(!enc->fout){
			ERROR("Could not open file for output %s", enc->out_pattern);
			goto fail;
		}
	}
	nbytes = fwrite(enc->cdata, 1, bytesToWrite, enc->fout);
    return nbytes;
fail:
	return -1;
}


static int parse_codecinfo(char *argv[], encoder *enc)
{
/*
 * Configuration based on the input parameters
 */
	const char *argcodec	= argv[8];
	const char *profile		= argv[9];
	const char *level		= argv[10];

	enc->level = atoi(level);
    if( (!(strcmp(argcodec, "h264"))) ) {
        enc->codectype = DCE_ENC_TEST_H264;
        if( (!(strcmp(profile, "baseline"))) ) {
            enc->profile = IH264_BASELINE_PROFILE;
        } else if( (!(strcmp(profile, "high"))) ) {
            enc->profile = IH264_HIGH_PROFILE;
        } else {
            ERROR("Wrong profile value. Please use: baseline or high.\n");
			usage(argv);
            return -1;
        }

        switch(enc->level) {
            case IH264_LEVEL_10 :
            case IH264_LEVEL_1b :
            case IH264_LEVEL_11 :
            case IH264_LEVEL_12 :
            case IH264_LEVEL_13 :
            case IH264_LEVEL_20 :
            case IH264_LEVEL_21 :
            case IH264_LEVEL_22 :
            case IH264_LEVEL_30 :
            case IH264_LEVEL_31 :
            case IH264_LEVEL_32 :
            case IH264_LEVEL_40 :
            case IH264_LEVEL_41 :
            case IH264_LEVEL_42 :
            case IH264_LEVEL_50 :
            case IH264_LEVEL_51 :
                MSG("Acceptable H.264 level value = %d\n", enc->level);
                break;
            default :
                ERROR("Wrong level value. Please use the correct level value for H.264\n");
				usage(argv);
                return -1;
        }
    } else if( !(strcmp(argcodec, "mpeg4"))) {

        enc->codectype = DCE_ENC_TEST_MPEG4;

        if( (!(strcmp(profile, "simple"))) ) {
            enc->profile = 3;
        } else {
            ERROR("Wrong profile value. Please use: simple\n");
			usage(argv);
            return -1;
        }

        switch(enc->level) {
            case IMPEG4ENC_SP_LEVEL_0 :
            case IMPEG4ENC_SP_LEVEL_0B :
            case IMPEG4ENC_SP_LEVEL_1 :
            case IMPEG4ENC_SP_LEVEL_2 :
            case IMPEG4ENC_SP_LEVEL_3 :
            case IMPEG4ENC_SP_LEVEL_4A :
            case IMPEG4ENC_SP_LEVEL_5 :
            case IMPEG4ENC_SP_LEVEL_6 :
                MSG("Acceptable MPEG4 level value = %d\n", enc->level);
                break;
            default :
                ERROR("Wrong level value. Please use the correct level value for MPEG4\n");
				usage(argv);
                return -1;
        }
    } else if( !(strcmp(argcodec, "h263"))) {

        enc->codectype = DCE_ENC_TEST_H263;

        if( (!(strcmp(profile, "simple"))) ) {
            enc->profile = 3;
        } else {
            ERROR("Wrong profile value. Please use: simple\n");
			usage(argv);
            return -1;
        }

        switch(enc->level) {
            case IMPEG4ENC_H263_LEVEL_10 :
            case IMPEG4ENC_H263_LEVEL_20 :
            case IMPEG4ENC_H263_LEVEL_30 :
            case IMPEG4ENC_H263_LEVEL_40 :
            case IMPEG4ENC_H263_LEVEL_45 :
            case IMPEG4ENC_H263_LEVEL_50 :
            case IMPEG4ENC_H263_LEVEL_60 :
            case IMPEG4ENC_H263_LEVEL_70 :
                MSG("Acceptable H263 level value = %d\n", enc->level);
                break;
            default :
                ERROR("Wrong level value. Please use the correct level value for H263\n");
				usage(argv);
                return -1;
        }
    } else if( !(strcmp(argcodec, "jpeg"))) {
        enc->codectype = DCE_ENC_TEST_JPEG;
    } else {
        ERROR("No valid codec entry. Please use: h264 or mpeg4 or h263\n");
		usage(argv);
        return -1;
    }
    MSG("Selected codec: %d\n", enc->codectype);
	return 0;
}


static void usage(char **argv)
{
	printf("usage: %s width height frames_to_write inpattern outpattern fps bitrate(kbps) codec baseline/high level buffertype\n", argv[0]);
	printf("example: %s 1920 1088 300 in.yuv out.h264 15 128 h264 baseline 10 OMAPDRM\n", argv[0]);
	printf("example: %s 176 144 300 in.yuv out.m4v 30 64 mpeg4 simple/baseline 0 OMAPDRM\n", argv[0]);
	printf("example: %s 176 144 300 in.yuv out.m4v 15 150 h263 simple/baseline 0 OMAPDRM\n", argv[0]);
	printf("example: %s 176 144 5 in.yuv out.mjpeg 30 5000 jpeg null 0 OMAPDRM\n", argv[0]);
	printf("Currently supported codecs: h264 or mpeg4 or h263 or jpeg\n");
	printf("Currently supported Buffertypes: OMAPDRM\n");
	printf("Run this command for help on the use case:%s\n", argv[0]);
	return;
}
static int parse_command(int argc, char *argv[], encoder *enc)
{

    if( argc != 12 ) {
		usage(argv);
        return -1;
    }
    enc->width  = atoi(argv[1]);
    enc->height = atoi(argv[2]);
    enc->nframes = atoi(argv[3]);
    enc->in_pattern  = argv[4];
    enc->out_pattern = argv[5];
	enc->fps = atoi(argv[6]);
	enc->bps = atoi(argv[7]) * 1000;
	if(parse_codecinfo(argv, enc))
		return -1;

	if(!strcmp(argv[11], "OMAPDRM")){
		MSG("Only DRM buffer type supported.. Rolling back to OMAPDRM\n");
	}

	enc->memtype =  MEMTYPE_OMAPDRM;
    MSG("Selected buffer: %d\n", enc->memtype);

	return 0;
}


static void init_common_static_params(encoder *enc)
{
	VIDENC2_Params *params = enc->params;
    params->encodingPreset = XDM_USER_DEFINED; //XDM_USER_DEFINED; //XDM_EncodingPreset
    params->rateControlPreset = IVIDEO_USER_DEFINED;
    params->maxHeight = enc->height;
    params->maxWidth = enc->width;
    params->dataEndianness = XDM_BYTE; //XDM_DataFormat
    params->maxBitRate = -1; //IGNORED
    params->minBitRate = 0;
    params->inputChromaFormat = XDM_YUV_420SP; //XDM_ChromaFormat
    params->inputContentType = IVIDEO_PROGRESSIVE; //IVIDEO_ContentType
    params->operatingMode = IVIDEO_ENCODE_ONLY; //IVIDEO_OperatingMode
    params->profile = enc->profile;
    params->level = enc->level;
    params->inputDataMode = IVIDEO_ENTIREFRAME; //IVIDEO_DataMode
    params->outputDataMode = IVIDEO_ENTIREFRAME; //IVIDEO_DataMode
    params->numInputDataUnits = 1;
    params->numOutputDataUnits = 1;
    params->metadataType[0] = IVIDEO_METADATAPLANE_NONE;
    params->metadataType[1] = IVIDEO_METADATAPLANE_NONE;
    params->metadataType[2] = IVIDEO_METADATAPLANE_NONE;
	return;
}

static int init_h264_static_params(encoder *enc)
{
	IH264ENC_Params          *h264enc_params    = NULL;
	enc->inArgs = dce_alloc(sizeof(IH264ENC_InArgs));
	if(!enc->inArgs) goto bail;
	enc->inArgs->size = sizeof(IH264ENC_InArgs);
	enc->outArgs = dce_alloc(sizeof(IH264ENC_OutArgs));
	if(!enc->outArgs) goto bail;
	enc->outArgs->size = sizeof(IH264ENC_OutArgs);
	enc->h264enc_outArgs = (IH264ENC_OutArgs *) enc->outArgs;
	enc->params = dce_alloc(sizeof(IH264ENC_Params));
	if(!enc->params) goto bail;
	enc->params->size = sizeof(IH264ENC_Params);

	init_common_static_params(enc);
	MSG("H.264 Encoding with profile_value %d level %d", enc->profile, enc->level);
	enc->params->maxInterFrameInterval = 1; //1,31 if IVIDEO_ContentType is IVIDEO_PROGRESSIVE
    //Miscellaneous
	h264enc_params    =  enc->h264enc_params = (IH264ENC_Params *) enc->params;
	h264enc_params->interlaceCodingType = IH264_INTERLACE_DEFAULT;
	h264enc_params->bottomFieldIntra = 0;
	h264enc_params->gopStructure = IH264ENC_GOPSTRUCTURE_DEFAULT; // IH264ENC_GOPSTRUCTURE_NONUNIFORM
	h264enc_params->entropyCodingMode = IH264_ENTROPYCODING_DEFAULT; // IH264_ENTROPYCODING_CAVLC - BASE PROFILE
	h264enc_params->transformBlockSize = IH264_TRANSFORM_4x4; // BASE PROFILE
	h264enc_params->log2MaxFNumMinus4 = 10;
	h264enc_params->picOrderCountType = IH264_POC_TYPE_DEFAULT; // IH264_POC_TYPE_0
	h264enc_params->enableWatermark = 0;
	h264enc_params->IDRFrameInterval = 1;
	h264enc_params->pConstantMemory = NULL;
	h264enc_params->maxIntraFrameInterval = 0x7FFFFFFF;
    h264enc_params->debugTraceLevel = 0;
    h264enc_params->lastNFramesToLog = 0;
    h264enc_params->enableAnalyticinfo = 0;
    h264enc_params->enableGMVSei = 0;
    h264enc_params->constraintSetFlags = 20;
    h264enc_params->enableRCDO = 0;
    h264enc_params->enableLongTermRefFrame = IH264ENC_LTRP_NONE;
    h264enc_params->LTRPPeriod = 0;

    //H-P Coding Control Params
    h264enc_params->numTemporalLayer = IH264_TEMPORAL_LAYERS_1;
    h264enc_params->referencePicMarking = IH264_LONG_TERM_PICTURE;
    h264enc_params->reservedParams[0] = 0;
    h264enc_params->reservedParams[1] = 0;
    h264enc_params->reservedParams[2] = 0;

    //rate control params
    h264enc_params->rateControlParams.rateControlParamsPreset = IH264_RATECONTROLPARAMS_USERDEFINED;
    h264enc_params->rateControlParams.scalingMatrixPreset = IH264_SCALINGMATRIX_NONE;
    h264enc_params->rateControlParams.rcAlgo = IH264_RATECONTROL_DEFAULT; // 0
    h264enc_params->rateControlParams.qpI = 28;
    h264enc_params->rateControlParams.qpMaxI = 36;
    h264enc_params->rateControlParams.qpMinI = 10;
    h264enc_params->rateControlParams.qpP = 28;
    h264enc_params->rateControlParams.qpMaxP = 40;
    h264enc_params->rateControlParams.qpMinP = 10;
    h264enc_params->rateControlParams.qpOffsetB = 4;
    h264enc_params->rateControlParams.qpMaxB = 44;
    h264enc_params->rateControlParams.qpMinB = 10;
    h264enc_params->rateControlParams.allowFrameSkip = 0;
    h264enc_params->rateControlParams.removeExpensiveCoeff = 0;
    h264enc_params->rateControlParams.chromaQPIndexOffset = 0;
    h264enc_params->rateControlParams.IPQualityFactor = IH264_QUALITY_FACTOR_DEFAULT; // 0
    h264enc_params->rateControlParams.initialBufferLevel = 64000;
    h264enc_params->rateControlParams.HRDBufferSize = 64000;
    h264enc_params->rateControlParams.minPicSizeRatioI = 0;
    h264enc_params->rateControlParams.maxPicSizeRatioI = 20;
    h264enc_params->rateControlParams.minPicSizeRatioP = 0;
    h264enc_params->rateControlParams.maxPicSizeRatioP = 0;
    h264enc_params->rateControlParams.minPicSizeRatioB = 0;
    h264enc_params->rateControlParams.maxPicSizeRatioB = 0;
    h264enc_params->rateControlParams.enablePRC = 1;
    h264enc_params->rateControlParams.enablePartialFrameSkip = 0;
    h264enc_params->rateControlParams.discardSavedBits = 0;
    h264enc_params->rateControlParams.reserved = 0;
    h264enc_params->rateControlParams.VBRDuration = 8;
    h264enc_params->rateControlParams.VBRsensitivity = 0;
    h264enc_params->rateControlParams.skipDistributionWindowLength = 5;
    h264enc_params->rateControlParams.numSkipInDistributionWindow =1;
    h264enc_params->rateControlParams.enableHRDComplianceMode = 1;
    h264enc_params->rateControlParams.frameSkipThMulQ5 = 0;
    h264enc_params->rateControlParams.vbvUseLevelThQ5 = 0;
    h264enc_params->rateControlParams.reservedRC[0] = 0;
    h264enc_params->rateControlParams.reservedRC[1] = 0;
    h264enc_params->rateControlParams.reservedRC[2] = 0;

    //intercoding coding params
    h264enc_params->interCodingParams.interCodingPreset = IH264_INTERCODING_USERDEFINED;
    h264enc_params->interCodingParams.searchRangeHorP = 144;
    h264enc_params->interCodingParams.searchRangeVerP = 32;
    h264enc_params->interCodingParams.searchRangeHorB = 144;
    h264enc_params->interCodingParams.searchRangeVerB = 16;
    h264enc_params->interCodingParams.interCodingBias = IH264_BIASFACTOR_DEFAULT;
    h264enc_params->interCodingParams.skipMVCodingBias = IH264_BIASFACTOR_MILD;
    h264enc_params->interCodingParams.minBlockSizeP = IH264_BLOCKSIZE_8x8;
    h264enc_params->interCodingParams.minBlockSizeB = IH264_BLOCKSIZE_8x8;
    h264enc_params->interCodingParams.meAlgoMode = IH264ENC_MOTIONESTMODE_DEFAULT;

    //intra coding params.
    h264enc_params->intraCodingParams.intraCodingPreset = IH264_INTRACODING_DEFAULT;
    h264enc_params->intraCodingParams.lumaIntra4x4Enable = 0;
    h264enc_params->intraCodingParams.lumaIntra8x8Enable = 0x0FF;
    h264enc_params->intraCodingParams.lumaIntra16x16Enable = 0;  // BASE PROFILE
    h264enc_params->intraCodingParams.chromaIntra8x8Enable = 0;  // BASE PROFILE
    h264enc_params->intraCodingParams.chromaComponentEnable = IH264_CHROMA_COMPONENT_CB_CR_BOTH;  // BASE PROFILE
    h264enc_params->intraCodingParams.intraRefreshMethod = IH264_INTRAREFRESH_DEFAULT;
    h264enc_params->intraCodingParams.intraRefreshRate = 0;
    h264enc_params->intraCodingParams.gdrOverlapRowsBtwFrames = 0;
    h264enc_params->intraCodingParams.constrainedIntraPredEnable = 0;
    h264enc_params->intraCodingParams.intraCodingBias = IH264ENC_INTRACODINGBIAS_DEFAULT;

    //NALU Control Params.
    h264enc_params->nalUnitControlParams.naluControlPreset = IH264_NALU_CONTROL_USERDEFINED;
    h264enc_params->nalUnitControlParams.naluPresentMaskStartOfSequence = 0x01A0; // 416
    h264enc_params->nalUnitControlParams.naluPresentMaskIDRPicture = 0x0020; //32
    h264enc_params->nalUnitControlParams.naluPresentMaskIntraPicture = 2;
    h264enc_params->nalUnitControlParams.naluPresentMaskNonIntraPicture = 2;
    h264enc_params->nalUnitControlParams.naluPresentMaskEndOfSequence = 0x0C00; // 3072

    //Slice coding params
    h264enc_params->sliceCodingParams.sliceCodingPreset = IH264_SLICECODING_DEFAULT;
    h264enc_params->sliceCodingParams.sliceMode = IH264_SLICEMODE_DEFAULT;
    h264enc_params->sliceCodingParams.sliceUnitSize = 0;
    h264enc_params->sliceCodingParams.sliceStartOffset[0] = 0;
    h264enc_params->sliceCodingParams.sliceStartOffset[1] = 0;
    h264enc_params->sliceCodingParams.sliceStartOffset[2] = 0;
    h264enc_params->sliceCodingParams.streamFormat = IH264_STREAM_FORMAT_DEFAULT;

    //Loop Filter Params
    h264enc_params->loopFilterParams.loopfilterPreset = IH264_LOOPFILTER_DEFAULT;
    h264enc_params->loopFilterParams.loopfilterDisableIDC = IH264_DISABLE_FILTER_DEFAULT;
    h264enc_params->loopFilterParams.filterOffsetA = 0;
    h264enc_params->loopFilterParams.filterOffsetB = 0;

    //fmo coding params
    h264enc_params->fmoCodingParams.fmoCodingPreset = IH264_FMOCODING_DEFAULT;
    h264enc_params->fmoCodingParams.numSliceGroups = 1;
    h264enc_params->fmoCodingParams.sliceGroupMapType = IH264_SLICE_GRP_MAP_DEFAULT; // 4
    h264enc_params->fmoCodingParams.sliceGroupChangeDirectionFlag = IH264ENC_SLICEGROUP_CHANGE_DIRECTION_DEFAULT;
    h264enc_params->fmoCodingParams.sliceGroupChangeRate = 0;
    h264enc_params->fmoCodingParams.sliceGroupChangeCycle = 0;
    h264enc_params->fmoCodingParams.sliceGroupParams[0] = 0;
    h264enc_params->fmoCodingParams.sliceGroupParams[1] = 0;

    //VUI Control Params
    h264enc_params->vuiCodingParams.vuiCodingPreset = IH264_VUICODING_DEFAULT;
    h264enc_params->vuiCodingParams.aspectRatioInfoPresentFlag = 0;
    h264enc_params->vuiCodingParams.aspectRatioIdc = 0;
    h264enc_params->vuiCodingParams.videoSignalTypePresentFlag = 0;
    h264enc_params->vuiCodingParams.videoFormat = IH264ENC_VIDEOFORMAT_NTSC;
    h264enc_params->vuiCodingParams.videoFullRangeFlag = 0;
    h264enc_params->vuiCodingParams.timingInfoPresentFlag = 0;
    h264enc_params->vuiCodingParams.hrdParamsPresentFlag = 0;
    h264enc_params->vuiCodingParams.numUnitsInTicks= 1000;

    //Stereo Info Control Params
    h264enc_params->stereoInfoParams.stereoInfoPreset = IH264_STEREOINFO_DISABLE;
    h264enc_params->stereoInfoParams.topFieldIsLeftViewFlag = 1;
    h264enc_params->stereoInfoParams.viewSelfContainedFlag = 0;

    //Frame Packing SEI Params
    h264enc_params->framePackingSEIParams.framePackingPreset = IH264_FRAMEPACK_SEI_DISABLE;
    h264enc_params->framePackingSEIParams.framePackingType = IH264_FRAMEPACK_TYPE_DEFAULT;
    h264enc_params->framePackingSEIParams.frame0PositionX = 0;
    h264enc_params->framePackingSEIParams.frame0PositionY = 0;
    h264enc_params->framePackingSEIParams.frame1PositionX = 0;
    h264enc_params->framePackingSEIParams.frame1PositionY = 0;
    h264enc_params->framePackingSEIParams.reservedByte = 0;

    //SVC coding params
    h264enc_params->svcCodingParams.svcExtensionFlag = IH264_SVC_EXTENSION_FLAG_DISABLE;
    h264enc_params->svcCodingParams.dependencyID = 0;
    h264enc_params->svcCodingParams.qualityID = 0;
    h264enc_params->svcCodingParams.enhancementProfileID = 0;
    h264enc_params->svcCodingParams.layerIndex = 0;
    h264enc_params->svcCodingParams.refLayerDQId = 0;

    MSG("dce_alloc VIDENC2_Params successful h264enc_params=%p", h264enc_params);
    enc->codec = VIDENC2_create(enc->engine, (char*) "ivahd_h264enc", (VIDENC2_Params *)h264enc_params);
	if(!enc->codec){
		ERROR("Codec could not be created %p\n", enc->codec);
		goto bail;
	}
	return 0;
bail:
	encoder_deinit(enc);
	return -1;
}

static int init_mpeg4_static_params(encoder *enc)
{
	IMPEG4ENC_Params          *mpeg4enc_params    = NULL;
    enc->inArgs = dce_alloc(sizeof(IMPEG4ENC_InArgs));
	if(!enc->inArgs) goto bail;
    enc->inArgs->size = sizeof(IMPEG4ENC_InArgs);

    enc->outArgs = dce_alloc(sizeof(IMPEG4ENC_OutArgs));
	if(!enc->outArgs) goto bail;
    enc->outArgs->size = sizeof (IMPEG4ENC_OutArgs);
    enc->mpeg4enc_outArgs = (IMPEG4ENC_OutArgs *) enc->outArgs;

    enc->params = dce_alloc(sizeof(IMPEG4ENC_Params));
	if(!enc->params) goto bail;
    enc->params->size = sizeof(IMPEG4ENC_Params);

	init_common_static_params(enc);

	enc->params->maxInterFrameInterval = 0;
    mpeg4enc_params = enc->mpeg4enc_params = (IMPEG4ENC_Params *) enc->params;

    mpeg4enc_params->useDataPartitioning = 0;
    mpeg4enc_params->useRvlc = 0;
    if( enc->codectype == DCE_ENC_TEST_H263 ) {
        mpeg4enc_params->useShortVideoHeader = 1;
    } else {
        mpeg4enc_params->useShortVideoHeader = 0;
    }
    mpeg4enc_params->vopTimeIncrementResolution = 30;
    mpeg4enc_params->nonMultiple16RefPadMethod = IMPEG4_PAD_METHOD_MPEG4;
    mpeg4enc_params->pixelRange = IMPEG4ENC_PR_0_255;
    mpeg4enc_params->enableSceneChangeAlgo = IMPEG4ENC_SCDA_DISABLE;
    mpeg4enc_params->useVOS = 0;
    mpeg4enc_params->enableMONA = 0;
    mpeg4enc_params->enableAnalyticinfo = -1;
    mpeg4enc_params->debugTraceLevel = 0;
    mpeg4enc_params->lastNFramesToLog = 0;

    // IMPEG4ENC_RateControlParams
    mpeg4enc_params->rateControlParams.rateControlParamsPreset = IMPEG4_RATECONTROLPARAMS_DEFAULT;
    mpeg4enc_params->rateControlParams.rcAlgo = IMPEG4_RATECONTROLALGO_VBR;
    mpeg4enc_params->rateControlParams.qpI = 5;
    mpeg4enc_params->rateControlParams.qpP = 5;
    mpeg4enc_params->rateControlParams.seIntialQP = 5;
    mpeg4enc_params->rateControlParams.qpMax = 31;
    mpeg4enc_params->rateControlParams.qpMin = 1;
    mpeg4enc_params->rateControlParams.enablePerceptualQuantMode = 0;
    mpeg4enc_params->rateControlParams.allowFrameSkip = 0;
    mpeg4enc_params->rateControlParams.initialBufferLevel = 0;
    mpeg4enc_params->rateControlParams.vbvBufferSize = 0;
    mpeg4enc_params->rateControlParams.qpMinIntra = 0;

    // IMPEG4ENC_InterCodingParams
    mpeg4enc_params->interCodingParams.interCodingPreset = IMPEG4_INTERCODING_DEFAULT;
    mpeg4enc_params->interCodingParams.searchRangeHorP = 144;
    mpeg4enc_params->interCodingParams.searchRangeVerP = 32;
    mpeg4enc_params->interCodingParams.globalOffsetME = 1;
    mpeg4enc_params->interCodingParams.earlySkipThreshold = 200;
    mpeg4enc_params->interCodingParams.enableThresholdingMethod = 1;
    mpeg4enc_params->interCodingParams.minBlockSizeP = IMPEG4_BLOCKSIZE_8x8;
    mpeg4enc_params->interCodingParams.enableRoundingControl = 1;

    // IMPEG4ENC_IntraCodingParams
    mpeg4enc_params->intraCodingParams.intraCodingPreset = IMPEG4_INTRACODING_DEFAULT;
    mpeg4enc_params->intraCodingParams.intraRefreshMethod = 0;
    mpeg4enc_params->intraCodingParams.intraRefreshRate = 0;
    mpeg4enc_params->intraCodingParams.acpredEnable = 1;
    mpeg4enc_params->intraCodingParams.insertGOVHdrBeforeIframe = 0;
    mpeg4enc_params->intraCodingParams.enableDriftControl = 1;

    // IMPEG4ENC_sliceCodingParams
    mpeg4enc_params->sliceCodingParams.sliceCodingPreset = IMPEG4_SLICECODING_DEFAULT;
    mpeg4enc_params->sliceCodingParams.sliceMode = IMPEG4_SLICEMODE_NONE;
    mpeg4enc_params->sliceCodingParams.sliceUnitSize = 0;
    mpeg4enc_params->sliceCodingParams.gobInterval = 0;
    mpeg4enc_params->sliceCodingParams.useHec = 0;

    MSG("dce_alloc VIDENC2_Params successful mpeg4enc_params=%p", mpeg4enc_params);
	enc->codec = VIDENC2_create(enc->engine, (String)"ivahd_mpeg4enc", (VIDENC2_Params *)mpeg4enc_params);
	if(!enc->codec){
		ERROR("Codec could not be created %p\n", enc->codec);
		goto bail;
	}
	return 0;
bail:
	encoder_deinit(enc);
	return -1;
}

static int init_jpeg_static_params(encoder *enc)
{
    IJPEGVENC_Params          *jpegenc_params    = NULL;
    VIDENC2_Params            *params;
    enc->inArgs = dce_alloc(sizeof(IJPEGVENC_InArgs));
        if(!enc->inArgs) goto bail;
    enc->inArgs->size = sizeof(IJPEGVENC_InArgs);

    enc->outArgs = dce_alloc(sizeof(IJPEGVENC_OutArgs));
        if(!enc->outArgs) goto bail;
    enc->outArgs->size = sizeof (IJPEGVENC_OutArgs);
    enc->jpegenc_outArgs = (IJPEGVENC_OutArgs *) enc->outArgs;

    enc->params = dce_alloc(sizeof(IJPEGVENC_Params));
        if(!enc->params) goto bail;
    params = enc->params;

    init_common_static_params(enc);

    params->size = sizeof(IJPEGVENC_Params);

    jpegenc_params = enc->jpegenc_params = (IJPEGVENC_Params *) enc->params;


    /*--------------------------------------------------------------------------*/
    /*  Set Extended Parameters in IJpegVENC parameters                         */
    /*--------------------------------------------------------------------------*/
    jpegenc_params->maxThumbnailHSizeApp0 = 4096;
    jpegenc_params->maxThumbnailHSizeApp1 = 4096;
    jpegenc_params->maxThumbnailVSizeApp0 = 4096;
    jpegenc_params->maxThumbnailVSizeApp1 = 4096;
    jpegenc_params->debugTraceLevel = 0;

    jpegenc_params->lastNFramesToLog = 0;
    jpegenc_params->Markerposition   = 0;

    jpegenc_params->rateControlParams.VBRDuration     = 8;
    jpegenc_params->rateControlParams.VBRsensitivity  = 0;
    jpegenc_params->rateControlParams.vbvUseLevelThQ5 = 0;

    params->rateControlPreset = 4;
    params->maxBitRate = 7500000;
    params->minBitRate = 4500000;

    jpegenc_params->rateControlParams.rateControlParamsPreset = 1;
    jpegenc_params->rateControlParams.rcAlgo = 0;
    jpegenc_params->rateControlParams.qpMaxI = 51;
    jpegenc_params->rateControlParams.qpMinI = 1;
    jpegenc_params->rateControlParams.qpI = -1;
    jpegenc_params->rateControlParams.initialBufferLevel = 10000000;
    jpegenc_params->rateControlParams.HRDBufferSize = 10000000;
    jpegenc_params->rateControlParams.discardSavedBits = 0;

    MSG("dce_alloc VIDENC2_Params successful jpegenc_params=%p", jpegenc_params);
        enc->codec = VIDENC2_create(enc->engine, (String)"ivahd_jpegvenc", (VIDENC2_Params *)jpegenc_params);
        if(!enc->codec){
                ERROR("Codec could not be created %p\n", enc->codec);
                goto bail;
        }
        return 0;
bail:
        encoder_deinit(enc);
        return -1;
}

static void set_common_dyn_params(encoder *enc)
{
	VIDENC2_DynamicParams   *dynParams = enc->dynParams;
    dynParams->inputHeight  = enc->height;
    dynParams->inputWidth  = enc->width;
    dynParams->refFrameRate = enc->fps * 1000; // refFrameRate in fps * 1000
    dynParams->targetFrameRate= enc->fps * 1000; // Target frame rate in fps * 1000
    dynParams->targetBitRate = enc->bps;
	MSG("targetFramerate = %d, targetbitrate = %d\n", dynParams->targetFrameRate, dynParams->targetBitRate);
    dynParams->intraFrameInterval = 30; //Only 1st frame to be intra frame (I-frame)
    dynParams->generateHeader = XDM_ENCODE_AU;
    dynParams->captureWidth = enc->width;
    dynParams->forceFrame = IVIDEO_NA_FRAME;
    dynParams->sampleAspectRatioHeight = 1;
    dynParams->sampleAspectRatioWidth = 1;
    dynParams->ignoreOutbufSizeFlag = XDAS_FALSE;  // If this is XDAS_TRUE then getBufferFxn and getBufferHandle needs to be set.
    dynParams->putDataFxn = NULL;
    dynParams->putDataHandle = NULL;
    dynParams->getDataFxn = NULL;
    dynParams->getDataHandle = NULL;
    dynParams->getBufferFxn = NULL;
    dynParams->getBufferHandle = NULL;
    dynParams->lateAcquireArg = -1;
	return;
}

static inline int init_mpeg4_dyn_params(encoder *enc)
{
	VIDENC2_DynamicParams   *dynParams = NULL;
    XDAS_Int32      err;
	IMPEG4ENC_DynamicParams   *mpeg4enc_dynParams;

	dynParams = enc->dynParams = dce_alloc(sizeof(IMPEG4ENC_DynamicParams));
	if(!enc->dynParams) goto bail;
	enc->dynParams->size = sizeof(IMPEG4ENC_DynamicParams);
	MSG("dce_alloc dynParams successful dynParams=%p size=%d", enc->dynParams, enc->dynParams->size);
	set_common_dyn_params(enc);
    dynParams->interFrameInterval = 0;
    dynParams->mvAccuracy = IVIDENC2_MOTIONVECTOR_HALFPEL; //IVIDENC2_MotionVectorAccuracy

    MSG("dce_alloc IMPEG4ENC_DynamicParams successful size %d dynParams=%p", dynParams->size, dynParams);
    mpeg4enc_dynParams = (IMPEG4ENC_DynamicParams *) dynParams;

    mpeg4enc_dynParams->aspectRatioIdc = IMPEG4ENC_ASPECTRATIO_SQUARE;

    // IMPEG4ENC_RateControlParams
    memcpy(&mpeg4enc_dynParams->rateControlParams, &(enc->mpeg4enc_params->rateControlParams), sizeof(IMPEG4ENC_RateControlParams));
    // IMPEG4ENC_InterCodingParams
    memcpy(&mpeg4enc_dynParams->interCodingParams, &(enc->mpeg4enc_params->interCodingParams), sizeof(IMPEG4ENC_InterCodingParams));
    // IMPEG4ENC_sliceCodingParams
    memcpy(&mpeg4enc_dynParams->sliceCodingParams, &(enc->mpeg4enc_params->sliceCodingParams), sizeof(IMPEG4ENC_sliceCodingParams));

    enc->mpeg4enc_status = dce_alloc(sizeof(IMPEG4ENC_Status));
	if(!enc->mpeg4enc_status) goto bail;
    ((VIDENC2_Status *)(enc->mpeg4enc_status))->size = sizeof(IMPEG4ENC_Status);
    MSG("dce_alloc IMPEG4ENC_Status successful status=%p", enc->mpeg4enc_status);

    err = VIDENC2_control(enc->codec, XDM_SETPARAMS, (VIDENC2_DynamicParams *) mpeg4enc_dynParams, (VIDENC2_Status *) (enc->mpeg4enc_status));
	if(err){
		ERROR("Codec_control returned err=%d, extendedError=%08x", err, enc->mpeg4enc_status->videnc2Status.extendedError);
		goto bail;
	}
	return 0;
bail:
	encoder_deinit(enc);
	return -1;
}

static inline int init_h264_dyn_params(encoder *enc)
{
	VIDENC2_DynamicParams   *dynParams = NULL;
	IH264ENC_DynamicParams   *h264enc_dynParams;
    XDAS_Int32      err;

	dynParams = enc->dynParams = dce_alloc(sizeof(IH264ENC_DynamicParams));
	if(!enc->dynParams) goto bail;
	enc->dynParams->size = sizeof(IH264ENC_DynamicParams);
	MSG("dce_alloc dynParams successful dynParams=%p size=%d", enc->dynParams, enc->dynParams->size);
	set_common_dyn_params(enc);
    dynParams->interFrameInterval = 1; // 2 B frames
    dynParams->mvAccuracy = IVIDENC2_MOTIONVECTOR_QUARTERPEL; //IVIDENC2_MotionVectorAccuracy

    MSG("dce_alloc IH264ENC_DynamicParams successful size %d dynParams=%p", dynParams->size, dynParams);
    h264enc_dynParams = (IH264ENC_DynamicParams *) dynParams;

    h264enc_dynParams->sliceGroupChangeCycle = 0;
    h264enc_dynParams->searchCenter.x = 0x7FFF; // or 32767
    h264enc_dynParams->searchCenter.y = 0x7FFF; // or 32767
    h264enc_dynParams->enableStaticMBCount = 0;
    h264enc_dynParams->enableROI = 0;
    h264enc_dynParams->reservedDynParams[0] = 0;
    h264enc_dynParams->reservedDynParams[1] = 0;
    h264enc_dynParams->reservedDynParams[2] = 0;

    //Rate Control Params
    h264enc_dynParams->rateControlParams.rateControlParamsPreset = IH264_RATECONTROLPARAMS_EXISTING;
    h264enc_dynParams->rateControlParams.scalingMatrixPreset = IH264_SCALINGMATRIX_NONE;
    h264enc_dynParams->rateControlParams.rcAlgo = IH264_RATECONTROL_DEFAULT;
    h264enc_dynParams->rateControlParams.qpI = 28;
    h264enc_dynParams->rateControlParams.qpMaxI = 36;
    h264enc_dynParams->rateControlParams.qpMinI = 10;
    h264enc_dynParams->rateControlParams.qpP = 28;
    h264enc_dynParams->rateControlParams.qpMaxP = 40;
    h264enc_dynParams->rateControlParams.qpMinP = 10;
    h264enc_dynParams->rateControlParams.qpOffsetB = 4;
    h264enc_dynParams->rateControlParams.qpMaxB = 44;
    h264enc_dynParams->rateControlParams.qpMinB = 10;
    h264enc_dynParams->rateControlParams.allowFrameSkip = 0;
    h264enc_dynParams->rateControlParams.removeExpensiveCoeff = 0;
    h264enc_dynParams->rateControlParams.IPQualityFactor = IH264_QUALITY_FACTOR_DEFAULT;
    h264enc_dynParams->rateControlParams.chromaQPIndexOffset = 0;
    h264enc_dynParams->rateControlParams.initialBufferLevel = 64000;
    h264enc_dynParams->rateControlParams.HRDBufferSize = 64000;
    h264enc_dynParams->rateControlParams.enablePartialFrameSkip = 0;
    h264enc_dynParams->rateControlParams.minPicSizeRatioI = 0;
    h264enc_dynParams->rateControlParams.maxPicSizeRatioI = 20;
    h264enc_dynParams->rateControlParams.minPicSizeRatioP = 0;
    h264enc_dynParams->rateControlParams.maxPicSizeRatioP = 0;
    h264enc_dynParams->rateControlParams.minPicSizeRatioB = 0;
    h264enc_dynParams->rateControlParams.maxPicSizeRatioB = 0;
    h264enc_dynParams->rateControlParams.enablePRC = 1;
    h264enc_dynParams->rateControlParams.enableHRDComplianceMode = 0;
    h264enc_dynParams->rateControlParams.reserved = 0;
    h264enc_dynParams->rateControlParams.VBRDuration = 8;
    h264enc_dynParams->rateControlParams.VBRsensitivity = 0;
    h264enc_dynParams->rateControlParams.skipDistributionWindowLength = 5;
    h264enc_dynParams->rateControlParams.numSkipInDistributionWindow = 1;
    h264enc_dynParams->rateControlParams.enableHRDComplianceMode = 1;
    h264enc_dynParams->rateControlParams.frameSkipThMulQ5 = 0;
    h264enc_dynParams->rateControlParams.vbvUseLevelThQ5 = 0;
    h264enc_dynParams->rateControlParams.reservedRC[0] = 0;
    h264enc_dynParams->rateControlParams.reservedRC[1] = 0;
    h264enc_dynParams->rateControlParams.reservedRC[2] = 0;

    //Inter Coding Params
    h264enc_dynParams->interCodingParams.interCodingPreset = IH264_INTERCODING_EXISTING;
    h264enc_dynParams->interCodingParams.searchRangeHorP = 144;
    h264enc_dynParams->interCodingParams.searchRangeVerP = 32;
    h264enc_dynParams->interCodingParams.searchRangeHorB = 144;
    h264enc_dynParams->interCodingParams.searchRangeVerB = 16;
    h264enc_dynParams->interCodingParams.interCodingBias= IH264_BIASFACTOR_DEFAULT;
    h264enc_dynParams->interCodingParams.skipMVCodingBias = IH264_BIASFACTOR_MILD;
    h264enc_dynParams->interCodingParams.minBlockSizeP = IH264_BLOCKSIZE_8x8;
    h264enc_dynParams->interCodingParams.minBlockSizeB = IH264_BLOCKSIZE_8x8;
    h264enc_dynParams->interCodingParams.meAlgoMode = IH264ENC_MOTIONESTMODE_DEFAULT;

    //Intra Coding Params
    h264enc_dynParams->intraCodingParams.intraCodingPreset = IH264_INTRACODING_EXISTING;
    h264enc_dynParams->intraCodingParams.lumaIntra4x4Enable = 0xFF; // or 255 BASE PROFILE
    h264enc_dynParams->intraCodingParams.lumaIntra8x8Enable = 0; // BASE PROFILE
    h264enc_dynParams->intraCodingParams.lumaIntra16x16Enable = 0;
    h264enc_dynParams->intraCodingParams.chromaIntra8x8Enable = 0;
    h264enc_dynParams->intraCodingParams.chromaComponentEnable = IH264_CHROMA_COMPONENT_CB_CR_BOTH;
    h264enc_dynParams->intraCodingParams.intraRefreshMethod = IH264_INTRAREFRESH_DEFAULT;
    h264enc_dynParams->intraCodingParams.intraRefreshRate = 0;
    h264enc_dynParams->intraCodingParams.gdrOverlapRowsBtwFrames = 0;
    h264enc_dynParams->intraCodingParams.constrainedIntraPredEnable = 0;
    h264enc_dynParams->intraCodingParams.intraCodingBias = IH264ENC_INTRACODINGBIAS_DEFAULT;

    //Slice Coding Params
    h264enc_dynParams->sliceCodingParams.sliceCodingPreset = IH264_SLICECODING_EXISTING;
    h264enc_dynParams->sliceCodingParams.sliceMode = IH264_SLICEMODE_DEFAULT;
    h264enc_dynParams->sliceCodingParams.sliceUnitSize = 0;
    h264enc_dynParams->sliceCodingParams.sliceStartOffset[0] = 0;
    h264enc_dynParams->sliceCodingParams.sliceStartOffset[1] = 0;
    h264enc_dynParams->sliceCodingParams.sliceStartOffset[2] = 0;
    h264enc_dynParams->sliceCodingParams.streamFormat = IH264_STREAM_FORMAT_DEFAULT;

    enc->h264enc_status = dce_alloc(sizeof(IH264ENC_Status));
	if(!enc->h264enc_status) goto bail;
    ((VIDENC2_Status*)(enc->h264enc_status))->size = sizeof(IH264ENC_Status);
    MSG("dce_alloc IH264ENC_Status successful status=%p", enc->h264enc_status);

    err = VIDENC2_control(enc->codec, XDM_SETPARAMS, (VIDENC2_DynamicParams *) h264enc_dynParams, (VIDENC2_Status *) (enc->h264enc_status));
    if( err ) {
		ERROR("Codec_control returned err=%d, extendedError=%08x", err, enc->h264enc_status->videnc2Status.extendedError);
        goto bail;
    }
    MSG("dce_alloc IH264ENC_Status successful h264enc_status=%p", enc->h264enc_status);


	return 0;
bail:
	encoder_deinit(enc);
	return -1;
}

static inline int init_jpeg_dyn_params(encoder *enc)
{
    VIDENC2_DynamicParams   *dynParams = NULL;
    XDAS_Int32      err;
    IJPEGVENC_DynamicParams   *jpegenc_dynParams;

    dynParams = enc->dynParams = dce_alloc(sizeof(IJPEGVENC_DynamicParams));
    if(!enc->dynParams) goto bail;
    enc->dynParams->size = sizeof(IJPEGVENC_DynamicParams);
    MSG("dce_alloc dynParams successful dynParams=%p size=%d", enc->dynParams, enc->dynParams->size);
    set_common_dyn_params(enc);

    MSG("dce_alloc IJPEGVENC_DynamicParams successful size %d dynParams=%p", dynParams->size, dynParams);
    jpegenc_dynParams = (IJPEGVENC_DynamicParams *) dynParams;

    jpegenc_dynParams->restartInterval = 0;
    jpegenc_dynParams->qualityFactor = 50;
    jpegenc_dynParams->quantTable = NULL;

    jpegenc_dynParams->enablePrivacyMasking = 0;

    enc->jpegenc_status = dce_alloc(sizeof(IJPEGVENC_Status));
    if(!enc->jpegenc_status) goto bail;
    ((VIDENC2_Status *)(enc->jpegenc_status))->size = sizeof(IJPEGVENC_Status);
    MSG("dce_alloc IJPEGVENC_Status successful status=%p", enc->jpegenc_status);

    err = VIDENC2_control(enc->codec, XDM_SETPARAMS, (VIDENC2_DynamicParams *) jpegenc_dynParams, (VIDENC2_Status *) (enc->jpegenc_status));
    if(err){
                ERROR("Codec_control returned err=%d, extendedError=%08x", err, enc->jpegenc_status->videnc2Status.extendedError);
                goto bail;
    }
        return 0;
bail:
        encoder_deinit(enc);
        return -1;
}

static int encoder_init(encoder *enc)
{
    Engine_Error    ec;
    XDAS_Int32      err;
    int  output_size = 0;
    int  mvbufinfo_size = 0;


	/*Initialze and Open DRM device*/
	enc->drmfd = drmOpen("omapdrm", "platform:omapdrm:00");
	if(!enc->drmfd)
	{
		ERROR("Unable to open drm device");
		return -1;
	}
	dce_set_fd(enc->drmfd);
	enc->dev = dce_init();

	enc->engine = Engine_open((String)"ivahd_vidsvr", NULL, &ec);
    if( !enc->engine ) {
        ERROR("Engine open failed");
        goto bail;
    }
    MSG("Engine_open successful engine=%p", enc->engine);
    /* input buffer parameters in terms of MBs, Needs alignment to multiple of 16 */
    enc->width  = ALIGN2(enc->width, 4);         /* round up to MB */
    enc->height = ALIGN2(enc->height, 1);        /* round up to MB */

    switch( enc->codectype ) {
        case DCE_ENC_TEST_H264 :
        case DCE_ENC_TEST_MPEG4 :
        case DCE_ENC_TEST_H263 :
        case DCE_ENC_TEST_JPEG :
            enc->num_buffers = 1;
            break;
        default :
            ERROR("Unrecognized codec to encode");
    }
	/*Allocate the input buffers */
	enc->buf.y_bo = omap_bo_new(enc->dev, enc->width * enc->height, OMAP_BO_WC);
	if(!enc->buf.y_bo) goto bail;
	enc->buf.ybuff = omap_bo_map(enc->buf.y_bo);
	enc->buf.fdy = omap_bo_dmabuf(enc->buf.y_bo);
	dce_buf_lock(1, (size_t*) &enc->buf.fdy);
	enc->buf.sizey = enc->width * enc->height;
	enc->buf.uv_bo = omap_bo_new(enc->dev,
								(enc->width * enc->height) / 2, OMAP_BO_WC);
	if(!enc->buf.uv_bo) goto bail;
	enc->buf.uvbuff = omap_bo_map(enc->buf.uv_bo);
	enc->buf.fduv = omap_bo_dmabuf(enc->buf.uv_bo);
	dce_buf_lock(1, (size_t*) &enc->buf.fduv);
	enc->buf.sizeuv = (enc->width * enc->height) / 2;

	/*Initialize the static ivariant input buffer parameters*/
    MSG("input buffer configuration width %d height %d", enc->width, enc->height);
    enc->inBufs = dce_alloc(sizeof(IVIDEO2_BufDesc));
	if(!enc->inBufs) goto bail;
    enc->inBufs->numPlanes = 2;
    enc->inBufs->imageRegion.topLeft.x = 0;
    enc->inBufs->imageRegion.topLeft.y = 0;
    enc->inBufs->imageRegion.bottomRight.x = enc->width;

    enc->inBufs->topFieldFirstFlag = 0; //Only valid for interlace content.
    enc->inBufs->contentType = IVIDEO_PROGRESSIVE;

    enc->inBufs->activeFrameRegion.topLeft.x = 0;
    enc->inBufs->activeFrameRegion.topLeft.y = 0;
    enc->inBufs->activeFrameRegion.bottomRight.x = enc->width;
    enc->inBufs->activeFrameRegion.bottomRight.y = enc->height;

    enc->inBufs->imageRegion.bottomRight.y = enc->height;
    enc->inBufs->chromaFormat = XDM_YUV_420SP;

    enc->inBufs->secondFieldOffsetWidth[0] = 0;
    enc->inBufs->secondFieldOffsetHeight[0] = 0;


    MSG("Allocating input buffers from omapdrm");

    enc->inBufs->imagePitch[0] = enc->width;
    enc->inBufs->planeDesc[0].memType = XDM_MEMTYPE_RAW;
    enc->inBufs->planeDesc[0].bufSize.bytes = enc->width * enc->height;
    enc->inBufs->secondFieldOffsetWidth[1] = 1;
    enc->inBufs->secondFieldOffsetHeight[1] = 0;

    enc->inBufs->imagePitch[1] = enc->width;
    enc->inBufs->planeDesc[1].memType = XDM_MEMTYPE_RAW;
    enc->inBufs->planeDesc[1].bufSize.bytes = enc->width * enc->height / 2;



	/*Initiaze static parameters of the codec*/
	switch(enc->codectype){
	case DCE_ENC_TEST_H264:
		if(init_h264_static_params(enc)){
			ERROR("H264 encoder static parameter error");
			goto bail;
		}

		if(init_h264_dyn_params(enc)){
			ERROR("H264 encoder static parameter error");
			goto bail;
		}
		enc->status = (VIDENC2_Status*) (enc->h264enc_status);
		break;
	case DCE_ENC_TEST_MPEG4:
	case DCE_ENC_TEST_H263:
		if(init_mpeg4_static_params(enc)){
			ERROR("MPEG4 encoder static parameter error");
			goto bail;
		}
		if(init_mpeg4_dyn_params(enc)){
			ERROR("H264 encoder static parameter error");
			goto bail;
		}
		enc->status = (VIDENC2_Status*) (enc->mpeg4enc_status);
		break;
        case DCE_ENC_TEST_JPEG:
                if(init_jpeg_static_params(enc)){
                        ERROR("JPEG encoder static parameter error");
                        goto bail;
                }

                if(init_jpeg_dyn_params(enc)){
                        ERROR("JPEG encoder static parameter error");
                        goto bail;
                }
                enc->status = (VIDENC2_Status*) (enc->jpegenc_status);
                break;
	default:
		ERROR("Unknown codec type");
		goto bail;
	}
    // XDM_GETBUFINFO
    // Send Control cmd XDM_GETBUFINFO to get min output and output size
    err = VIDENC2_control(enc->codec, XDM_GETBUFINFO, enc->dynParams, (VIDENC2_Status*) enc->status);
    MSG("VIDENC2_control - XDM_GETBUFINFO err %d status numOutBuf %d OutBufSize %d MVBufInfo %d",
			err, ((VIDENC2_Status *)(enc->status))->bufInfo.minNumOutBufs,
			 ((VIDENC2_Status *)(enc->status))->bufInfo.minOutBufSize[0].bytes, ((VIDENC2_Status *)(enc->status))->bufInfo.minOutBufSize[1].bytes);
/*
 * outBufs handling
 */
    enc->outBufs = dce_alloc(sizeof(XDM2_BufDesc));
	if(!enc->outBufs) goto bail;
    output_size = ((VIDENC2_Status *)(enc->status))->bufInfo.minOutBufSize[0].bytes;
    mvbufinfo_size = ((VIDENC2_Status *)(enc->status))->bufInfo.minOutBufSize[1].bytes;

	enc->outBufs->numBufs = (enc->codectype == DCE_ENC_TEST_H264) ? ((VIDENC2_Status *)(enc->h264enc_status))->bufInfo.minNumOutBufs : 1;

	/*allocate the output buffer*/
	enc->output_bo = omap_bo_new(enc->dev, output_size, OMAP_BO_WC);
	enc->cdata = omap_bo_map(enc->output_bo);
	enc->outBufs->descs[0].buf = (void *)omap_bo_dmabuf(enc->output_bo);
	dce_buf_lock(1, (size_t*) &(enc->outBufs->descs[0].buf));
    enc->outBufs->descs[0].memType = XDM_MEMTYPE_RAW;
    enc->outBufs->descs[0].bufSize.bytes = output_size;
    MSG("buf %p  fd %p ", enc->output_bo, enc->outBufs->descs[0].buf);

    if( mvbufinfo_size > 0 ) {
		/*Allocate the output mv buffer*/
		enc->mv_bo = omap_bo_new(enc->dev, mvbufinfo_size, OMAP_BO_WC);
		enc->outBufs->descs[1].buf = (void *)omap_bo_dmabuf(enc->mv_bo);
		dce_buf_lock(1, (size_t*) &(enc->outBufs->descs[1].buf));
		enc->outBufs->descs[1].memType = XDM_MEMTYPE_RAW;
		enc->outBufs->descs[1].bufSize.bytes = mvbufinfo_size;
		MSG("mv buf %p  fd %p ", enc->mv_bo, enc->outBufs->descs[1].buf);
	}

	return 0;
bail:
	err = encoder_deinit(enc);
	return -1;
}

static int encoder_deinit(encoder *enc)
{

	if(enc->buf.y_bo) {
		dce_buf_unlock(1, (size_t*) &enc->buf.fdy);
		close(enc->buf.fdy);
		omap_bo_del(enc->buf.y_bo);
	}
	if(enc->buf.uv_bo) {
		dce_buf_unlock(1, (size_t*) &enc->buf.fduv);
		close(enc->buf.fduv);
		omap_bo_del(enc->buf.uv_bo);
	}

	if(enc->codec) {
		MSG("\nDeleting encoder codec...\n");
	    VIDENC2_delete(enc->codec);
	}

    if( enc->output_bo ) {
	MSG("\nFreeing output %p \n", enc->output_bo);
		dce_buf_unlock(1, (size_t*) &(enc->outBufs->descs[0].buf));
		close((int)(enc->outBufs->descs[0].buf));
		omap_bo_del(enc->output_bo);
    }
    if( enc->mv_bo ){
	MSG("\nFreeing output_mvbuf %p...\n", enc->mv_bo);
		dce_buf_unlock(1, (size_t*) &(enc->outBufs->descs[1].buf));
		close((int)(enc->outBufs->descs[1].buf));
		omap_bo_del(enc->mv_bo);
    }

    if( enc->params ) {
        dce_free(enc->params);
    }
    if( enc->dynParams ) {
        dce_free(enc->dynParams);
    }
    if( enc->h264enc_status ) {
        dce_free(enc->h264enc_status);
    }
    if( enc->mpeg4enc_status ) {
        dce_free(enc->mpeg4enc_status);
    }
    if( enc->inBufs ) {
        dce_free(enc->inBufs);
    }
    if( enc->outBufs ) {
        dce_free(enc->outBufs);
    }
    if( enc->inArgs ) {
        dce_free(enc->inArgs);
    }
    if( enc->outArgs ) {
        dce_free(enc->outArgs);
    }
    if( enc->engine ) {
        Engine_close(enc->engine);
    }

	if(enc->fin) fclose(enc->fin);
	if(enc->fout) fclose(enc->fout);
	if(enc->dev) dce_deinit(enc->dev);
	if(enc->drmfd) drmClose(enc->drmfd);
	memset(enc, 0, sizeof(encoder));
	return 0;
}

encoder encObj;

static void sig_handler(int signo, siginfo_t *siginfo, void *context)
 {
   if (signo == SIGINT) {
       encoder_deinit(&encObj);
       sleep(1);
       exit(0);
   }
 }

/* encoder body */
int main(int argc, char * *argv)
{
	struct sigaction sa;
	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO;
	if (sigaction(SIGINT, &sa, NULL) == -1) {
	    ERROR ("\nDid not catch  SIGINT\n");
	}

	XDAS_Int32      err;
	IH264ENC_InArgs *h264enc_inArgs;
	IMPEG4ENC_InArgs *mpeg4enc_inArgs;
        IJPEGVENC_InArgs *jpegenc_inArgs;
	IH264ENC_OutArgs *h264enc_outArgs;
	IMPEG4ENC_OutArgs *mpeg4enc_outArgs;
        IJPEGVENC_OutArgs *jpegenc_outArgs;

    int             in_cnt = 0, out_cnt = 0, iters = 0;
    int             eof = 0;
    int             bytesGenerated = 0;


	memset(&encObj, 0, sizeof(encoder));

	if(parse_command(argc, argv, &encObj)){
		goto shutdown;
	}


	if(encoder_init(&encObj))
	{
		MSG("Error during encoder initialization");
		goto shutdown;
	}

/*
 * codec process
 */
    while( encObj.inBufs->numPlanes && encObj.outBufs->numBufs ) {
        int    n;
        MSG("Looping on reading input inBufs->numPlanes %d outBufs->numBufs %d",
					 encObj.inBufs->numPlanes, encObj.outBufs->numBufs);

        //Read the NV12 frame to input buffer to be encoded.
        n = read_NV12frame(&encObj);

        if( n > 0) {
            eof = 0;
			/*Pass the FDs for subplanes*/
            encObj.inBufs->planeDesc[0].buf = (XDAS_Int8 *)(encObj.buf.fdy);
            encObj.inBufs->planeDesc[1].buf = (XDAS_Int8 *)(encObj.buf.fduv);
            MSG("inBufs->planeDesc[0].buf %p inBufs->planeDesc[1].buf %p",
					 encObj.inBufs->planeDesc[0].buf, encObj.inBufs->planeDesc[1].buf);
            MSG("push: %d (plane[0]= %d + plane[1]= %d = %d bytes) (%p)",
					 in_cnt, encObj.inBufs->planeDesc[0].bufSize.bytes, encObj.inBufs->planeDesc[1].bufSize.bytes, n, &encObj.buf);
            in_cnt++;

            encObj.inArgs->inputID = in_cnt; // Send frame count as the input ID
            /*
             * Input buffer has data to be encoded.
             */
        } else if( n == -1 ) {

            // Set EOF as 1 to ensure flush completes
            eof = 1;
            in_cnt++;

            MSG("n == -1 - go to shutdown");
			printf("Encoding completed successfully\n");

            goto shutdown;
        } else {
            /* end of input..  (n == 0) */
            encObj.inBufs->numPlanes = 0;
            eof = 1;
            MSG("n == 0 - go to shutdown");
			printf("Encoding completed successfully\n");

            goto shutdown;

        }


        do {

            if( encObj.codectype == DCE_ENC_TEST_H264 ) {
                h264enc_inArgs = (IH264ENC_InArgs *) encObj.inArgs;
				h264enc_outArgs = (IH264ENC_OutArgs *) encObj.outArgs;
                MSG("TEST inArgs->inputID %d h264enc_inArgs->videnc2InArgs.inputID %d",
						 encObj.inArgs->inputID, h264enc_inArgs->videnc2InArgs.inputID);
                err = VIDENC2_process(encObj.codec, encObj.inBufs, encObj.outBufs, (VIDENC2_InArgs *) h264enc_inArgs, (VIDENC2_OutArgs *) h264enc_outArgs);
                MSG("[DCE_ENC_TEST] VIDENC2_process - err %d", err);

                if( err < 0 ) {
                    int    i = 0;

                    for( i=0; i < IH264ENC_EXTERROR_NUM_MAXWORDS; i++ ) {
                        MSG("DETAIL EXTENDED ERROR h264enc_outArgs->extErrorCode[%d]=%08x", i, (uint)h264enc_outArgs->extErrorCode[i]);
                    }

                    err = VIDENC2_control(encObj.codec, XDM_GETSTATUS, (VIDENC2_DynamicParams *) encObj.dynParams, (VIDENC2_Status *) encObj.h264enc_status);
                    MSG("[DCE_ENC_TEST] VIDENC2_control - XDM_GETSTATUS err %d", err);

                    for( i=0; i < IH264ENC_EXTERROR_NUM_MAXWORDS; i++ ) {
                        MSG("DETAIL EXTENDED ERROR h264enc_status->extErrorCode[%d]=%08x", i, (uint)encObj.h264enc_status->extErrorCode[i]);
                    }

                    if( XDM_ISFATALERROR(h264enc_outArgs->videnc2OutArgs.extendedError) ) {
                        ERROR("process returned error: %d\n", err);
                        ERROR("extendedError: %08x", h264enc_outArgs->videnc2OutArgs.extendedError);
						printf("Encoding Error\n");
                        goto shutdown;
                    } else if( eof ) {
                        ERROR("Codec_process returned err=%d, extendedError=%08x", err, h264enc_outArgs->videnc2OutArgs.extendedError);
                        err = XDM_EFAIL;

                        if( err == XDM_EFAIL ) {
                            MSG("-------------------- Flush completed------------------------");
                        }
                    } else {
                        ERROR("Non-fatal err=%d, h264enc_outArgs->videnc2OutArgs.extendedError=%08x ", err, h264enc_outArgs->videnc2OutArgs.extendedError);
                        err = XDM_EOK;
                    }
                }

                MSG("bytesGenerated %d", h264enc_outArgs->videnc2OutArgs.bytesGenerated);
                bytesGenerated = h264enc_outArgs->videnc2OutArgs.bytesGenerated;
            } else if( encObj.codectype == DCE_ENC_TEST_MPEG4 || encObj.codectype == DCE_ENC_TEST_H263 ) {
                mpeg4enc_inArgs = (IMPEG4ENC_InArgs *) encObj.inArgs;
				mpeg4enc_outArgs = (IMPEG4ENC_OutArgs *) encObj.outArgs;
                MSG("TEST inArgs->inputID %d mpeg4enc_inArgs->videnc2InArgs.inputID %d", encObj.inArgs->inputID, mpeg4enc_inArgs->videnc2InArgs.inputID);
                MSG("[DCE_ENC_TEST] codec %p inBufs %p outBufs %p mpeg4enc_inArgs %p mpeg4enc_outArgs %p", encObj.codec, encObj.inBufs, encObj.outBufs, mpeg4enc_inArgs, mpeg4enc_outArgs);
                err = VIDENC2_process(encObj.codec, encObj.inBufs, encObj.outBufs, (VIDENC2_InArgs *) mpeg4enc_inArgs, (VIDENC2_OutArgs *) mpeg4enc_outArgs);
                MSG("[DCE_ENC_TEST] VIDENC2_process - err %d", err);
                if( err < 0 ) {
                    //TODO error handling on MPEG4/H.263
                    ERROR("Codec_process returned err=%d, extendedError=%08x", err, mpeg4enc_outArgs->videnc2OutArgs.extendedError);
					printf("Encoding Error\n");
                    goto shutdown;
                }
                MSG("\n bytesGenerated %d", mpeg4enc_outArgs->videnc2OutArgs.bytesGenerated);
                bytesGenerated = mpeg4enc_outArgs->videnc2OutArgs.bytesGenerated;
            } else if (encObj.codectype == DCE_ENC_TEST_JPEG) {
                jpegenc_inArgs = (IJPEGVENC_InArgs *) encObj.inArgs;
                jpegenc_outArgs = (IJPEGVENC_OutArgs *) encObj.outArgs;
                MSG("TEST inArgs->inputID %d jpegenc_inArgs->videnc2InArgs.inputID %d", encObj.inArgs->inputID, jpegenc_inArgs->videnc2InArgs.inputID);
                MSG("[DCE_ENC_TEST] codec %p inBufs %p outBufs %p jpegenc_inArgs %p jpegenc_outArgs %p", encObj.codec, encObj.inBufs, encObj.outBufs, jpegenc_inArgs, jpegenc_outArgs);
                err = VIDENC2_process(encObj.codec, encObj.inBufs, encObj.outBufs, (VIDENC2_InArgs *) jpegenc_inArgs, (VIDENC2_OutArgs *) jpegenc_outArgs);
                MSG("[DCE_ENC_TEST] VIDENC2_process - err %d", err);
                if( err < 0 ) {
                    //TODO error handling on JPEG
                    ERROR("Codec_process returned err=%d, extendedError=%08x", err, jpegenc_outArgs->videnc2OutArgs.extendedError);
                                        printf("Encoding Error\n");
                    goto shutdown;
                }
                MSG("\n bytesGenerated %d", jpegenc_outArgs->videnc2OutArgs.bytesGenerated);
                bytesGenerated = jpegenc_outArgs->videnc2OutArgs.bytesGenerated;
            }


            /*
             * Handling of output data from codec
             */

            /* get the output buffer and write it to file */
            if( bytesGenerated ) {
                // write the frames to output file based on the value of frames_to_write on how many frames to write.
                if( out_cnt > encObj.nframes ){
					printf("Encoding completed successfully\n");
					goto shutdown;
				}
                INFO("Dumping frame %d", out_cnt);
				write_output(&encObj, bytesGenerated);
            }
            out_cnt++;
            ++iters; // Guard for infinite VIDENC2_PROCESS loop when codec never return XDM_EFAIL
        } while( eof && (err != XDM_EFAIL) && (iters < 1000));  // Multiple VIDENC2_process when eof until err == XDM_EFAIL
    }
shutdown:
	encoder_deinit(&encObj);
    return 0;
}
