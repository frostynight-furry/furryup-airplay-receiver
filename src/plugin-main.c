/*
 * AirPlay Live Source
 * Receives AirPlay frames directly inside OBS. There is no renderer window,
 * so minimizing other windows cannot interrupt the source.
 */

#include <obs-module.h>
#include "airplay-source.h"
#include "frontend-controller.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("airplay-live-source", "en-US")

bool obs_module_load(void)
{
	airplay_source_register();
	blog(LOG_INFO, "[AirPlay Live] Plugin loaded (version %s)",
	     PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	airplay_frontend_controller_register();
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[AirPlay Live] Plugin unloaded");
}

const char *obs_module_name(void)
{
	return obs_module_text("Module.Name");
}

const char *obs_module_description(void)
{
	return obs_module_text("Module.Description");
}
