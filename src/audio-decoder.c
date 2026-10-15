/*
 * AirPlay audio decoder using FFmpeg (libavcodec)
 *
 * Decodes AAC-ELD / AAC-LC / ALAC from AirPlay into float32 PCM
 * that can be fed to OBS via obs_source_output_audio().
 */

#include "audio-decoder.h"

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <util/base.h>

#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define AIRPLAY_OUTPUT_CHANNELS 2
#define AIRPLAY_MIN_SAMPLE_RATE 8000
#define AIRPLAY_MAX_SAMPLE_RATE 192000
#define AIRPLAY_MAX_OUTPUT_SAMPLES 8192

/* AudioSpecificConfig blobs (MPEG-4 ISO 14496-3 1.6.2.1), matching
 * UxPlay's canonical caps for the AirPlay audio streams. */
static const uint8_t ASC_AAC_ELD_44100_2[] = {0xF8, 0xE8, 0x50, 0x00}; /* spf 480 */
static const uint8_t ASC_AAC_LC_44100_2[] = {0x12, 0x10};              /* spf 1024 */

/* ALAC magic cookie: 44100/16/2, spf 352 */
static const uint8_t ALAC_COOKIE_44100_16_2[] = {
	0x00, 0x00, 0x00, 0x24, 'a',  'l',  'a',  'c',
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x60,
	0x00, 0x10, 0x28, 0x0a, 0x0e, 0x02, 0x00, 0xff,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xac, 0x44,
};

struct audio_decoder {
	AVCodecContext *ctx;
	AVFrame *frame;
	AVPacket *pkt;
	struct SwrContext *swr;

	/* swr input config cache - rebuilt when the decoded frame changes */
	int swr_in_rate;
	int swr_in_channels;
	int swr_in_fmt;

	/* Output buffer */
	float *pcm_buf;
	size_t pcm_buf_size;

	unsigned char ct; /* current AirPlay compression type, 0 = not set */
	int err_count;    /* decode errors logged so far (rate limit) */
};

static void log_av_error(const char *what, int err)
{
	char buf[64] = {0};
	av_strerror(err, buf, sizeof(buf));
	blog(LOG_ERROR, "[AirPlay] %s: %s (%d)", what, buf, err);
}

struct audio_decoder *audio_decoder_create(void)
{
	struct audio_decoder *dec = calloc(1, sizeof(struct audio_decoder));
	if (!dec)
		return NULL;

	dec->frame = av_frame_alloc();
	dec->pkt = av_packet_alloc();
	if (!dec->frame || !dec->pkt) {
		audio_decoder_destroy(dec);
		return NULL;
	}

	/* Initial PCM buffer (1024 samples * 2 ch * 4 bytes) */
	dec->pcm_buf_size = 1024 * 2 * sizeof(float);
	dec->pcm_buf = malloc(dec->pcm_buf_size);
	if (!dec->pcm_buf) {
		audio_decoder_destroy(dec);
		return NULL;
	}

	return dec;
}

void audio_decoder_destroy(struct audio_decoder *dec)
{
	if (!dec)
		return;

	if (dec->swr)
		swr_free(&dec->swr);

	free(dec->pcm_buf);

	if (dec->frame)
		av_frame_free(&dec->frame);
	if (dec->pkt)
		av_packet_free(&dec->pkt);
	if (dec->ctx)
		avcodec_free_context(&dec->ctx);

	free(dec);
}

void audio_decoder_reset(struct audio_decoder *dec)
{
	if (!dec)
		return;
	if (dec->ctx)
		avcodec_flush_buffers(dec->ctx);
	if (dec->pkt)
		av_packet_unref(dec->pkt);
	if (dec->frame)
		av_frame_unref(dec->frame);
	/* Discard delay/configuration from the previous AirPlay session. */
	if (dec->swr)
		swr_free(&dec->swr);
	dec->swr_in_rate = 0;
	dec->swr_in_channels = 0;
	dec->swr_in_fmt = 0;
	dec->err_count = 0;
}

static bool open_codec(struct audio_decoder *dec, unsigned char ct)
{
	const AVCodec *codec = NULL;
	const uint8_t *extradata = NULL;
	size_t extradata_size = 0;

	switch (ct) {
	case 2: /* ALAC */
		codec = avcodec_find_decoder(AV_CODEC_ID_ALAC);
		extradata = ALAC_COOKIE_44100_16_2;
		extradata_size = sizeof(ALAC_COOKIE_44100_16_2);
		break;
	case 4: /* AAC-LC */
	case 8: /* AAC-ELD */
		/* libfdk_aac if present (best ELD support), else native */
		codec = avcodec_find_decoder_by_name("libfdk_aac");
		if (!codec)
			codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
		if (ct == 8) {
			extradata = ASC_AAC_ELD_44100_2;
			extradata_size = sizeof(ASC_AAC_ELD_44100_2);
		} else {
			extradata = ASC_AAC_LC_44100_2;
			extradata_size = sizeof(ASC_AAC_LC_44100_2);
		}
		break;
	default:
		blog(LOG_ERROR, "[AirPlay] unsupported audio ct=%u", ct);
		return false;
	}

	if (!codec) {
		blog(LOG_ERROR, "[AirPlay] no decoder found for audio ct=%u", ct);
		return false;
	}

	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return false;

	ctx->extradata = av_mallocz(extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
	if (!ctx->extradata) {
		avcodec_free_context(&ctx);
		return false;
	}
	memcpy(ctx->extradata, extradata, extradata_size);
	ctx->extradata_size = (int)extradata_size;

	ctx->sample_rate = 44100;
	av_channel_layout_default(&ctx->ch_layout, 2);

	int ret = avcodec_open2(ctx, codec, NULL);
	if (ret < 0) {
		log_av_error("avcodec_open2 (audio)", ret);
		avcodec_free_context(&ctx);
		return false;
	}

	dec->ctx = ctx;
	blog(LOG_INFO, "[AirPlay] audio decoder opened: %s (ct=%u)",
	     codec->name, ct);
	return true;
}

bool audio_decoder_set_format(struct audio_decoder *dec, unsigned char ct)
{
	if (!dec)
		return false;
	if (dec->ct == ct && dec->ctx)
		return true;

	if (dec->ctx)
		avcodec_free_context(&dec->ctx);
	if (dec->swr) {
		swr_free(&dec->swr);
		dec->swr_in_rate = 0;
		dec->swr_in_channels = 0;
		dec->swr_in_fmt = 0;
	}

	if (!open_codec(dec, ct)) {
		dec->ct = 0;
		return false;
	}

	dec->ct = ct;
	dec->err_count = 0;
	return true;
}

static bool ensure_swr(struct audio_decoder *dec, const AVFrame *frame)
{
	int channels = frame->ch_layout.nb_channels;
	if (channels <= 0 || channels > 8 ||
	    frame->sample_rate < AIRPLAY_MIN_SAMPLE_RATE ||
	    frame->sample_rate > AIRPLAY_MAX_SAMPLE_RATE || frame->format < 0)
		return false;

	if (dec->swr && dec->swr_in_rate == frame->sample_rate &&
	    dec->swr_in_channels == channels &&
	    dec->swr_in_fmt == frame->format)
		return true;

	if (dec->swr)
		swr_free(&dec->swr);

	AVChannelLayout out_layout;
	av_channel_layout_default(&out_layout, AIRPLAY_OUTPUT_CHANNELS);

	int ret = swr_alloc_set_opts2(&dec->swr,
				      &out_layout,
				      AV_SAMPLE_FMT_FLT,
				      frame->sample_rate,
				      &frame->ch_layout,
				      (enum AVSampleFormat)frame->format,
				      frame->sample_rate,
				      0, NULL);
	av_channel_layout_uninit(&out_layout);
	if (ret < 0 || !dec->swr) {
		log_av_error("swr_alloc_set_opts2", ret);
		return false;
	}

	ret = swr_init(dec->swr);
	if (ret < 0) {
		log_av_error("swr_init", ret);
		swr_free(&dec->swr);
		return false;
	}

	dec->swr_in_rate = frame->sample_rate;
	dec->swr_in_channels = channels;
	dec->swr_in_fmt = frame->format;
	return true;
}

struct audio_accumulator {
	int samples;
	int sample_rate;
	int input_channels;
	int input_format;
	int64_t pts;
	bool failed;
};

static bool append_audio_frame(struct audio_decoder *dec,
			       struct audio_accumulator *acc,
			       const AVFrame *frame)
{
	int channels = frame->ch_layout.nb_channels;
	int samples = frame->nb_samples;
	int sample_rate = frame->sample_rate;
	if (channels <= 0 || channels > 8 || samples <= 0 ||
	    sample_rate < AIRPLAY_MIN_SAMPLE_RATE ||
	    sample_rate > AIRPLAY_MAX_SAMPLE_RATE || frame->format < 0)
		return false;

	/* One OBS audio block cannot change format halfway through. AirPlay is
	 * fixed-rate stereo; treat a mid-block change as a broken stream. */
	if (acc->sample_rate &&
	    (acc->sample_rate != sample_rate ||
	     acc->input_channels != channels ||
	     acc->input_format != frame->format))
		return false;

	if (!ensure_swr(dec, frame))
		return false;

	int max_out = swr_get_out_samples(dec->swr, samples);
	if (max_out < 0 || max_out > AIRPLAY_MAX_OUTPUT_SAMPLES - acc->samples)
		return false;

	size_t total_capacity = (size_t)(acc->samples + max_out);
	if (total_capacity >
	    SIZE_MAX / (AIRPLAY_OUTPUT_CHANNELS * sizeof(float)))
		return false;
	size_t needed = total_capacity * AIRPLAY_OUTPUT_CHANNELS *
			sizeof(float);
	if (needed > dec->pcm_buf_size) {
		float *tmp = realloc(dec->pcm_buf, needed);
		if (!tmp)
			return false;
		dec->pcm_buf = tmp;
		dec->pcm_buf_size = needed;
	}

	uint8_t *out_planes[1] = {
		(uint8_t *)(dec->pcm_buf +
			    (size_t)acc->samples * AIRPLAY_OUTPUT_CHANNELS),
	};
	int converted = swr_convert(dec->swr, out_planes, max_out,
				    (const uint8_t **)frame->extended_data,
				    samples);
	if (converted < 0) {
		if (dec->err_count < 10) {
			dec->err_count++;
			log_av_error("swr_convert (audio)", converted);
		}
		return false;
	}

	if (!acc->sample_rate) {
		acc->sample_rate = sample_rate;
		acc->input_channels = channels;
		acc->input_format = frame->format;
	}
	if (converted > 0 && acc->samples == 0)
		acc->pts = frame->pts == AV_NOPTS_VALUE ? 0 : frame->pts;
	acc->samples += converted;
	return true;
}

/* Receive every currently available frame. FFmpeg requires callers to drain
 * to EAGAIN before retrying a packet that avcodec_send_packet rejected with
 * EAGAIN. Audio frames are concatenated into one OBS block. */
static int receive_audio_frames(struct audio_decoder *dec,
				struct audio_accumulator *acc)
{
	for (;;) {
		int ret = avcodec_receive_frame(dec->ctx, dec->frame);
		if (ret < 0)
			return ret;

		if (!acc->failed && !append_audio_frame(dec, acc, dec->frame))
			acc->failed = true;
		av_frame_unref(dec->frame);
	}
}

bool audio_decoder_decode(struct audio_decoder *dec, const uint8_t *aac_data,
			  size_t aac_size, uint64_t pts,
			  struct decoded_audio *out)
{
	if (!dec || !dec->ctx || !aac_data || !aac_size ||
	    aac_size > INT_MAX || !out)
		return false;

	memset(out, 0, sizeof(*out));

	av_packet_unref(dec->pkt);
	if (av_new_packet(dec->pkt, (int)aac_size) < 0)
		return false;
	memcpy(dec->pkt->data, aac_data, aac_size);
	dec->pkt->pts = (int64_t)pts;

	struct audio_accumulator acc = {0};
	int ret = avcodec_send_packet(dec->ctx, dec->pkt);
	if (ret == AVERROR(EAGAIN)) {
		ret = receive_audio_frames(dec, &acc);
		if (ret != AVERROR(EAGAIN)) {
			if (ret != AVERROR_EOF && dec->err_count < 10) {
				dec->err_count++;
				log_av_error("avcodec_receive_frame (audio)", ret);
			}
			acc.failed = true;
		}

		ret = avcodec_send_packet(dec->ctx, dec->pkt);
		if (ret == AVERROR(EAGAIN)) {
			/* This violates the send/receive contract after a full drain.
			 * Reset so later independent AirPlay audio packets can recover. */
			blog(LOG_ERROR,
			     "[AirPlay] audio decoder remained full after drain");
			avcodec_flush_buffers(dec->ctx);
			acc.failed = true;
		}
	}
	if (ret < 0) {
		if (dec->err_count < 10) {
			dec->err_count++;
			log_av_error("avcodec_send_packet (audio)", ret);
		}
		acc.failed = true;
	}

	if (ret >= 0) {
		ret = receive_audio_frames(dec, &acc);
		if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
			if (dec->err_count < 10) {
				dec->err_count++;
				log_av_error("avcodec_receive_frame (audio)", ret);
			}
			acc.failed = true;
		}
	}

	av_packet_unref(dec->pkt);
	if (acc.failed || acc.samples <= 0) {
		if (acc.failed && dec->swr) {
			swr_free(&dec->swr);
			dec->swr_in_rate = 0;
			dec->swr_in_channels = 0;
			dec->swr_in_fmt = 0;
		}
		return false;
	}

	out->data = (uint8_t *)dec->pcm_buf;
	out->data_size = (size_t)acc.samples * AIRPLAY_OUTPUT_CHANNELS *
			 sizeof(float);
	out->samples = acc.samples;
	out->channels = AIRPLAY_OUTPUT_CHANNELS;
	out->sample_rate = acc.sample_rate;
	out->pts = acc.pts;

	return true;
}
