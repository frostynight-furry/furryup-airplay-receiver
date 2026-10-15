#define _WIN32_WINNT 0x0602
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AIRPLAY_RTSP_PORT 7000

typedef bool(__cdecl *obs_startup_fn)(const char *, const char *, void *);
typedef void(__cdecl *obs_shutdown_fn)(void);
typedef int(__cdecl *obs_open_module_fn)(void **, const char *, const char *);
typedef bool(__cdecl *obs_init_module_fn)(void *);
typedef void *(__cdecl *obs_source_create_private_fn)(const char *,
						       const char *, void *);
typedef void(__cdecl *obs_source_release_fn)(void *);
typedef void(__cdecl *obs_source_update_fn)(void *, void *);
typedef void *(__cdecl *obs_source_get_settings_fn)(void *);
typedef void *(__cdecl *obs_data_create_fn)(void);
typedef void(__cdecl *obs_data_release_fn)(void *);
typedef void(__cdecl *obs_data_set_bool_fn)(void *, const char *, bool);
typedef bool(__cdecl *obs_data_get_bool_fn)(void *, const char *);
typedef void(__cdecl *base_log_handler_fn)(int, const char *, va_list, void *);
typedef void(__cdecl *base_set_log_handler_fn)(base_log_handler_fn, void *);
typedef void(__cdecl *obs_add_data_path_fn)(const char *);

struct smoke_obs_video_info {
	const char *graphics_module;
	uint32_t fps_num;
	uint32_t fps_den;
	uint32_t base_width;
	uint32_t base_height;
	uint32_t output_width;
	uint32_t output_height;
	int output_format;
	uint32_t adapter;
	bool gpu_conversion;
	int colorspace;
	int range;
	int scale_type;
};

typedef int(__cdecl *obs_reset_video_fn)(struct smoke_obs_video_info *);

struct lifecycle_log_capture {
	volatile LONG session_disabled;
	volatile LONG start_attempts;
	volatile LONG explicitly_disabled;
};

static void lifecycle_log_handler(int level, const char *format, va_list args,
				  void *private_data)
{
	(void)level;
	struct lifecycle_log_capture *capture = private_data;
	char message[4096];
	vsnprintf(message, sizeof(message), format, args);
	message[sizeof(message) - 1] = '\0';
	if (strstr(message, "[AirPlay]"))
		fprintf(stderr, "%s\n", message);

	if (strstr(message, "created session-disabled"))
		InterlockedExchange(&capture->session_disabled, 1);
	if (strstr(message, "calling raop_start_httpd"))
		InterlockedIncrement(&capture->start_attempts);
	if (strstr(message, "FUS-managed receiver disabled"))
		InterlockedExchange(&capture->explicitly_disabled, 1);
}

static void print_last_error(const wchar_t *operation)
{
	DWORD error = GetLastError();
	wchar_t *message = NULL;
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
			       FORMAT_MESSAGE_FROM_SYSTEM |
			       FORMAT_MESSAGE_IGNORE_INSERTS,
		       NULL, error, 0, (wchar_t *)&message, 0, NULL);
	fwprintf(stderr, L"%ls failed (%lu): %ls\n", operation,
		 (unsigned long)error, message ? message : L"unknown error");
	if (message)
		LocalFree(message);
}

static char *wide_to_utf8(const wchar_t *input)
{
	int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1,
				     NULL, 0, NULL, NULL);
	if (length <= 0)
		return NULL;
	char *output = malloc((size_t)length);
	if (!output)
		return NULL;
	if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1,
				 output, length, NULL, NULL)) {
		free(output);
		return NULL;
	}
	return output;
}

static FARPROC require_proc(HMODULE module, const char *name)
{
	FARPROC proc = GetProcAddress(module, name);
	if (!proc)
		fprintf(stderr, "Missing OBS export: %s\n", name);
	return proc;
}

static bool tcp_port_accepting(unsigned short port)
{
	SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (socket_handle == INVALID_SOCKET)
		return false;

	u_long nonblocking = 1;
	if (ioctlsocket(socket_handle, FIONBIO, &nonblocking) != 0) {
		closesocket(socket_handle);
		return false;
	}

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	int connect_result = connect(socket_handle, (struct sockaddr *)&address,
				     sizeof(address));
	if (connect_result == 0) {
		closesocket(socket_handle);
		return true;
	}

	int error = WSAGetLastError();
	if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) {
		closesocket(socket_handle);
		return false;
	}

	fd_set writable;
	FD_ZERO(&writable);
	FD_SET(socket_handle, &writable);
	struct timeval timeout = {0, 100 * 1000};
	int select_result =
		select(0, NULL, &writable, NULL, &timeout);
	if (select_result <= 0) {
		closesocket(socket_handle);
		return false;
	}

	int socket_error = 0;
	int option_length = sizeof(socket_error);
	if (getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
		       (char *)&socket_error, &option_length) != 0) {
		closesocket(socket_handle);
		return false;
	}

	closesocket(socket_handle);
	return socket_error == 0;
}

static bool wait_for_port_state(unsigned short port, bool expected_open,
				unsigned long timeout_ms)
{
	unsigned long elapsed = 0;
	while (elapsed <= timeout_ms) {
		if (tcp_port_accepting(port) == expected_open)
			return true;
		Sleep(50);
		elapsed += 50;
	}
	return false;
}

static bool port_stays_closed(unsigned short port, unsigned long duration_ms)
{
	unsigned long elapsed = 0;
	while (elapsed <= duration_ms) {
		if (tcp_port_accepting(port))
			return false;
		Sleep(50);
		elapsed += 50;
	}
	return true;
}

static bool wait_for_counter(volatile LONG *counter, unsigned long timeout_ms)
{
	unsigned long elapsed = 0;
	while (elapsed <= timeout_ms) {
		if (InterlockedCompareExchange(counter, 0, 0) != 0)
			return true;
		Sleep(50);
		elapsed += 50;
	}
	return false;
}

int wmain(int argc, wchar_t **argv)
{
	if (argc != 6) {
		fwprintf(stderr,
			 L"usage: obs-managed-lifecycle-smoke.exe "
			 L"<obs-bin> <plugin-dir> <plugin-dll> <plugin-data> "
			 L"<obs-data>\n");
		return 2;
	}

	WSADATA winsock_data;
	if (WSAStartup(MAKEWORD(2, 2), &winsock_data) != 0) {
		fprintf(stderr, "WSAStartup failed\n");
		return 3;
	}
	bool port_was_busy = tcp_port_accepting(AIRPLAY_RTSP_PORT);
	if (port_was_busy)
		fprintf(stderr,
			"TCP port %d is already occupied; startup transitions will be verified from the isolated OBS log instead of port ownership\n",
			AIRPLAY_RTSP_PORT);

	if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 |
				      LOAD_LIBRARY_SEARCH_USER_DIRS)) {
		print_last_error(L"SetDefaultDllDirectories");
		WSACleanup();
		return 5;
	}

	DLL_DIRECTORY_COOKIE obs_cookie = AddDllDirectory(argv[1]);
	DLL_DIRECTORY_COOKIE plugin_cookie = AddDllDirectory(argv[2]);
	if (!obs_cookie || !plugin_cookie) {
		print_last_error(L"AddDllDirectory");
		if (plugin_cookie)
			RemoveDllDirectory(plugin_cookie);
		if (obs_cookie)
			RemoveDllDirectory(obs_cookie);
		WSACleanup();
		return 6;
	}

	HMODULE obs_module = LoadLibraryExW(
		L"obs.dll", NULL,
		LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
	if (!obs_module) {
		print_last_error(L"LoadLibraryExW(obs.dll)");
		RemoveDllDirectory(plugin_cookie);
		RemoveDllDirectory(obs_cookie);
		WSACleanup();
		return 7;
	}

#define LOAD_OBS_FN(type, variable, symbol) \
	type variable = (type)require_proc(obs_module, symbol)
	LOAD_OBS_FN(obs_startup_fn, obs_startup, "obs_startup");
	LOAD_OBS_FN(obs_shutdown_fn, obs_shutdown, "obs_shutdown");
	LOAD_OBS_FN(obs_open_module_fn, obs_open_module, "obs_open_module");
	LOAD_OBS_FN(obs_init_module_fn, obs_init_module, "obs_init_module");
	LOAD_OBS_FN(obs_source_create_private_fn, obs_source_create_private,
		    "obs_source_create_private");
	LOAD_OBS_FN(obs_source_release_fn, obs_source_release,
		    "obs_source_release");
	LOAD_OBS_FN(obs_source_update_fn, obs_source_update,
		    "obs_source_update");
	LOAD_OBS_FN(obs_source_get_settings_fn, obs_source_get_settings,
		    "obs_source_get_settings");
	LOAD_OBS_FN(obs_data_create_fn, obs_data_create, "obs_data_create");
	LOAD_OBS_FN(obs_data_release_fn, obs_data_release, "obs_data_release");
	LOAD_OBS_FN(obs_data_set_bool_fn, obs_data_set_bool,
		    "obs_data_set_bool");
	LOAD_OBS_FN(obs_data_get_bool_fn, obs_data_get_bool,
		    "obs_data_get_bool");
	LOAD_OBS_FN(base_set_log_handler_fn, base_set_log_handler,
		    "base_set_log_handler");
	LOAD_OBS_FN(obs_add_data_path_fn, obs_add_data_path,
		    "obs_add_data_path");
	LOAD_OBS_FN(obs_reset_video_fn, obs_reset_video, "obs_reset_video");
#undef LOAD_OBS_FN

	if (!obs_startup || !obs_shutdown || !obs_open_module ||
	    !obs_init_module || !obs_source_create_private ||
	    !obs_source_release || !obs_source_update ||
	    !obs_source_get_settings || !obs_data_create || !obs_data_release ||
	    !obs_data_set_bool || !obs_data_get_bool || !base_set_log_handler ||
	    !obs_add_data_path || !obs_reset_video) {
		FreeLibrary(obs_module);
		RemoveDllDirectory(plugin_cookie);
		RemoveDllDirectory(obs_cookie);
		WSACleanup();
		return 8;
	}

	char *plugin_path = wide_to_utf8(argv[3]);
	char *data_path = wide_to_utf8(argv[4]);
	char *obs_bin_path = wide_to_utf8(argv[1]);
	char *obs_data_path = wide_to_utf8(argv[5]);
	if (!plugin_path || !data_path || !obs_bin_path || !obs_data_path) {
		fprintf(stderr, "Unable to convert smoke-test paths to UTF-8\n");
		free(plugin_path);
		free(data_path);
		free(obs_bin_path);
		free(obs_data_path);
		FreeLibrary(obs_module);
		RemoveDllDirectory(plugin_cookie);
		RemoveDllDirectory(obs_cookie);
		WSACleanup();
		return 9;
	}

	int result = 0;
	bool video_tick_available = false;
	struct lifecycle_log_capture capture = {0};
	char obs_data_directory[4096];
	int obs_data_length = snprintf(obs_data_directory,
				       sizeof(obs_data_directory), "%s\\",
				       obs_data_path);
	if (obs_data_length < 0 ||
	    obs_data_length >= (int)sizeof(obs_data_directory)) {
		fprintf(stderr, "OBS data path is too long\n");
		free(plugin_path);
		free(data_path);
		free(obs_bin_path);
		free(obs_data_path);
		FreeLibrary(obs_module);
		RemoveDllDirectory(plugin_cookie);
		RemoveDllDirectory(obs_cookie);
		WSACleanup();
		return 10;
	}
	obs_add_data_path(obs_data_directory);
	base_set_log_handler(lifecycle_log_handler, &capture);
	bool started = obs_startup("en-US", NULL, NULL);
	if (!started) {
		fprintf(stderr, "obs_startup failed\n");
		result = 10;
	} else {
		char graphics_module[4096];
		int graphics_length = snprintf(graphics_module,
					       sizeof(graphics_module),
					       "%s\\libobs-d3d11.dll",
					       obs_bin_path);
		if (graphics_length < 0 ||
		    graphics_length >= (int)sizeof(graphics_module)) {
			fprintf(stderr, "OBS smoke-test path is too long\n");
			result = 11;
		} else {
			struct smoke_obs_video_info video_info = {0};
			video_info.graphics_module = graphics_module;
			video_info.fps_num = 30;
			video_info.fps_den = 1;
			video_info.base_width = 64;
			video_info.base_height = 64;
			video_info.output_width = 64;
			video_info.output_height = 64;
			video_info.output_format = 7; /* VIDEO_FORMAT_BGRA */
			int video_result = obs_reset_video(&video_info);
			if (video_result != 0) {
				fprintf(stderr,
					"obs_reset_video failed (%d); deferred runtime-update checks will be skipped\n",
					video_result);
			} else
				video_tick_available = true;
		}

		void *plugin_module = NULL;
		int open_result = result == 0
				  ? obs_open_module(&plugin_module, plugin_path,
						    data_path)
				  : -1;
		if (result != 0) {
			/* Preserve the earlier video/path error. */
		} else if (open_result != 0) {
			fprintf(stderr, "obs_open_module failed (%d)\n", open_result);
			result = 13;
		} else if (!obs_init_module(plugin_module)) {
			fprintf(stderr, "obs_init_module returned false\n");
			result = 14;
		} else {
			void *settings = obs_data_create();
			obs_data_set_bool(settings, "managed_by_fus", true);
			obs_data_set_bool(settings, "receiver_enabled", true);
			void *source = obs_source_create_private(
				"airplay_live_source", "FUS managed smoke",
				settings);
			obs_data_release(settings);
			if (!source) {
				fprintf(stderr,
					"FUS-managed source creation failed\n");
				result = 15;
			} else {
				void *live_settings =
					obs_source_get_settings(source);
				bool persisted_enable = obs_data_get_bool(
					live_settings, "receiver_enabled");
				obs_data_release(live_settings);
				if (persisted_enable) {
					fprintf(stderr,
						"Persisted receiver_enabled=true was not reset during source creation\n");
					result = 16;
				} else {
					Sleep(750);
					if (!InterlockedCompareExchange(
						    &capture.session_disabled, 0, 0)) {
						fprintf(stderr,
							"Managed create did not report its session-disabled state\n");
						result = 15;
					} else if (InterlockedCompareExchange(
							   &capture.start_attempts, 0,
							   0) != 0) {
						fprintf(stderr,
							"Managed source attempted receiver startup before an in-session enable update\n");
						result = 16;
					} else if (!port_was_busy &&
						   !port_stays_closed(
							   AIRPLAY_RTSP_PORT, 250)) {
						fprintf(stderr,
							"Managed source bound port %d before an in-session enable update\n",
							AIRPLAY_RTSP_PORT);
						result = 17;
					}
				}
				if (result == 0 && video_tick_available) {
					void *enable = obs_data_create();
					obs_data_set_bool(enable,
							  "managed_by_fus", true);
					obs_data_set_bool(enable,
							  "receiver_enabled", true);
					obs_source_update(source, enable);
					obs_data_release(enable);
					if (!wait_for_counter(&capture.start_attempts,
							      5000)) {
						fprintf(stderr,
							"In-session enable update did not enter the receiver startup path\n");
						result = 18;
					} else if (!port_was_busy &&
						   !wait_for_port_state(
							   AIRPLAY_RTSP_PORT, true,
							   5000)) {
						fprintf(stderr,
							"In-session enable update did not start the receiver on port %d\n",
							AIRPLAY_RTSP_PORT);
						result = 19;
					} else {
						void *disable = obs_data_create();
						obs_data_set_bool(
							disable, "managed_by_fus",
							true);
						obs_data_set_bool(
							disable, "receiver_enabled",
							false);
						obs_source_update(source, disable);
						obs_data_release(disable);
						if (!wait_for_counter(
							    &capture.explicitly_disabled,
							    5000)) {
							fprintf(stderr,
								"Disable update did not enter the managed stop path\n");
							result = 20;
						} else if (!port_was_busy &&
							   !wait_for_port_state(
								   AIRPLAY_RTSP_PORT,
								   false, 5000)) {
							fprintf(stderr,
								"Disable update did not release port %d\n",
								AIRPLAY_RTSP_PORT);
							result = 21;
						}
					}
				}
				obs_source_release(source);
			}
		}
		obs_shutdown();
	}

	free(plugin_path);
	free(data_path);
	free(obs_bin_path);
	free(obs_data_path);
	FreeLibrary(obs_module);
	RemoveDllDirectory(plugin_cookie);
	RemoveDllDirectory(obs_cookie);
	WSACleanup();
	if (result == 0 && video_tick_available)
		printf("FUS-managed create/enable/disable lifecycle succeeded.\n");
	else if (result == 0)
		printf("FUS-managed persisted-enable create/load gate succeeded.\n");
	return result;
}
