#pragma once

#include <obs-module.h>
#include <stdbool.h>

enum airplay_status_tone {
	AIRPLAY_STATUS_TONE_STARTING,
	AIRPLAY_STATUS_TONE_READY,
	AIRPLAY_STATUS_TONE_CONNECTED,
	AIRPLAY_STATUS_TONE_ERROR,
};

/* Draws and submits a single, opaque 1280x720 BGRX status frame. libobs
 * copies the pixels synchronously, so all GDI resources remain local to the
 * call and the function is safe to use from UxPlay callback threads. */
bool airplay_status_frame_output(obs_source_t *source,
				 enum airplay_status_tone tone,
				 const char *badge, const char *heading,
				 const char *detail,
				 const char *receiver_label,
				 const char *receiver_name,
				 const char *footer);
