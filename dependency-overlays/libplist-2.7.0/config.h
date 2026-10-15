#pragma once

#define PACKAGE_VERSION "2.7.0"
#define PACKAGE_NAME "libplist"
#define PACKAGE_STRING "libplist 2.7.0"
#define HAVE_STRDUP 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1

#ifdef _MSC_VER
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strdup _strdup
#ifndef ssize_t
#include <basetsd.h>
typedef SSIZE_T ssize_t;
#endif
#endif
