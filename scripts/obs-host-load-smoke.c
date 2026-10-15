#define _WIN32_WINNT 0x0602
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>

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

int wmain(int argc, wchar_t **argv)
{
	if (argc != 4) {
		fwprintf(stderr,
			 L"usage: obs-host-load-smoke.exe <obs-bin> <plugin-dir> <plugin-dll>\n");
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
		if (obs_cookie)
			RemoveDllDirectory(obs_cookie);
		if (plugin_cookie)
			RemoveDllDirectory(plugin_cookie);
		return 4;
	}

	HMODULE plugin = LoadLibraryExW(
		argv[3], NULL,
		LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
	if (!plugin) {
		print_last_error(L"LoadLibraryExW(plugin)");
		RemoveDllDirectory(plugin_cookie);
		RemoveDllDirectory(obs_cookie);
		return 5;
	}

	FreeLibrary(plugin);
	RemoveDllDirectory(plugin_cookie);
	RemoveDllDirectory(obs_cookie);
	wprintf(L"Plugin dependency load succeeded against the pinned OBS host.\n");
	return 0;
}
