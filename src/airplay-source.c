/*
 * OBS AirPlay Source - Uses UxPlay's raop library (v1.73.6) for the full
 * AirPlay 2 mirroring protocol (FairPlay, pairing, encryption)
 * and FFmpeg for video/audio decoding.
 */

#include "airplay-source.h"
#include "video-decoder.h"
#include "audio-decoder.h"
#include "status-frame.h"

#include <obs-module.h>
#include <util/platform.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/* UxPlay's raop API */
#include "raop.h"
#include "dnssd.h"
#include "stream.h"
#include "logger.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <process.h>
#define getpid _getpid
#endif

/* Fixed network ports so installers can open firewall rules for them.
 * TCP: mirror data, RTSP/http. UDP: timing, control, audio data. */
#define AIRPLAY_TCP_MIRROR 7100
#define AIRPLAY_TCP_RTSP 7000
#define AIRPLAY_UDP_TIMING 7011
#define AIRPLAY_UDP_CONTROL 6001
#define AIRPLAY_UDP_DATA 6000

#define MAX_VIDEO_PACKET_SIZE (32 * 1024 * 1024)
#define MAX_AUDIO_PACKET_SIZE (1024 * 1024)
#define MAX_AUDIO_FRAMES_PER_PACKET 8192
#define MAX_TIMESTAMP_DRIFT_NS 3000000000ULL
#define DEFAULT_SERVER_NAME "AirPlay Live"
#define STATUS_FRAME_WIDTH 1280
#define STATUS_FRAME_HEIGHT 720

enum receiver_state {
	RECEIVER_STATE_STOPPED = 0,
	RECEIVER_STATE_STARTING,
	RECEIVER_STATE_WAITING,
	RECEIVER_STATE_CONNECTING,
	RECEIVER_STATE_STREAMING,
	RECEIVER_STATE_ERROR,
};

enum receiver_start_error {
	RECEIVER_ERROR_NONE = 0,
	RECEIVER_ERROR_INITIALIZATION,
	RECEIVER_ERROR_PORT_IN_USE,
	RECEIVER_ERROR_BONJOUR,
};

/* UxPlay 1.73.6 and the fixed AirPlay ports support one receiver per OBS
 * process. OBS_SOURCE_DO_NOT_DUPLICATE does not prevent a user from adding a
 * second source manually, so enforce that invariant here. */
static volatile LONG g_instance_claimed = 0;

struct airplay_source {
	obs_source_t *source;

	/* UxPlay components */
	raop_t *raop;
	dnssd_t *dnssd;
	bool dnssd_raop_registered;
	bool dnssd_airplay_registered;

	/* Decoders */
	struct video_decoder *vdec;
	struct audio_decoder *adec;
	pthread_mutex_t video_mutex;
	pthread_mutex_t audio_mutex;
	pthread_mutex_t timestamp_mutex;
	pthread_mutex_t client_mutex;
	bool video_mutex_ready;
	bool audio_mutex_ready;
	bool timestamp_mutex_ready;
	bool client_mutex_ready;

	/* Settings */
	char server_name[256];
	bool use_random_mac;
	bool managed_by_fus;
	bool receiver_enabled;
	char reconnect_id[64];
	int cfg_width;
	int cfg_height;
	int cfg_fps;
	int cfg_max_fps;

	/* State */
	volatile LONG width;
	volatile LONG height;
	volatile LONG open_connections;
	volatile LONG stopping;
	volatile LONG server_running;
	volatile LONG receiver_state;
	volatile LONG start_error;
	volatile LONG has_video;
	volatile LONG status_epoch;
	int video_frame_count;
	int video_no_output;
	int invalid_video_pkts;

	/* Converts AirPlay wall-clock timestamps to OBS monotonic time. */
	bool timestamp_anchor_set;
	uint64_t remote_timestamp_anchor;
	uint64_t local_timestamp_anchor;

	/* Audio diagnostics */
	int audio_pkts;
	int audio_fails;
	int invalid_audio_pkts;
	bool audio_output_logged;
	bool warned_no_adec;
	bool warned_audio_ct;
	bool warned_h265;

	/* The UxPlay setup request exposes friendly client information. Keep the
	 * last admitted device so it can be shown in OBS without a second window. */
	char client_name[256];
	char client_model[128];
	char client_device_id[128];
};

static bool is_stopping(struct airplay_source *ctx)
{
	return !ctx || InterlockedCompareExchange(&ctx->stopping, 0, 0) != 0;
}

/* Manual OBS sources retain the historical create-and-run behavior.  The
 * receiver_enabled setting is authoritative only after FUS explicitly marks
 * a source as managed, so old scene collections and third-party callers do
 * not accidentally create a dormant receiver. */
static bool receiver_should_run(const struct airplay_source *ctx)
{
	return ctx && (!ctx->managed_by_fus || ctx->receiver_enabled);
}

/* The checked-in OBS import library is intentionally limited to the ABI used
 * by the original module and does not include this data mutator.  Resolve it
 * from the OBS DLL that already owns the source.  Failure is non-fatal for the
 * current session (ctx is still forced off), but is logged because the stale
 * persisted flag could not be replaced with the safe baseline. */
static bool persist_receiver_session_disabled(obs_data_t *settings)
{
	typedef void (*obs_data_set_bool_fn)(obs_data_t *, const char *, bool);
	HMODULE obs_module = GetModuleHandleW(L"obs.dll");
	obs_data_set_bool_fn set_bool = obs_module
		? (obs_data_set_bool_fn)GetProcAddress(obs_module,
						       "obs_data_set_bool")
		: NULL;
	if (!set_bool) {
		blog(LOG_WARNING,
		     "[AirPlay] Could not persist the session-disabled receiver state");
		return false;
	}

	set_bool(settings, "receiver_enabled", false);
	return true;
}

static uint64_t timestamp_diff(uint64_t a, uint64_t b)
{
	return a > b ? a - b : b - a;
}

static void reset_timestamp_mapper(struct airplay_source *ctx)
{
	if (!ctx || !ctx->timestamp_mutex_ready)
		return;

	pthread_mutex_lock(&ctx->timestamp_mutex);
	ctx->timestamp_anchor_set = false;
	ctx->remote_timestamp_anchor = 0;
	ctx->local_timestamp_anchor = 0;
	pthread_mutex_unlock(&ctx->timestamp_mutex);
}

static uint64_t map_timestamp(struct airplay_source *ctx, uint64_t remote_ts)
{
	uint64_t now = os_gettime_ns();
	if (!ctx || !ctx->timestamp_mutex_ready || !remote_ts)
		return now;

	pthread_mutex_lock(&ctx->timestamp_mutex);

	uint64_t mapped = now;
	if (!ctx->timestamp_anchor_set) {
		ctx->timestamp_anchor_set = true;
		ctx->remote_timestamp_anchor = remote_ts;
		ctx->local_timestamp_anchor = now;
	} else if (remote_ts >= ctx->remote_timestamp_anchor) {
		uint64_t delta = remote_ts - ctx->remote_timestamp_anchor;
		if (UINT64_MAX - ctx->local_timestamp_anchor >= delta)
			mapped = ctx->local_timestamp_anchor + delta;
	} else {
		uint64_t delta = ctx->remote_timestamp_anchor - remote_ts;
		if (ctx->local_timestamp_anchor >= delta)
			mapped = ctx->local_timestamp_anchor - delta;
	}

	/* Re-anchor after a clock jump or malformed timestamp. */
	if (timestamp_diff(mapped, now) > MAX_TIMESTAMP_DRIFT_NS) {
		ctx->remote_timestamp_anchor = remote_ts;
		ctx->local_timestamp_anchor = now;
		mapped = now;
	}

	pthread_mutex_unlock(&ctx->timestamp_mutex);
	return mapped;
}

/* ---------- MAC address helpers ---------- */

static void generate_random_mac(char *mac, size_t len)
{
	srand((unsigned)(time(NULL) * getpid()));
	int octet = (rand() % 64) << 2 | 0x02; /* locally administered */
	snprintf(mac, len, "%02x:%02x:%02x:%02x:%02x:%02x",
		 octet, rand() % 256, rand() % 256,
		 rand() % 256, rand() % 256, rand() % 256);
}

/* A stable, locally administered address keeps the receiver identity the same
 * across OBS restarts without exposing the physical network adapter address. */
static void generate_stable_mac(const char *server_name, char *mac, size_t len)
{
	char computer_name[MAX_COMPUTERNAME_LENGTH + 1] = {0};
	DWORD computer_name_len = MAX_COMPUTERNAME_LENGTH + 1;
	if (!GetComputerNameA(computer_name, &computer_name_len))
		strncpy(computer_name, "windows", sizeof(computer_name) - 1);

	uint64_t hash = 14695981039346656037ULL;
	const char *parts[] = {computer_name,
			       server_name ? server_name : DEFAULT_SERVER_NAME,
			       "airplay-live-source"};
	for (size_t p = 0; p < sizeof(parts) / sizeof(parts[0]); ++p) {
		const unsigned char *cursor = (const unsigned char *)parts[p];
		while (*cursor) {
			hash ^= *cursor++;
			hash *= 1099511628211ULL;
		}
	}

	unsigned char bytes[6];
	for (size_t i = 0; i < sizeof(bytes); ++i)
		bytes[i] = (unsigned char)(hash >> (i * 8));
	bytes[0] = (unsigned char)((bytes[0] & 0xfc) | 0x02);

	snprintf(mac, len, "%02x:%02x:%02x:%02x:%02x:%02x",
		 bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
}

static void copy_utf8_value(char *dst, size_t dst_size, const char *src,
			    const char *fallback)
{
	if (!dst || dst_size == 0)
		return;

	const unsigned char *input = (const unsigned char *)
		((src && *src) ? src : (fallback ? fallback : ""));
	size_t used = 0;
	while (*input && used + 1 < dst_size) {
		size_t sequence = 1;
		if ((*input & 0xe0) == 0xc0)
			sequence = 2;
		else if ((*input & 0xf0) == 0xe0)
			sequence = 3;
		else if ((*input & 0xf8) == 0xf0)
			sequence = 4;
		else if (*input >= 0x80)
			sequence = 0;

		bool valid = sequence > 0;
		for (size_t i = 1; valid && i < sequence; ++i)
			valid = input[i] && (input[i] & 0xc0) == 0x80;

		if (!valid) {
			dst[used++] = '?';
			++input;
			continue;
		}
		if (used + sequence >= dst_size)
			break;
		memcpy(dst + used, input, sequence);
		used += sequence;
		input += sequence;
	}
	dst[used] = '\0';
}

static void copy_server_name(char *dst, size_t dst_size, const char *src)
{
	copy_utf8_value(dst, dst_size, src, DEFAULT_SERVER_NAME);
}

static void parse_hw_addr(const char *str, char *hw, int *hw_len)
{
	*hw_len = 0;
	for (size_t i = 0; i < strlen(str) && *hw_len < 6; i += 3) {
		hw[(*hw_len)++] = (char)strtol(str + i, NULL, 16);
	}
}

static void clear_client_info(struct airplay_source *ctx)
{
	if (!ctx || !ctx->client_mutex_ready)
		return;
	pthread_mutex_lock(&ctx->client_mutex);
	ctx->client_name[0] = '\0';
	ctx->client_model[0] = '\0';
	ctx->client_device_id[0] = '\0';
	pthread_mutex_unlock(&ctx->client_mutex);
}

static void get_client_info(struct airplay_source *ctx, char *name,
			    size_t name_size, char *model, size_t model_size)
{
	if (name && name_size)
		name[0] = '\0';
	if (model && model_size)
		model[0] = '\0';
	if (!ctx || !ctx->client_mutex_ready)
		return;

	pthread_mutex_lock(&ctx->client_mutex);
	copy_utf8_value(name, name_size, ctx->client_name, "");
	copy_utf8_value(model, model_size, ctx->client_model, "");
	pthread_mutex_unlock(&ctx->client_mutex);
}

static const char *receiver_error_detail(struct airplay_source *ctx)
{
	LONG error = ctx ? InterlockedCompareExchange(&ctx->start_error, 0, 0)
			 : RECEIVER_ERROR_INITIALIZATION;
	switch (error) {
	case RECEIVER_ERROR_PORT_IN_USE:
		return obs_module_text("Status.Error.Port");
	case RECEIVER_ERROR_BONJOUR:
		return obs_module_text("Status.Error.Bonjour");
	case RECEIVER_ERROR_INITIALIZATION:
	default:
		return obs_module_text("Status.Error.Initialization");
	}
}

/* Called with video_mutex held. The epoch is captured at the event that
 * requested a slate; a real frame or newer state transition invalidates it. */
static LONG begin_receiver_status_locked(struct airplay_source *ctx,
					 enum receiver_state state)
{
	InterlockedExchange(&ctx->receiver_state, (LONG)state);
	if (state != RECEIVER_STATE_STREAMING)
		InterlockedExchange(&ctx->has_video, 0);
	return InterlockedIncrement(&ctx->status_epoch);
}

static LONG begin_receiver_status(struct airplay_source *ctx,
				  enum receiver_state state)
{
	if (!ctx || !ctx->video_mutex_ready)
		return 0;

	pthread_mutex_lock(&ctx->video_mutex);
	LONG epoch = is_stopping(ctx) ? 0 :
		begin_receiver_status_locked(ctx, state);
	pthread_mutex_unlock(&ctx->video_mutex);
	return epoch;
}

static void output_receiver_status(struct airplay_source *ctx,
				   enum receiver_state state,
				   LONG expected_epoch)
{
	if (!ctx || !ctx->source || !ctx->video_mutex_ready || !expected_epoch)
		return;

	/* Drawing is rare and serialized with decoded-frame submission. Recheck
	 * the event token and current connection state after taking the lock: an
	 * older callback may have been waiting behind the first real frame. */
	pthread_mutex_lock(&ctx->video_mutex);
	LONG current_epoch =
		InterlockedCompareExchange(&ctx->status_epoch, 0, 0);
	LONG current_state =
		InterlockedCompareExchange(&ctx->receiver_state, 0, 0);
	LONG open = InterlockedCompareExchange(&ctx->open_connections, 0, 0);
	LONG has_video = InterlockedCompareExchange(&ctx->has_video, 0, 0);
	bool valid = !is_stopping(ctx) && current_epoch == expected_epoch &&
		current_state == (LONG)state;
	if (state == RECEIVER_STATE_WAITING)
		valid = valid && open == 0 && !has_video;
	else if (state == RECEIVER_STATE_CONNECTING)
		valid = valid && open > 0 && !has_video;
	else if (state == RECEIVER_STATE_STARTING ||
		 state == RECEIVER_STATE_ERROR)
		valid = valid && !has_video;
	else if (state == RECEIVER_STATE_STOPPED)
		valid = valid && open == 0 && !has_video &&
			InterlockedCompareExchange(&ctx->server_running, 0, 0) == 0;
	if (!valid) {
		pthread_mutex_unlock(&ctx->video_mutex);
		return;
	}

	const char *heading = obs_module_text("Status.Waiting.Heading");
	const char *detail = obs_module_text("Status.Waiting.Detail");
	enum airplay_status_tone tone = AIRPLAY_STATUS_TONE_READY;
	char connected_detail[640] = {0};
	char client_name[256] = {0};
	char client_model[128] = {0};

	switch (state) {
	case RECEIVER_STATE_STARTING:
		heading = obs_module_text("Status.Starting.Heading");
		detail = obs_module_text("Status.Starting.Detail");
		tone = AIRPLAY_STATUS_TONE_STARTING;
		break;
	case RECEIVER_STATE_CONNECTING:
		heading = obs_module_text("Status.Connecting.Heading");
		get_client_info(ctx, client_name, sizeof(client_name),
				client_model, sizeof(client_model));
		if (client_name[0] || client_model[0]) {
			snprintf(connected_detail, sizeof(connected_detail),
				 "%s%s%s%s%s",
				 obs_module_text("Status.Connecting.DevicePrefix"),
				 client_name[0] ? client_name :
					obs_module_text("Status.UnknownDevice"),
				 client_model[0] ? " (" : "",
				 client_model[0] ? client_model : "",
				 client_model[0] ? ")" : "");
			detail = connected_detail;
		} else {
			detail = obs_module_text("Status.Connecting.Detail");
		}
		tone = AIRPLAY_STATUS_TONE_CONNECTED;
		break;
	case RECEIVER_STATE_ERROR:
		heading = obs_module_text("Status.Error.Heading");
		detail = receiver_error_detail(ctx);
		tone = AIRPLAY_STATUS_TONE_ERROR;
		break;
	case RECEIVER_STATE_STOPPED:
		heading = obs_module_text("Status.Stopped.Heading");
		detail = obs_module_text("Status.Stopped.Detail");
		tone = AIRPLAY_STATUS_TONE_STARTING;
		break;
	case RECEIVER_STATE_WAITING:
	default:
		break;
	}

	InterlockedExchange(&ctx->width, STATUS_FRAME_WIDTH);
	InterlockedExchange(&ctx->height, STATUS_FRAME_HEIGHT);
	airplay_status_frame_output(
		ctx->source, tone, obs_module_text("Status.Badge"), heading,
		detail, obs_module_text("Status.ReceiverLabel"),
		ctx->server_name, obs_module_text("Status.Footer"));

	pthread_mutex_unlock(&ctx->video_mutex);
}

/* ---------- UxPlay callbacks ---------- */

static void cb_conn_init(void *cls)
{
	struct airplay_source *ctx = cls;
	if (is_stopping(ctx))
		return;

	LONG open = InterlockedIncrement(&ctx->open_connections);
	LONG status_epoch = 0;
	if (open == 1) {
		clear_client_info(ctx);
		reset_timestamp_mapper(ctx);
		if (ctx->video_mutex_ready) {
			pthread_mutex_lock(&ctx->video_mutex);
			video_decoder_reset(ctx->vdec);
			ctx->video_frame_count = 0;
			ctx->video_no_output = 0;
			ctx->invalid_video_pkts = 0;
			if (!is_stopping(ctx))
				status_epoch = begin_receiver_status_locked(
					ctx, RECEIVER_STATE_CONNECTING);
			pthread_mutex_unlock(&ctx->video_mutex);
		}
	}

	if (ctx->audio_mutex_ready) {
		pthread_mutex_lock(&ctx->audio_mutex);
		if (open == 1)
			audio_decoder_reset(ctx->adec);
		ctx->audio_pkts = 0;
		ctx->audio_fails = 0;
		ctx->invalid_audio_pkts = 0;
		ctx->audio_output_logged = false;
		pthread_mutex_unlock(&ctx->audio_mutex);
	}
	blog(LOG_INFO, "[AirPlay] Connection init (open: %ld)", open);
	if (status_epoch)
		output_receiver_status(ctx, RECEIVER_STATE_CONNECTING,
				       status_epoch);
}

static void cb_conn_destroy(void *cls)
{
	struct airplay_source *ctx = cls;
	if (!ctx)
		return;

	LONG open = InterlockedDecrement(&ctx->open_connections);
	if (open < 0) {
		InterlockedExchange(&ctx->open_connections, 0);
		open = 0;
	}
	blog(LOG_INFO, "[AirPlay] Connection destroy (open: %ld)", open);
	if (open == 0 && !is_stopping(ctx)) {
		clear_client_info(ctx);
		LONG status_epoch = 0;
		if (ctx->video_mutex_ready) {
			pthread_mutex_lock(&ctx->video_mutex);
			if (!is_stopping(ctx) &&
			    InterlockedCompareExchange(&ctx->open_connections, 0,
						       0) == 0)
				status_epoch = begin_receiver_status_locked(
					ctx, RECEIVER_STATE_WAITING);
			pthread_mutex_unlock(&ctx->video_mutex);
		}
		if (status_epoch)
			output_receiver_status(ctx, RECEIVER_STATE_WAITING,
					       status_epoch);
	}
}

static void cb_conn_reset(void *cls, int reason)
{
	struct airplay_source *ctx = cls;
	if (is_stopping(ctx))
		return;
	blog(LOG_WARNING, "[AirPlay] Connection reset (reason=%d)", reason);
	LONG status_epoch = 0;
	if (ctx->video_mutex_ready) {
		pthread_mutex_lock(&ctx->video_mutex);
		if (!is_stopping(ctx) &&
		    InterlockedCompareExchange(&ctx->open_connections, 0, 0) > 0)
			status_epoch = begin_receiver_status_locked(
				ctx, RECEIVER_STATE_CONNECTING);
		pthread_mutex_unlock(&ctx->video_mutex);
	}
	if (status_epoch)
		output_receiver_status(ctx, RECEIVER_STATE_CONNECTING,
				       status_epoch);
}

static void cb_report_client_request(void *cls, char *device_id, char *model,
				     char *name, bool *admit)
{
	struct airplay_source *ctx = cls;
	if (is_stopping(ctx) || !ctx->client_mutex_ready)
		return;

	/* This receiver does not implement an allow-list. Explicitly admit the
	 * request while retaining only the bounded display metadata. */
	if (admit)
		*admit = true;
	char logged_name[256] = {0};
	char logged_model[128] = {0};
	pthread_mutex_lock(&ctx->client_mutex);
	copy_utf8_value(ctx->client_name, sizeof(ctx->client_name), name, "");
	copy_utf8_value(ctx->client_model, sizeof(ctx->client_model), model, "");
	copy_utf8_value(ctx->client_device_id, sizeof(ctx->client_device_id),
			device_id, "");
	copy_utf8_value(logged_name, sizeof(logged_name), ctx->client_name, "");
	copy_utf8_value(logged_model, sizeof(logged_model), ctx->client_model, "");
	pthread_mutex_unlock(&ctx->client_mutex);

	blog(LOG_INFO, "[AirPlay] Client request: name='%s' model='%s'",
	     logged_name[0] ? logged_name : "unknown",
	     logged_model[0] ? logged_model : "unknown");
	LONG status_epoch = 0;
	if (ctx->video_mutex_ready) {
		pthread_mutex_lock(&ctx->video_mutex);
		if (!is_stopping(ctx) &&
		    InterlockedCompareExchange(&ctx->open_connections, 0, 0) > 0 &&
		    InterlockedCompareExchange(&ctx->has_video, 0, 0) == 0)
			status_epoch = begin_receiver_status_locked(
				ctx, RECEIVER_STATE_CONNECTING);
		pthread_mutex_unlock(&ctx->video_mutex);
	}
	if (status_epoch)
		output_receiver_status(ctx, RECEIVER_STATE_CONNECTING,
				       status_epoch);
}

static void cb_conn_teardown(void *cls, bool *t96, bool *t110)
{
	(void)cls;
	if (!t96 || !t110)
		return;
	blog(LOG_INFO, "[AirPlay] Connection teardown (audio=%d video=%d)",
	     (int)*t96, (int)*t110);
}

static void cb_conn_feedback(void *cls)
{
	(void)cls; /* client keep-alive, ~2s interval */
}

static void cb_video_process(void *cls, raop_ntp_t *ntp,
			     video_decode_struct *data)
{
	(void)ntp;
	struct airplay_source *ctx = cls;
	if (is_stopping(ctx) || !data || !data->data || data->data_len <= 0 ||
	    data->data_len > MAX_VIDEO_PACKET_SIZE || !ctx->video_mutex_ready)
		return;

	pthread_mutex_lock(&ctx->video_mutex);
	if (is_stopping(ctx) || !ctx->vdec) {
		pthread_mutex_unlock(&ctx->video_mutex);
		return;
	}

	if (data->is_h265) {
		if (!ctx->warned_h265) {
			ctx->warned_h265 = true;
			blog(LOG_ERROR, "[AirPlay] H265 video not supported");
		}
		pthread_mutex_unlock(&ctx->video_mutex);
		return;
	}

	/* UxPlay marks a packet whose FairPlay decryption/NAL validation failed
	 * by changing the first byte from the Annex-B 0x00 to 0x01. Its renderer
	 * discards that marker; feeding it to FFmpeg only poisons decoder state. */
	if (data->data[0] != 0) {
		if (ctx->invalid_video_pkts < 5 ||
		    ctx->invalid_video_pkts % 500 == 0) {
			blog(LOG_WARNING,
			     "[AirPlay] skipped invalid/decryption-failed video packet #%d",
			     ctx->invalid_video_pkts + 1);
		}
		ctx->invalid_video_pkts++;
		pthread_mutex_unlock(&ctx->video_mutex);
		return;
	}

	if (ctx->video_frame_count < 5) {
		blog(LOG_INFO, "[AirPlay] video_process: %d bytes, %d NALs, ntp=%llu",
		     data->data_len, data->nal_count,
		     (unsigned long long)data->ntp_time_local);
	}

	uint64_t timestamp = map_timestamp(ctx, data->ntp_time_local);
	struct decoded_frame frame = {0};
	if (!video_decoder_decode(ctx->vdec, data->data, data->data_len,
				  timestamp, &frame)) {
		if (ctx->video_no_output < 10 ||
		    ctx->video_no_output % 500 == 0) {
			blog(LOG_WARNING,
			     "[AirPlay] video packet produced no output frame (count #%d)",
			     ctx->video_no_output + 1);
		}
		ctx->video_no_output++;
		pthread_mutex_unlock(&ctx->video_mutex);
		return;
	}
	ctx->video_frame_count++;

	struct obs_source_frame2 obs_frame = {0};
	obs_frame.format = VIDEO_FORMAT_NV12;
	obs_frame.range = frame.full_range ? VIDEO_RANGE_FULL
					   : VIDEO_RANGE_PARTIAL;
	obs_frame.width = frame.width;
	obs_frame.height = frame.height;
	obs_frame.timestamp = frame.pts > 0 ? (uint64_t)frame.pts : timestamp;

	obs_frame.data[0] = frame.data[0];
	obs_frame.data[1] = frame.data[1];
	obs_frame.linesize[0] = frame.linesize[0];
	obs_frame.linesize[1] = frame.linesize[1];

	enum video_colorspace color_space = frame.bt601 ? VIDEO_CS_601
							 : VIDEO_CS_709;
	if (!video_format_get_parameters_for_format(
		    color_space, obs_frame.range, VIDEO_FORMAT_NV12,
		    obs_frame.color_matrix, obs_frame.color_range_min,
		    obs_frame.color_range_max)) {
		blog(LOG_WARNING, "[AirPlay] Failed to map video color metadata");
		pthread_mutex_unlock(&ctx->video_mutex);
		return;
	}
	InterlockedExchange(&ctx->width, frame.width);
	InterlockedExchange(&ctx->height, frame.height);
	InterlockedExchange(&ctx->has_video, 1);
	InterlockedExchange(&ctx->receiver_state, RECEIVER_STATE_STREAMING);
	InterlockedIncrement(&ctx->status_epoch);
	obs_source_output_video2(ctx->source, &obs_frame);
	pthread_mutex_unlock(&ctx->video_mutex);
}

static void cb_video_pause(void *cls)
{
	(void)cls;
}

static void cb_video_resume(void *cls)
{
	(void)cls;
}

static void cb_video_reset(void *cls, reset_type_t reset_type)
{
	struct airplay_source *ctx = cls;
	if (is_stopping(ctx))
		return;
	blog(LOG_INFO, "[AirPlay] video reset (type=%d)", (int)reset_type);
	LONG status_epoch = 0;
	enum receiver_state state = RECEIVER_STATE_WAITING;
	if (ctx->video_mutex_ready) {
		pthread_mutex_lock(&ctx->video_mutex);
		video_decoder_reset(ctx->vdec);
		if (!is_stopping(ctx)) {
			state = InterlockedCompareExchange(
					&ctx->open_connections, 0, 0) > 0
				? RECEIVER_STATE_CONNECTING
				: RECEIVER_STATE_WAITING;
			status_epoch = begin_receiver_status_locked(ctx, state);
		}
		pthread_mutex_unlock(&ctx->video_mutex);
	}
	if (status_epoch)
		output_receiver_status(ctx, state, status_epoch);
}

static int cb_video_set_codec(void *cls, video_codec_t codec)
{
	(void)cls;
	if (codec == VIDEO_CODEC_H264)
		return 0;
	/* refuse H265 so the client falls back to H264 */
	blog(LOG_WARNING, "[AirPlay] client offered codec %d, requesting H264",
	     (int)codec);
	return -1;
}

static bool compressed_audio_header_is_valid(unsigned char ct,
					     unsigned char first)
{
	switch (ct) {
	case 8: /* AAC-ELD */
		return first == 0x80 || first == 0x81 || first == 0x82 ||
		       first == 0x8c || first == 0x8d || first == 0x8e;
	case 2: /* ALAC */
		return first == 0x20;
	case 4: /* AAC-LC/ADTS (rare in AirPlay mirroring) */
		return first == 0xff;
	default:
		/* Let audio_decoder_set_format report unsupported compression types. */
		return true;
	}
}

static void cb_audio_process(void *cls, raop_ntp_t *ntp,
			     audio_decode_struct *data)
{
	(void)ntp;
	struct airplay_source *ctx = cls;
	if (is_stopping(ctx) || !data || !data->data || data->data_len <= 0 ||
	    data->data_len > MAX_AUDIO_PACKET_SIZE || !ctx->audio_mutex_ready)
		return;

	pthread_mutex_lock(&ctx->audio_mutex);
	if (is_stopping(ctx)) {
		pthread_mutex_unlock(&ctx->audio_mutex);
		return;
	}

	if (ctx->audio_pkts < 5 || ctx->audio_pkts % 500 == 0) {
		blog(LOG_INFO,
		     "[AirPlay] audio pkt #%d: ct=%u len=%d first=0x%02x ntp=%llu",
		     ctx->audio_pkts, data->ct, data->data_len,
		     data->data_len > 0 ? data->data[0] : 0,
		     (unsigned long long)data->ntp_time_local);
	}
	ctx->audio_pkts++;

	struct obs_source_audio obs_audio = {0};
	uint64_t timestamp = map_timestamp(ctx, data->ntp_time_local);

	if (data->ct == 1) {
		/* Raw PCM S16LE 44100 stereo - no decoder needed */
		if (data->data_len % 4 != 0) {
			if (ctx->invalid_audio_pkts < 5 ||
			    ctx->invalid_audio_pkts % 500 == 0) {
				blog(LOG_WARNING,
				     "[AirPlay] skipped malformed PCM packet (len=%d)",
				     data->data_len);
			}
			ctx->invalid_audio_pkts++;
			goto done;
		}
		obs_audio.data[0] = data->data;
		obs_audio.frames = (uint32_t)(data->data_len / 4);
		obs_audio.speakers = SPEAKERS_STEREO;
		obs_audio.format = AUDIO_FORMAT_16BIT;
		obs_audio.samples_per_sec = 44100;
	} else {
		if (!compressed_audio_header_is_valid(data->ct, data->data[0])) {
			if (ctx->invalid_audio_pkts < 5 ||
			    ctx->invalid_audio_pkts % 500 == 0) {
				blog(LOG_WARNING,
				     "[AirPlay] skipped invalid audio packet: ct=%u first=0x%02x",
				     data->ct, data->data[0]);
			}
			ctx->invalid_audio_pkts++;
			goto done;
		}

		if (!ctx->adec) {
			if (!ctx->warned_no_adec) {
				ctx->warned_no_adec = true;
				blog(LOG_ERROR,
				     "[AirPlay] audio decoder missing - no audio");
			}
			goto done;
		}

		if (!audio_decoder_set_format(ctx->adec, data->ct)) {
			if (!ctx->warned_audio_ct) {
				ctx->warned_audio_ct = true;
				blog(LOG_ERROR,
				     "[AirPlay] cannot decode audio ct=%u",
				     data->ct);
			}
			goto done;
		}

		struct decoded_audio audio = {0};
		if (!audio_decoder_decode(ctx->adec, data->data,
					  data->data_len,
					  timestamp, &audio)) {
			if (ctx->audio_fails < 10)
				blog(LOG_WARNING,
				     "[AirPlay] audio decode failed (pkt %d, ct=%u, len=%d)",
				     ctx->audio_pkts, data->ct, data->data_len);
			ctx->audio_fails++;
			goto done;
		}

		obs_audio.data[0] = audio.data;
		obs_audio.frames = (uint32_t)audio.samples;
		obs_audio.speakers = (audio.channels == 2) ? SPEAKERS_STEREO
							   : SPEAKERS_MONO;
		obs_audio.format = AUDIO_FORMAT_FLOAT;
		obs_audio.samples_per_sec = (uint32_t)audio.sample_rate;
		if (audio.pts > 0)
			timestamp = audio.pts;
	}

	if (!obs_audio.data[0] || !obs_audio.frames ||
	    obs_audio.frames > MAX_AUDIO_FRAMES_PER_PACKET ||
	    obs_audio.samples_per_sec < 8000 ||
	    obs_audio.samples_per_sec > 192000)
		goto done;

	obs_audio.timestamp = timestamp;

	if (!ctx->audio_output_logged) {
		ctx->audio_output_logged = true;
		blog(LOG_INFO,
		     "[AirPlay] audio flowing: ct=%u %u Hz, %u frames/pkt",
		     data->ct, obs_audio.samples_per_sec, obs_audio.frames);
	}

	obs_source_output_audio(ctx->source, &obs_audio);

done:
	pthread_mutex_unlock(&ctx->audio_mutex);
}

static void cb_audio_flush(void *cls)
{
	struct airplay_source *ctx = cls;
	if (is_stopping(ctx) || !ctx->audio_mutex_ready)
		return;
	pthread_mutex_lock(&ctx->audio_mutex);
	audio_decoder_reset(ctx->adec);
	pthread_mutex_unlock(&ctx->audio_mutex);
}

static void cb_video_flush(void *cls)
{
	struct airplay_source *ctx = cls;
	if (is_stopping(ctx) || !ctx->video_mutex_ready)
		return;
	pthread_mutex_lock(&ctx->video_mutex);
	video_decoder_reset(ctx->vdec);
	pthread_mutex_unlock(&ctx->video_mutex);
}
static void cb_audio_set_volume(void *cls, float v) { (void)cls; (void)v; }

static double cb_audio_set_client_volume(void *cls)
{
	(void)cls;
	return 0.0; /* dB attenuation: 0 = full volume */
}

static void cb_audio_set_metadata(void *cls, const void *b, int l)
{
	(void)cls; (void)b; (void)l;
}

static void cb_audio_get_format(void *cls, unsigned char *ct,
				unsigned short *spf, bool *usingScreen,
				bool *isMedia, uint64_t *audioFormat)
{
	/* Informational: the client tells us the negotiated audio format.
	 * Never write to *ct - it is passed on to the RTP layer. */
	(void)cls;
	if (!ct || !spf || !usingScreen || !isMedia || !audioFormat)
		return;
	blog(LOG_INFO,
	     "[AirPlay] audio format: ct=%u spf=%u usingScreen=%d isMedia=%d audioFormat=0x%llx",
	     *ct, *spf, (int)*usingScreen, (int)*isMedia,
	     (unsigned long long)*audioFormat);
}

static void cb_video_report_size(void *cls, float *ws, float *hs,
				 float *w, float *h)
{
	struct airplay_source *ctx = cls;
	if (is_stopping(ctx) || !ws || !hs || !w || !h)
		return;
	if (*ws <= 0.0f || *hs <= 0.0f || *ws > 8192.0f || *hs > 8192.0f)
		return;
	InterlockedExchange(&ctx->width, (LONG)*ws);
	InterlockedExchange(&ctx->height, (LONG)*hs);
	blog(LOG_INFO, "[AirPlay] Video size: %.0fx%.0f (source) %.0fx%.0f",
	     *ws, *hs, *w, *h);
}

static void cb_log(void *cls, int level, const char *msg)
{
	(void)cls;
	if (!msg)
		return;
	int obs_level = LOG_DEBUG;
	if (level <= LOGGER_ERR) obs_level = LOG_ERROR;
	else if (level <= LOGGER_WARNING) obs_level = LOG_WARNING;
	else if (level <= LOGGER_INFO) obs_level = LOG_INFO;
	blog(obs_level, "[AirPlay-UxPlay] %s", msg);
}

/* ---------- Server lifecycle ---------- */

static void stop_server(struct airplay_source *ctx);

static bool start_server(struct airplay_source *ctx)
{
	if (!ctx || !ctx->video_mutex_ready || !ctx->audio_mutex_ready ||
	    !ctx->timestamp_mutex_ready)
		return false;

	InterlockedExchange(&ctx->stopping, 0);
	InterlockedExchange(&ctx->open_connections, 0);
	InterlockedExchange(&ctx->server_running, 0);
	InterlockedExchange(&ctx->receiver_state, RECEIVER_STATE_STARTING);
	InterlockedExchange(&ctx->start_error, RECEIVER_ERROR_NONE);
	InterlockedExchange(&ctx->has_video, 0);
	clear_client_info(ctx);
	reset_timestamp_mapper(ctx);

	raop_callbacks_t cbs;
	memset(&cbs, 0, sizeof(cbs));
	cbs.cls = ctx;
	cbs.conn_init = cb_conn_init;
	cbs.conn_destroy = cb_conn_destroy;
	cbs.conn_reset = cb_conn_reset;
	cbs.conn_teardown = cb_conn_teardown;
	cbs.conn_feedback = cb_conn_feedback;
	cbs.audio_process = cb_audio_process;
	cbs.video_process = cb_video_process;
	cbs.video_pause = cb_video_pause;
	cbs.video_resume = cb_video_resume;
	cbs.video_reset = cb_video_reset;
	cbs.video_set_codec = cb_video_set_codec;
	cbs.audio_flush = cb_audio_flush;
	cbs.video_flush = cb_video_flush;
	cbs.audio_set_volume = cb_audio_set_volume;
	cbs.audio_set_client_volume = cb_audio_set_client_volume;
	cbs.audio_get_format = cb_audio_get_format;
	cbs.video_report_size = cb_video_report_size;
	cbs.audio_set_metadata = cb_audio_set_metadata;
	cbs.report_client_request = cb_report_client_request;

	ctx->raop = raop_init(&cbs);
	if (!ctx->raop) {
		blog(LOG_ERROR, "[AirPlay] raop_init failed");
		InterlockedExchange(&ctx->start_error,
				    RECEIVER_ERROR_INITIALIZATION);
		InterlockedExchange(&ctx->receiver_state, RECEIVER_STATE_ERROR);
		return false;
	}

	raop_set_log_callback(ctx->raop, cb_log, NULL);
	raop_set_log_level(ctx->raop, LOGGER_INFO);

	/* MAC address - random to avoid exposing the real adapter MAC;
	 * also seeds the pairing key generation in raop_init2 */
	char mac_str[18] = {0};
	if (ctx->use_random_mac)
		generate_random_mac(mac_str, sizeof(mac_str));
	else
		generate_stable_mac(ctx->server_name, mac_str, sizeof(mac_str));
	blog(LOG_INFO, "[AirPlay] using MAC: %s", mac_str);

	/* nohold=1: a new client takes over from the connected one.
	 * keyfile="": ephemeral pairing key (must not be NULL) */
	if (raop_init2(ctx->raop, 1, mac_str, "") != 0) {
		blog(LOG_ERROR, "[AirPlay] raop_init2 failed");
		InterlockedExchange(&ctx->start_error,
				    RECEIVER_ERROR_INITIALIZATION);
		/* The pinned UxPlay build makes this partial-init path null-safe. */
		raop_destroy(ctx->raop);
		ctx->raop = NULL;
		InterlockedExchange(&ctx->receiver_state, RECEIVER_STATE_ERROR);
		return false;
	}

	if (ctx->cfg_width > 0)
		raop_set_plist(ctx->raop, "width", ctx->cfg_width);
	if (ctx->cfg_height > 0)
		raop_set_plist(ctx->raop, "height", ctx->cfg_height);
	if (ctx->cfg_fps > 0)
		raop_set_plist(ctx->raop, "refreshRate", ctx->cfg_fps);
	if (ctx->cfg_max_fps > 0)
		raop_set_plist(ctx->raop, "maxFPS", ctx->cfg_max_fps);

	unsigned short tcp[2] = {AIRPLAY_TCP_MIRROR, AIRPLAY_TCP_RTSP};
	unsigned short udp[3] = {AIRPLAY_UDP_TIMING, AIRPLAY_UDP_CONTROL,
				 AIRPLAY_UDP_DATA};
	raop_set_tcp_ports(ctx->raop, tcp);
	raop_set_udp_ports(ctx->raop, udp);

	unsigned short port = raop_get_port(ctx->raop);
	blog(LOG_INFO, "[AirPlay] calling raop_start_httpd (port %d)...", port);
	int start_ret = raop_start_httpd(ctx->raop, &port);
	if (start_ret < 0) {
		/* The installer opens only the fixed ports. A dynamic fallback would
		 * advertise an unreachable receiver and would not solve collisions on
		 * the remaining fixed media ports. */
		blog(LOG_ERROR,
		     "[AirPlay] cannot bind TCP port %d (error %d); close uxplay-windows or another AirPlay source",
		     port, start_ret);
		InterlockedExchange(&ctx->start_error,
				    RECEIVER_ERROR_PORT_IN_USE);
		raop_destroy(ctx->raop);
		ctx->raop = NULL;
		InterlockedExchange(&ctx->receiver_state, RECEIVER_STATE_ERROR);
		return false;
	}
	raop_set_port(ctx->raop, port);
	blog(LOG_INFO, "[AirPlay] raop httpd OK on port %d", port);

	/* Give the httpd thread a moment to stabilize */
	Sleep(100);

	char hw[6];
	int hw_len = 0;
	parse_hw_addr(mac_str, hw, &hw_len);

	blog(LOG_INFO, "[AirPlay] calling dnssd_init...");
	int err = 0;
	ctx->dnssd = dnssd_init(ctx->server_name,
				(int)strlen(ctx->server_name),
				hw, hw_len, &err, 0 /* no pin/password */);
	if (err || !ctx->dnssd) {
		blog(LOG_ERROR, "[AirPlay] dnssd_init failed (err=%d)", err);
		InterlockedExchange(&ctx->start_error, RECEIVER_ERROR_BONJOUR);
		if (ctx->dnssd) {
			dnssd_destroy(ctx->dnssd);
			ctx->dnssd = NULL;
		}
		raop_destroy(ctx->raop);
		ctx->raop = NULL;
		InterlockedExchange(&ctx->receiver_state, RECEIVER_STATE_ERROR);
		return false;
	}

	raop_set_dnssd(ctx->raop, ctx->dnssd);

	int raop_reg = dnssd_register_raop(ctx->dnssd, port);
	ctx->dnssd_raop_registered = raop_reg == 0;
	/* the airplay service must point at the same httpd port */
	int airplay_reg = dnssd_register_airplay(ctx->dnssd, port);
	ctx->dnssd_airplay_registered = airplay_reg == 0;
	if (!ctx->dnssd_raop_registered || !ctx->dnssd_airplay_registered) {
		blog(LOG_ERROR,
		     "[AirPlay] Bonjour registration failed (raop=%d airplay=%d)",
		     raop_reg, airplay_reg);
		InterlockedExchange(&ctx->start_error, RECEIVER_ERROR_BONJOUR);
		stop_server(ctx);
		InterlockedExchange(&ctx->receiver_state, RECEIVER_STATE_ERROR);
		return false;
	}

	blog(LOG_INFO, "[AirPlay] Server started: '%s' port=%d mac=%s",
	     ctx->server_name, port, mac_str);
	InterlockedExchange(&ctx->server_running, 1);

	return true;
}

static void stop_server(struct airplay_source *ctx)
{
	if (!ctx)
		return;

	InterlockedExchange(&ctx->stopping, 1);
	InterlockedExchange(&ctx->server_running, 0);
	InterlockedIncrement(&ctx->status_epoch);

	/* Join HTTP/RTP callbacks before unregistering Bonjour: UxPlay request
	 * handlers can read dnssd data, and unregistering releases that data. */
	if (ctx->raop)
		raop_stop_httpd(ctx->raop);

	if (ctx->dnssd && ctx->dnssd_raop_registered) {
		dnssd_unregister_raop(ctx->dnssd);
		ctx->dnssd_raop_registered = false;
	}
	if (ctx->dnssd && ctx->dnssd_airplay_registered) {
		dnssd_unregister_airplay(ctx->dnssd);
		ctx->dnssd_airplay_registered = false;
	}
	if (ctx->raop) {
		raop_destroy(ctx->raop);
		ctx->raop = NULL;
	}
	if (ctx->dnssd) {
		dnssd_destroy(ctx->dnssd);
		ctx->dnssd = NULL;
	}
	InterlockedExchange(&ctx->open_connections, 0);
	InterlockedExchange(&ctx->has_video, 0);
	InterlockedExchange(&ctx->receiver_state, RECEIVER_STATE_STOPPED);
	clear_client_info(ctx);
}

static bool start_server_with_status(struct airplay_source *ctx)
{
	/* A previous stop has joined every UxPlay callback, so it is safe to
	 * reopen status output before starting the new server instance. */
	InterlockedExchange(&ctx->stopping, 0);
	LONG status_epoch =
		begin_receiver_status(ctx, RECEIVER_STATE_STARTING);
	if (status_epoch)
		output_receiver_status(ctx, RECEIVER_STATE_STARTING,
				       status_epoch);
	bool started = start_server(ctx);
	if (!started)
		InterlockedExchange(&ctx->stopping, 0);

	enum receiver_state state = RECEIVER_STATE_ERROR;
	status_epoch = 0;
	if (!started) {
		status_epoch = begin_receiver_status(ctx, RECEIVER_STATE_ERROR);
	} else if (ctx->video_mutex_ready) {
		/* The HTTP daemon is already accepting clients before start_server()
		 * returns. Do not let this post-start slate overwrite a connection or
		 * first frame that won that race. */
		pthread_mutex_lock(&ctx->video_mutex);
		if (!is_stopping(ctx) &&
		    InterlockedCompareExchange(&ctx->has_video, 0, 0) == 0) {
			state = InterlockedCompareExchange(
					&ctx->open_connections, 0, 0) > 0
				? RECEIVER_STATE_CONNECTING
				: RECEIVER_STATE_WAITING;
			status_epoch = begin_receiver_status_locked(ctx, state);
		}
		pthread_mutex_unlock(&ctx->video_mutex);
	}
	if (status_epoch)
		output_receiver_status(ctx, state, status_epoch);
	return started;
}

static void show_managed_stopped_status(struct airplay_source *ctx)
{
	if (!ctx)
		return;

	/* stop_server() deliberately closes the callback gate.  Reopen only the
	 * status renderer after every protocol thread has been joined; no RAOP or
	 * Bonjour component is created while this managed source is disabled. */
	InterlockedExchange(&ctx->stopping, 0);
	InterlockedExchange(&ctx->server_running, 0);
	InterlockedExchange(&ctx->open_connections, 0);
	InterlockedExchange(&ctx->start_error, RECEIVER_ERROR_NONE);
	clear_client_info(ctx);
	LONG status_epoch =
		begin_receiver_status(ctx, RECEIVER_STATE_STOPPED);
	if (status_epoch)
		output_receiver_status(ctx, RECEIVER_STATE_STOPPED,
				       status_epoch);
}

/* ---------- Helpers ---------- */

static void parse_resolution(const char *res_str, int *w, int *h)
{
	*w = 0;
	*h = 0;
	if (!res_str || strcmp(res_str, "0x0") == 0)
		return;

	int parsed_w = 0;
	int parsed_h = 0;
	char trailing = '\0';
	if (sscanf(res_str, "%dx%d%c", &parsed_w, &parsed_h, &trailing) != 2 ||
	    parsed_w <= 0 || parsed_h <= 0 || parsed_w > 8192 ||
	    parsed_h > 8192)
		return;

	*w = parsed_w;
	*h = parsed_h;
}

static bool init_source_mutexes(struct airplay_source *ctx)
{
	if (pthread_mutex_init(&ctx->video_mutex, NULL) != 0)
		return false;
	ctx->video_mutex_ready = true;

	if (pthread_mutex_init(&ctx->audio_mutex, NULL) != 0)
		return false;
	ctx->audio_mutex_ready = true;

	if (pthread_mutex_init(&ctx->timestamp_mutex, NULL) != 0)
		return false;
	ctx->timestamp_mutex_ready = true;

	if (pthread_mutex_init(&ctx->client_mutex, NULL) != 0)
		return false;
	ctx->client_mutex_ready = true;
	return true;
}

static void destroy_source_mutexes(struct airplay_source *ctx)
{
	if (ctx->client_mutex_ready) {
		pthread_mutex_destroy(&ctx->client_mutex);
		ctx->client_mutex_ready = false;
	}
	if (ctx->timestamp_mutex_ready) {
		pthread_mutex_destroy(&ctx->timestamp_mutex);
		ctx->timestamp_mutex_ready = false;
	}
	if (ctx->audio_mutex_ready) {
		pthread_mutex_destroy(&ctx->audio_mutex);
		ctx->audio_mutex_ready = false;
	}
	if (ctx->video_mutex_ready) {
		pthread_mutex_destroy(&ctx->video_mutex);
		ctx->video_mutex_ready = false;
	}
}

/* ---------- OBS Source API ---------- */

static const char *airplay_get_name(void *unused)
{
	(void)unused;
	return obs_module_text("Source.Name");
}

static void *airplay_create(obs_data_t *settings, obs_source_t *source)
{
	if (InterlockedCompareExchange(&g_instance_claimed, 1, 0) != 0) {
		blog(LOG_ERROR,
		     "[AirPlay] Only one AirPlay Live Source can run at a time");
		return NULL;
	}

	struct airplay_source *ctx = bzalloc(sizeof(struct airplay_source));
	if (!ctx || !settings || !source) {
		bfree(ctx);
		InterlockedExchange(&g_instance_claimed, 0);
		return NULL;
	}

	ctx->source = source;
	copy_utf8_value(ctx->reconnect_id, sizeof(ctx->reconnect_id),
		obs_data_get_string(settings, "receiver_reconnect_id"), "");
	obs_data_set_int(settings, "receiver_reconnect_version", 1);
	obs_data_set_string(settings, "receiver_reconnect_completed", "");
	obs_data_set_bool(settings, "receiver_reconnect_ok", false);
	ctx->width = STATUS_FRAME_WIDTH;
	ctx->height = STATUS_FRAME_HEIGHT;

	if (!init_source_mutexes(ctx)) {
		blog(LOG_ERROR, "[AirPlay] synchronization initialization failed");
		destroy_source_mutexes(ctx);
		bfree(ctx);
		InterlockedExchange(&g_instance_claimed, 0);
		return NULL;
	}

	ctx->vdec = video_decoder_create();
	ctx->adec = audio_decoder_create();
	if (!ctx->vdec || !ctx->adec) {
		blog(LOG_ERROR, "[AirPlay] decoder initialization failed");
		if (ctx->vdec) video_decoder_destroy(ctx->vdec);
		if (ctx->adec) audio_decoder_destroy(ctx->adec);
		destroy_source_mutexes(ctx);
		bfree(ctx);
		InterlockedExchange(&g_instance_claimed, 0);
		return NULL;
	}

	const char *name = obs_data_get_string(settings, "server_name");
	copy_server_name(ctx->server_name, sizeof(ctx->server_name), name);
	ctx->use_random_mac = obs_data_get_bool(settings, "use_random_mac");
	ctx->managed_by_fus = obs_data_get_bool(settings, "managed_by_fus");
	ctx->receiver_enabled =
		obs_data_get_bool(settings, "receiver_enabled");

	/* receiver_enabled is persisted with the OBS scene collection, but FUS
	 * ownership is session-scoped.  Never treat a persisted true value as
	 * permission to bind the receiver ports during OBS startup/reload.  Writing
	 * false back to the live source settings also makes the stopped state the
	 * new persistence baseline.  FUS must issue SetInputSettings(true) after it
	 * has deliberately started the plugin in this process. */
	if (ctx->managed_by_fus) {
		ctx->receiver_enabled = false;
		persist_receiver_session_disabled(settings);
	}

	const char *res = obs_data_get_string(settings, "resolution");
	parse_resolution(res, &ctx->cfg_width, &ctx->cfg_height);

	int fps_val = (int)obs_data_get_int(settings, "fps_preset");
	if (fps_val < 1 || fps_val > 240)
		fps_val = 60;
	ctx->cfg_fps = fps_val;
	ctx->cfg_max_fps = fps_val;


	if (!receiver_should_run(ctx)) {
		blog(LOG_INFO,
		     "[AirPlay] FUS-managed receiver created session-disabled; RAOP and Bonjour were not started");
		show_managed_stopped_status(ctx);
	} else {
		/* Keep a failed receiver source alive so OBS can show an actionable
		 * error card and the user can retry after closing the conflicting
		 * application. */
		if (!start_server_with_status(ctx))
			blog(LOG_ERROR,
			     "[AirPlay] Receiver is visible in OBS but failed to start; use its properties to retry");
	}

	return ctx;
}

static void airplay_destroy(void *data)
{
	struct airplay_source *ctx = data;
	if (!ctx) return;

	stop_server(ctx);

	if (ctx->vdec) video_decoder_destroy(ctx->vdec);
	if (ctx->adec) audio_decoder_destroy(ctx->adec);

	destroy_source_mutexes(ctx);
	bfree(ctx);
	InterlockedExchange(&g_instance_claimed, 0);
}

static void airplay_update(void *data, obs_data_t *settings)
{
	struct airplay_source *ctx = data;
	if (!ctx || !settings)
		return;

	const char *name = obs_data_get_string(settings, "server_name");
	bool random_mac = obs_data_get_bool(settings, "use_random_mac");
	bool managed_by_fus = obs_data_get_bool(settings, "managed_by_fus");
	bool receiver_enabled =
		obs_data_get_bool(settings, "receiver_enabled");
	const char *res = obs_data_get_string(settings, "resolution");
	int fps = (int)obs_data_get_int(settings, "fps_preset");
	if (fps < 1 || fps > 240)
		fps = 60;

	int w = 0, h = 0;
	parse_resolution(res, &w, &h);

	char normalized_name[sizeof(ctx->server_name)] = {0};
	char reconnect_id[sizeof(ctx->reconnect_id)] = {0};
	copy_utf8_value(reconnect_id, sizeof(reconnect_id),
		obs_data_get_string(settings, "receiver_reconnect_id"), "");
	bool reconnect = managed_by_fus && receiver_enabled && reconnect_id[0] &&
		strcmp(reconnect_id, ctx->reconnect_id) != 0;
	copy_server_name(normalized_name, sizeof(normalized_name), name);
	bool engine_settings_changed =
		strcmp(ctx->server_name, normalized_name) != 0 ||
		ctx->use_random_mac != random_mac || ctx->cfg_width != w ||
		ctx->cfg_height != h || ctx->cfg_fps != fps;
	bool was_requested = receiver_should_run(ctx);
	bool should_run = !managed_by_fus || receiver_enabled;
	bool was_running =
		InterlockedCompareExchange(&ctx->server_running, 0, 0) != 0;

	/* Stop before replacing settings so no callback can observe a partially
	 * updated configuration.  Turning off a FUS-managed receiver follows the
	 * same joined teardown path as source destruction. */
	if (reconnect || (was_running && (!should_run || engine_settings_changed)))
		stop_server(ctx);

	copy_server_name(ctx->server_name, sizeof(ctx->server_name),
			 normalized_name);
	ctx->use_random_mac = random_mac;
	ctx->managed_by_fus = managed_by_fus;
	ctx->receiver_enabled = receiver_enabled;
	ctx->cfg_width = w;
	ctx->cfg_height = h;
	ctx->cfg_fps = fps;
	ctx->cfg_max_fps = fps;

	if (!should_run) {
		blog(LOG_INFO,
		     "[AirPlay] FUS-managed receiver disabled; RAOP and Bonjour are stopped");
		show_managed_stopped_status(ctx);
		return;
	}

	if (reconnect || !was_running || engine_settings_changed || !was_requested) {
		if (!start_server_with_status(ctx))
			blog(LOG_ERROR, "[AirPlay] Receiver start/restart failed");
	}
	if (reconnect) {
		copy_utf8_value(ctx->reconnect_id, sizeof(ctx->reconnect_id), reconnect_id, "");
		obs_data_set_bool(settings, "receiver_reconnect_ok",
			InterlockedCompareExchange(&ctx->server_running, 0, 0) != 0);
		obs_data_set_string(settings, "receiver_reconnect_completed", reconnect_id);
		blog(LOG_INFO, "[AirPlay] Explicit reconnect completed");
	}
}

static const char *receiver_state_text(enum receiver_state state)
{
	switch (state) {
	case RECEIVER_STATE_STARTING:
		return obs_module_text("Status.Starting.Heading");
	case RECEIVER_STATE_WAITING:
		return obs_module_text("Status.Waiting.Heading");
	case RECEIVER_STATE_CONNECTING:
		return obs_module_text("Status.Connecting.Heading");
	case RECEIVER_STATE_STREAMING:
		return obs_module_text("Status.Streaming.Heading");
	case RECEIVER_STATE_ERROR:
		return obs_module_text("Status.Error.Heading");
	case RECEIVER_STATE_STOPPED:
	default:
		return obs_module_text("Status.Stopped.Heading");
	}
}

static void build_property_status(struct airplay_source *ctx, char *text,
				  size_t text_size)
{
	if (!text || text_size == 0)
		return;
	if (!ctx) {
		copy_utf8_value(text, text_size,
				obs_module_text("Property.Status.NotCreated"), "");
		return;
	}

	char client_name[256] = {0};
	char client_model[128] = {0};
	get_client_info(ctx, client_name, sizeof(client_name),
			client_model, sizeof(client_model));
	const char *device = client_name[0] ? client_name :
		obs_module_text("Status.NoClient");
	const char *control = ctx->managed_by_fus
		? (ctx->receiver_enabled
			? obs_module_text("Property.Status.ManagedEnabled")
			: obs_module_text("Property.Status.ManagedDisabled"))
		: obs_module_text("Property.Status.Manual");
	enum receiver_state state = (enum receiver_state)
		InterlockedCompareExchange(&ctx->receiver_state, 0, 0);

	snprintf(text, text_size, "%s\n%s%s\n%s%s%s%s%s\n%s%s",
		 receiver_state_text(state),
		 obs_module_text("Property.Status.Receiver"), ctx->server_name,
		 obs_module_text("Property.Status.Client"), device,
		 client_model[0] ? " (" : "",
		 client_model[0] ? client_model : "",
		 client_model[0] ? ")" : "",
		 obs_module_text("Property.Status.Control"), control);
	if (state == RECEIVER_STATE_ERROR) {
		size_t used = strlen(text);
		if (used + 2 < text_size)
			snprintf(text + used, text_size - used, "\n%s",
				 receiver_error_detail(ctx));
	}
}

static obs_properties_t *airplay_get_properties(void *data)
{
	struct airplay_source *ctx = data;
	obs_properties_t *p = obs_properties_create();
	obs_properties_set_flags(p, OBS_PROPERTIES_DEFER_UPDATE);
	char status[1200] = {0};
	build_property_status(ctx, status, sizeof(status));
	obs_properties_add_text(p, "receiver_status", status, OBS_TEXT_INFO);
	obs_properties_add_text(p, "usage_help",
				obs_module_text("Property.UsageHelp"), OBS_TEXT_INFO);

	obs_properties_add_text(p, "server_name",
				obs_module_text("Property.ServerName"),
				OBS_TEXT_DEFAULT);

	obs_property_t *res = obs_properties_add_list(p, "resolution",
		obs_module_text("Property.Resolution"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(res, obs_module_text("Resolution.Native"),
				     "0x0");
	obs_property_list_add_string(res, "1920x1080 (1080p)",
				     "1920x1080");
	obs_property_list_add_string(res, "1280x720 (720p)",
				     "1280x720");
	obs_property_list_add_string(res, "3840x2160 (4K)",
				     "3840x2160");
	obs_property_list_add_string(res, "2560x1440 (1440p)",
				     "2560x1440");

	obs_property_t *fps = obs_properties_add_list(p, "fps_preset",
		obs_module_text("Property.FPS"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fps, "60 FPS", 60);
	obs_property_list_add_int(fps, "30 FPS", 30);
	obs_property_list_add_int(fps, "120 FPS", 120);
	obs_property_list_add_int(fps, "24 FPS", 24);

	obs_properties_add_bool(p, "use_random_mac",
				obs_module_text("Property.RandomIdentity"));

	return p;
}

static void airplay_get_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, "server_name", DEFAULT_SERVER_NAME);
	obs_data_set_default_string(s, "resolution", "0x0");
	obs_data_set_default_int(s, "fps_preset", 60);
	obs_data_set_default_bool(s, "use_random_mac", false);
	obs_data_set_default_bool(s, "managed_by_fus", false);
	obs_data_set_default_bool(s, "receiver_enabled", true);
}

static uint32_t airplay_get_width(void *data)
{
	struct airplay_source *ctx = data;
	LONG width = ctx ? InterlockedCompareExchange(&ctx->width, 0, 0) : 0;
	return width > 0 ? (uint32_t)width : 0;
}

static uint32_t airplay_get_height(void *data)
{
	struct airplay_source *ctx = data;
	LONG height = ctx ? InterlockedCompareExchange(&ctx->height, 0, 0) : 0;
	return height > 0 ? (uint32_t)height : 0;
}

/* ---------- Register ---------- */
void airplay_source_register(void)
{
	struct obs_source_info info = {0};
	info.id = "airplay_live_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
			    OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = airplay_get_name;
	info.create = airplay_create;
	info.destroy = airplay_destroy;
	info.update = airplay_update;
	info.get_properties = airplay_get_properties;
	info.get_defaults = airplay_get_defaults;
	info.get_width = airplay_get_width;
	info.get_height = airplay_get_height;
	info.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE;

	obs_register_source(&info);
}
