/* Building FSR 2's API and Vulkan backend (third_party/FidelityFX-FSR2, C++) with g++ on Linux:
   force-included (-include) so that its few Windows-only library calls have stand-ins and its
   own sources stay as AMD ships them. */
#pragma once
#include <string.h>
#include <wchar.h>
#ifdef __cplusplus
#include <locale>                       /* std::wstring_convert (the backend includes only <codecvt>) */
#endif

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef _WIN32                          /* Windows (MinGW) has the array form of wcscpy_s */
#define wcscpy_s(dst, src) wcscpy((dst), (src))
#endif
