/*
 * Small OBS frontend bridge for discoverability.
 *
 * The receiver itself remains a native OBS source with no renderer HWND.  A
 * Tools-menu action gives users a client-like entry point: one click creates
 * or reuses the receiver in the current scene and opens its status/settings
 * window.  Frontend functions are resolved from the OBS host at runtime to
 * avoid taking a Qt build dependency merely for a menu action.
 */

#include "frontend-controller.h"

#include <obs-module.h>

#include <stdbool.h>
#include <limits.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define AIRPLAY_SOURCE_ID "airplay_live_source"

#ifdef _WIN32

typedef void (*frontend_callback_t)(void *private_data);
typedef void (*frontend_add_tools_item_t)(const char *name,
					  frontend_callback_t callback,
					  void *private_data);
typedef obs_source_t *(*frontend_get_current_scene_t)(void);
typedef void (*frontend_open_source_properties_t)(obs_source_t *source);
typedef void *(*frontend_get_main_window_handle_t)(void);

typedef void (*obs_enum_sources_t)(bool (*enum_proc)(void *, obs_source_t *),
				   void *param);
typedef const char *(*obs_source_get_unversioned_id_t)(
	const obs_source_t *source);
typedef obs_source_t *(*obs_source_get_ref_t)(obs_source_t *source);
typedef void (*obs_source_release_t)(obs_source_t *source);
typedef obs_source_t *(*obs_source_create_t)(const char *id,
					      const char *name,
					      obs_data_t *settings,
					      obs_data_t *hotkey_data);
typedef const char *(*obs_source_get_name_t)(const obs_source_t *source);
typedef obs_scene_t *(*obs_scene_from_source_t)(const obs_source_t *source);
typedef obs_sceneitem_t *(*obs_scene_find_source_t)(obs_scene_t *scene,
						     const char *name);
typedef obs_sceneitem_t *(*obs_scene_add_t)(obs_scene_t *scene,
					    obs_source_t *source);

struct frontend_api {
	frontend_add_tools_item_t add_tools_item;
	frontend_get_current_scene_t get_current_scene;
	frontend_open_source_properties_t open_source_properties;
	frontend_get_main_window_handle_t get_main_window_handle;
	obs_enum_sources_t enum_sources;
	obs_source_get_unversioned_id_t source_get_unversioned_id;
	obs_source_get_ref_t source_get_ref;
	obs_source_release_t source_release;
	obs_source_create_t source_create;
	obs_source_get_name_t source_get_name;
	obs_scene_from_source_t scene_from_source;
	obs_scene_find_source_t scene_find_source;
	obs_scene_add_t scene_add;
};

static struct frontend_api g_api;
static bool g_registered;

static bool resolve_proc(HMODULE module, const char *name, void **out)
{
	if (!module || !name || !out)
		return false;
	*out = (void *)GetProcAddress(module, name);
	return *out != NULL;
}

static bool resolve_frontend_api(void)
{
	HMODULE frontend = GetModuleHandleW(L"obs-frontend-api.dll");
	HMODULE libobs = GetModuleHandleW(L"obs.dll");
	if (!frontend || !libobs)
		return false;

#define RESOLVE(module, field, symbol)                                      \
	do {                                                                  \
		if (!resolve_proc((module), (symbol), (void **)&g_api.field))   \
			return false;                                             \
	} while (false)

	RESOLVE(frontend, add_tools_item, "obs_frontend_add_tools_menu_item");
	RESOLVE(frontend, get_current_scene, "obs_frontend_get_current_scene");
	RESOLVE(frontend, open_source_properties,
		"obs_frontend_open_source_properties");
	RESOLVE(frontend, get_main_window_handle,
		"obs_frontend_get_main_window_handle");

	RESOLVE(libobs, enum_sources, "obs_enum_sources");
	RESOLVE(libobs, source_get_unversioned_id,
		"obs_source_get_unversioned_id");
	RESOLVE(libobs, source_get_ref, "obs_source_get_ref");
	RESOLVE(libobs, source_release, "obs_source_release");
	RESOLVE(libobs, source_create, "obs_source_create");
	RESOLVE(libobs, source_get_name, "obs_source_get_name");
	RESOLVE(libobs, scene_from_source, "obs_scene_from_source");
	RESOLVE(libobs, scene_find_source, "obs_scene_find_source");
	RESOLVE(libobs, scene_add, "obs_scene_add");

#undef RESOLVE
	return true;
}

static void utf8_to_wide(const char *text, wchar_t *wide, size_t wide_count)
{
	if (!wide || wide_count == 0)
		return;
	wide[0] = L'\0';
	if (!text || !text[0])
		return;

	int capacity = wide_count > INT_MAX ? INT_MAX : (int)wide_count;
	if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, capacity))
		wide[0] = L'\0';
	wide[wide_count - 1] = L'\0';
}

static void show_controller_error(const char *message_key)
{
	wchar_t title[256];
	wchar_t message[1024];
	utf8_to_wide(obs_module_text("Controller.Title"), title,
		     sizeof(title) / sizeof(title[0]));
	utf8_to_wide(obs_module_text(message_key), message,
		     sizeof(message) / sizeof(message[0]));
	HWND owner = g_api.get_main_window_handle
			     ? (HWND)g_api.get_main_window_handle()
			     : NULL;
	MessageBoxW(owner, message, title, MB_OK | MB_ICONERROR);
}

struct source_search {
	obs_source_t *source;
};

static bool find_receiver_source(void *private_data, obs_source_t *source)
{
	struct source_search *search = private_data;
	const char *id = g_api.source_get_unversioned_id(source);
	if (!id || strcmp(id, AIRPLAY_SOURCE_ID) != 0)
		return true;

	search->source = g_api.source_get_ref(source);
	return false;
}

static obs_source_t *get_receiver_source(void)
{
	struct source_search search = {0};
	g_api.enum_sources(find_receiver_source, &search);
	return search.source;
}

static bool add_source_to_current_scene(obs_source_t *source,
					 bool *was_added)
{
	if (was_added)
		*was_added = false;

	obs_source_t *scene_source = g_api.get_current_scene();
	if (!scene_source)
		return false;

	obs_scene_t *scene = g_api.scene_from_source(scene_source);
	const char *source_name = g_api.source_get_name(source);
	bool present = scene && source_name &&
		g_api.scene_find_source(scene, source_name) != NULL;
	bool success = scene && (present || g_api.scene_add(scene, source));
	if (success && !present && was_added)
		*was_added = true;

	g_api.source_release(scene_source);
	return success;
}

static void open_controller(void *private_data)
{
	UNUSED_PARAMETER(private_data);

	obs_source_t *source = get_receiver_source();
	bool created = false;
	if (!source) {
		source = g_api.source_create(AIRPLAY_SOURCE_ID,
					 obs_module_text("Source.Name"), NULL, NULL);
		created = source != NULL;
	}

	if (!source) {
		show_controller_error("Controller.Error.CreateFailed");
		return;
	}

	bool added = false;
	if (!add_source_to_current_scene(source, &added)) {
		g_api.source_release(source);
		show_controller_error("Controller.Error.NoScene");
		return;
	}

	blog(LOG_INFO,
	     "[AirPlay Live] Controller opened (%s source, %s current scene)",
	     created ? "created" : "existing",
	     added ? "added to" : "already in");
	g_api.open_source_properties(source);
	g_api.source_release(source);
}

bool airplay_frontend_controller_register(void)
{
	if (g_registered)
		return true;
	if (!resolve_frontend_api()) {
		blog(LOG_WARNING,
		     "[AirPlay Live] OBS frontend API unavailable; Tools menu controller was not registered");
		return false;
	}

	g_api.add_tools_item(obs_module_text("Controller.Menu"), open_controller,
			     NULL);
	g_registered = true;
	blog(LOG_INFO,
	     "[AirPlay Live] Tools menu controller registered successfully");
	return true;
}

#else

bool airplay_frontend_controller_register(void)
{
	return false;
}

#endif
