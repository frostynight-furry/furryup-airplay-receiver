/*
 * H.264 Video Decoder using FFmpeg (libavcodec)
 *
 * Decodes H.264 Annex-B NAL units into NV12 frames
 * that can be directly fed to OBS.
 */

#include "video-decoder.h"

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#define MAX_VIDEO_DIMENSION 8192

struct video_decoder {
	const AVCodec *codec;
	AVCodecContext *ctx;
	AVFrame *frame;
	AVFrame *latest_frame;
	AVPacket *pkt;
	struct SwsContext *sws;

	/* NV12 output buffer */
	uint8_t *nv12_data[4];
	int nv12_linesize[4];
	int nv12_width;
	int nv12_height;
	int sws_src_format;
	int sws_full_range;
	int sws_colorspace;
	int color_error_count;

	bool initialized;
};

static bool map_color_space(struct video_decoder *dec, const AVFrame *frame,
			    int *sws_colorspace, bool *bt601)
{
	switch (frame->colorspace) {
	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_BT470BG:
		*sws_colorspace = SWS_CS_ITU601;
		*bt601 = true;
		return true;
	case AVCOL_SPC_BT709:
		*sws_colorspace = SWS_CS_ITU709;
		*bt601 = false;
		return true;
	case AVCOL_SPC_UNSPECIFIED:
		/* H.264 streams sometimes omit VUI matrix coefficients. Apply the
		 * conventional SD/HD default explicitly instead of silently treating
		 * every unknown matrix as BT.709. */
		*bt601 = frame->height <= 576;
		*sws_colorspace = *bt601 ? SWS_CS_ITU601 : SWS_CS_ITU709;
		return true;
	default:
		/* OBS's 8-bit NV12 source metadata cannot faithfully describe every
		 * FFmpeg matrix (notably BT.2020). Reject instead of mislabelling it. */
		if (dec->color_error_count < 5 ||
		    dec->color_error_count % 500 == 0) {
			fprintf(stderr,
				"[AirPlay] unsupported video color matrix %d\n",
				frame->colorspace);
		}
		dec->color_error_count++;
		return false;
	}
}

struct video_decoder *video_decoder_create(void)
{
	struct video_decoder *dec = calloc(1, sizeof(struct video_decoder));
	if (!dec)
		return NULL;

	/* Use software H.264 decoder for maximum compatibility */
	dec->codec = avcodec_find_decoder(AV_CODEC_ID_H264);

	if (!dec->codec) {
		fprintf(stderr, "[AirPlay] No H.264 decoder found!\n");
		free(dec);
		return NULL;
	}

	fprintf(stderr, "[AirPlay] Using decoder: %s\n", dec->codec->name);

	dec->ctx = avcodec_alloc_context3(dec->codec);
	if (!dec->ctx) {
		free(dec);
		return NULL;
	}

	/* Low latency config */
	dec->ctx->thread_count = 1;
	dec->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	dec->ctx->flags2 |= AV_CODEC_FLAG2_FAST;
	dec->sws_src_format = AV_PIX_FMT_NONE;
	dec->sws_full_range = -1;
	dec->sws_colorspace = -1;

	if (avcodec_open2(dec->ctx, dec->codec, NULL) < 0) {
		fprintf(stderr, "[AirPlay] Failed to open H264 decoder\n");
		avcodec_free_context(&dec->ctx);
		free(dec);
		return NULL;
	}

	dec->frame = av_frame_alloc();
	dec->latest_frame = av_frame_alloc();
	dec->pkt = av_packet_alloc();

	if (!dec->frame || !dec->latest_frame || !dec->pkt) {
		if (dec->frame)
			av_frame_free(&dec->frame);
		if (dec->latest_frame)
			av_frame_free(&dec->latest_frame);
		if (dec->pkt)
			av_packet_free(&dec->pkt);
		avcodec_free_context(&dec->ctx);
		free(dec);
		return NULL;
	}

	dec->initialized = true;
	return dec;
}

void video_decoder_destroy(struct video_decoder *dec)
{
	if (!dec)
		return;

	if (dec->sws)
		sws_freeContext(dec->sws);

	if (dec->nv12_data[0])
		av_freep(&dec->nv12_data[0]);

	if (dec->frame)
		av_frame_free(&dec->frame);
	if (dec->latest_frame)
		av_frame_free(&dec->latest_frame);
	if (dec->pkt)
		av_packet_free(&dec->pkt);
	if (dec->ctx)
		avcodec_free_context(&dec->ctx);

	free(dec);
}

void video_decoder_reset(struct video_decoder *dec)
{
	if (!dec || !dec->initialized)
		return;
	avcodec_flush_buffers(dec->ctx);
	av_packet_unref(dec->pkt);
	av_frame_unref(dec->frame);
	av_frame_unref(dec->latest_frame);
	dec->color_error_count = 0;
}

static bool ensure_nv12_buffer(struct video_decoder *dec, int width, int height)
{
	if (dec->nv12_width == width && dec->nv12_height == height &&
	    dec->nv12_data[0])
		return true;

	/* Free old buffer */
	if (dec->nv12_data[0])
		av_freep(&dec->nv12_data[0]);

	/* Allocate NV12 buffer */
	int ret = av_image_alloc(dec->nv12_data, dec->nv12_linesize, width,
				 height, AV_PIX_FMT_NV12, 32);
	if (ret < 0)
		return false;

	dec->nv12_width = width;
	dec->nv12_height = height;

	/* Update sws context */
	if (dec->sws) {
		sws_freeContext(dec->sws);
		dec->sws = NULL;
		dec->sws_src_format = AV_PIX_FMT_NONE;
		dec->sws_full_range = -1;
		dec->sws_colorspace = -1;
	}

	return true;
}

static bool convert_to_nv12(struct video_decoder *dec, AVFrame *frame,
			    int colorspace)
{
	if (!ensure_nv12_buffer(dec, frame->width, frame->height))
		return false;

	/* Recreate the converter if the decoder changes pixel format while the
	 * resolution stays the same. This can happen after a reconnect. */
	if (dec->sws && dec->sws_src_format != frame->format) {
		sws_freeContext(dec->sws);
		dec->sws = NULL;
		dec->sws_src_format = AV_PIX_FMT_NONE;
		dec->sws_full_range = -1;
		dec->sws_colorspace = -1;
	}

	/* Create/update sws context if needed */
	if (!dec->sws) {
		dec->sws = sws_getContext(
			frame->width, frame->height,
			(enum AVPixelFormat)frame->format, frame->width,
			frame->height, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR,
			NULL, NULL, NULL);

		if (!dec->sws)
			return false;
		dec->sws_src_format = frame->format;
	}

	/* Preserve the decoded signal range. Without this, swscale converts a
	 * full-range YUVJ frame to limited NV12 while OBS is told it is full range. */
	int full_range = frame->color_range == AVCOL_RANGE_JPEG;
	if (dec->sws_full_range != full_range ||
	    dec->sws_colorspace != colorspace) {
		const int *coefficients = sws_getCoefficients(colorspace);
		if (!coefficients ||
		    sws_setColorspaceDetails(dec->sws, coefficients, full_range,
					 coefficients, full_range, 0, 1 << 16,
					 1 << 16) < 0)
			return false;
		dec->sws_full_range = full_range;
		dec->sws_colorspace = colorspace;
	}

	/* Convert. Never submit an old buffer if conversion failed. */
	int scaled = sws_scale(dec->sws,
			       (const uint8_t *const *)frame->data,
			       frame->linesize, 0, frame->height,
			       dec->nv12_data, dec->nv12_linesize);

	return scaled == frame->height;
}

/* Drain all frames currently exposed by libavcodec. Mirroring should produce
 * one picture per access unit, but keeping the newest frame is a safe
 * low-latency fallback if a decoder ever queues more than one. */
static int receive_latest_video_frame(struct video_decoder *dec,
				      bool *got_picture)
{
	for (;;) {
		int ret = avcodec_receive_frame(dec->ctx, dec->frame);
		if (ret < 0)
			return ret;

		av_frame_unref(dec->latest_frame);
		av_frame_move_ref(dec->latest_frame, dec->frame);
		*got_picture = true;
	}
}

bool video_decoder_decode(struct video_decoder *dec, const uint8_t *h264_data,
			  size_t h264_size, uint64_t pts,
			  struct decoded_frame *out_frame)
{
	if (!dec || !dec->initialized || !h264_data || !h264_size ||
	    h264_size > INT_MAX || !out_frame)
		return false;

	memset(out_frame, 0, sizeof(*out_frame));
	av_frame_unref(dec->latest_frame);

	/* FFmpeg bitstream readers require AV_INPUT_BUFFER_PADDING_SIZE bytes
	 * beyond the payload. UxPlay does not promise that padding, so copy into
	 * an AVPacket-owned buffer. */
	av_packet_unref(dec->pkt);
	if (av_new_packet(dec->pkt, (int)h264_size) < 0)
		return false;
	memcpy(dec->pkt->data, h264_data, h264_size);
	dec->pkt->pts = (int64_t)pts;
	dec->pkt->dts = (int64_t)pts;

	/* Drain to EAGAIN before retrying a packet rejected with EAGAIN. */
	bool got_picture = false;
	int ret = avcodec_send_packet(dec->ctx, dec->pkt);
	if (ret == AVERROR(EAGAIN)) {
		ret = receive_latest_video_frame(dec, &got_picture);
		if (ret != AVERROR(EAGAIN))
			goto fail;
		ret = avcodec_send_packet(dec->ctx, dec->pkt);
		if (ret == AVERROR(EAGAIN)) {
			fprintf(stderr,
				"[AirPlay] H264 decoder remained full after drain\n");
			avcodec_flush_buffers(dec->ctx);
			goto fail;
		}
	}
	if (ret < 0)
		goto fail;

	ret = receive_latest_video_frame(dec, &got_picture);
	if (ret != AVERROR(EAGAIN))
		goto fail;
	av_packet_unref(dec->pkt);
	if (!got_picture)
		return false;

	if (dec->latest_frame->width <= 0 || dec->latest_frame->height <= 0 ||
	    dec->latest_frame->width > MAX_VIDEO_DIMENSION ||
	    dec->latest_frame->height > MAX_VIDEO_DIMENSION ||
	    dec->latest_frame->format < 0)
		goto fail;

	int colorspace = 0;
	bool bt601 = false;
	if (!map_color_space(dec, dec->latest_frame, &colorspace, &bt601))
		goto fail;

	/* OBS accepts NV12 directly. Keeping the frame in 4:2:0 avoids the much
	 * larger RGBA copy on every frame. */
	if (!convert_to_nv12(dec, dec->latest_frame, colorspace))
		goto fail;

	out_frame->data[0] = dec->nv12_data[0];
	out_frame->data[1] = dec->nv12_data[1];
	out_frame->linesize[0] = dec->nv12_linesize[0];
	out_frame->linesize[1] = dec->nv12_linesize[1];
	out_frame->width = dec->latest_frame->width;
	out_frame->height = dec->latest_frame->height;
	out_frame->pts = dec->latest_frame->pts;
	out_frame->full_range =
		dec->latest_frame->color_range == AVCOL_RANGE_JPEG;
	out_frame->bt601 = bt601;
	av_frame_unref(dec->latest_frame);

	return true;

fail:
	av_packet_unref(dec->pkt);
	av_frame_unref(dec->latest_frame);
	return false;
}
