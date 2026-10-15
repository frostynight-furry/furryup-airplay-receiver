#define _WIN32_WINNT 0x0602
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef bool(__cdecl *obs_startup_fn)(const char *, const char *, void *);
typedef void(__cdecl *obs_shutdown_fn)(void);
typedef int(__cdecl *obs_open_module_fn)(void **, const char *, const char *);
typedef bool(__cdecl *obs_init_module_fn)(void *);
typedef bool(__cdecl *obs_enum_source_types_fn)(size_t, const char **);
typedef void *(__cdecl *obs_source_create_private_fn)(const char *,
						       const char *, void *);
typedef void(__cdecl *obs_source_release_fn)(void *);

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

int wmain(int argc, wchar_t **argv)
{
	if (argc != 5) {
		fwprintf(stderr,
			 L"usage: obs-module-registration-smoke.exe "
			 L"<obs-bin> <plugin-dir> <plugin-dll> <plugin-data>\n");
		return 2;
	}

	if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 |
				      LOAD_LIBRARY_SEARCH_USER_DIRS)) {
		print_last_error(L"SetDefaultDllDirectories");
		return 3;
	}

	DLL_DIRECTORY_COOKIE obs_cookie = AddDllDirectory(argv[1]);
	DLL_DIRECTORY_COOKIE plugin_cookie = AddDllDirectory(argv[2]);
	if (!obs_cookie || !plugin_cookie) {
		print_last_error(L"AddDllDirectory");
		if (plugin_cookie)
			RemoveDllDirectory(plugin_cookie);
		if (obs_cookie)
			RemoveDllDirectory(obs_cookie);
		return 4;
	}

	HMODULE obs_module = LoadLibraryExW(
		L"obs.dll", NULL,
		LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
	if (!obs_module) {
		print_last_error(L"LoadLibraryExW(obs.dll)");
		RemoveDllDirectory(plugin_cookie);
		RemoveDllDirectory(obs_cookie);
		return 5;
	}

	obs_startup_fn obs_startup =
		(obs_startup_fn)require_proc(obs_module, "obs_startup");
	obs_shutdown_fn obs_shutdown =
		(obs_shutdown_fn)require_proc(obs_module, "obs_shutdown");
	obs_open_module_fn obs_open_module =
		(obs_open_module_fn)require_proc(obs_module, "obs_open_module");
	obs_init_module_fn obs_init_module =
		(obs_init_module_fn)require_proc(obs_module, "obs_init_module");
	obs_enum_source_types_fn obs_enum_source_types =
		(obs_enum_source_types_fn)require_proc(obs_module,
						       "obs_enum_source_types");
	obs_source_create_private_fn obs_source_create_private =
		(obs_source_create_private_fn)require_proc(
			obs_module, "obs_source_create_private");
	obs_source_release_fn obs_source_release =
		(obs_source_release_fn)require_proc(obs_module,
						     "obs_source_release");
	if (!obs_startup || !obs_shutdown || !obs_open_module ||
	    !obs_init_module || !obs_enum_source_types ||
	    !obs_source_create_private || !obs_source_release) {
		FreeLibrary(obs_module);
		RemoveDllDirectory(plugin_cookie);
		RemoveDllDirectory(obs_cookie);
		return 6;
	}

	char *plugin_path = wide_to_utf8(argv[3]);
	char *data_path = wide_to_utf8(argv[4]);
	if (!plugin_path || !data_path) {
		fprintf(stderr, "Unable to convert smoke-test paths to UTF-8\n");
		free(plugin_path);
		free(data_path);
		FreeLibrary(obs_module);
		RemoveDllDirectory(plugin_cookie);
		RemoveDllDirectory(obs_cookie);
		return 7;
	}

	int result = 0;
	bool started = obs_startup("en-US", NULL, NULL);
	if (!started) {
		fprintf(stderr, "obs_startup failed\n");
		result = 8;
	} else {
		void *plugin_module = NULL;
		int open_result =
			obs_open_module(&plugin_module, plugin_path, data_path);
		if (open_result != 0) {
			fprintf(stderr, "obs_open_module failed (%d)\n", open_result);
			result = 9;
		} else if (!obs_init_module(plugin_module)) {
			fprintf(stderr, "obs_init_module returned false\n");
			result = 10;
		} else {
			bool found = false;
			for (size_t index = 0;; index++) {
				const char *id = NULL;
				if (!obs_enum_source_types(index, &id))
					break;
				if (id && strcmp(id, "airplay_live_source") == 0) {
					found = true;
					break;
				}
			}
			if (!found) {
				fprintf(stderr,
					"Plugin loaded but source type was not registered\n");
				result = 11;
			} else {
				void *source = obs_source_create_private(
					"airplay_live_source", "AirPlay smoke", NULL);
				if (!source) {
					fprintf(stderr,
						"AirPlay source lifecycle initialization failed\n");
					result = 12;
				} else {
					Sleep(500);
					obs_source_release(source);
				}
			}
		}
		obs_shutdown();
	}

	free(plugin_path);
	free(data_path);
	FreeLibrary(obs_module);
	RemoveDllDirectory(plugin_cookie);
	RemoveDllDirectory(obs_cookie);
	if (result == 0)
		printf("Plugin registration and AirPlay source lifecycle succeeded.\n");
	return result;
}
