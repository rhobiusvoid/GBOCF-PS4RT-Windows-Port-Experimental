/* SPDX-License-Identifier: GPL-2.0-or-later
 * Windows 10 (1803+) placeholder-memory APIs for runtime_memory.c.
 *
 * The MinGW-w64 import libraries of several toolchains lack VirtualAlloc2, MapViewOfFile3 and
 * CreateFileMapping2 (only the *FromApp names are exported by their libmincore), and the
 * headers hide some of the prototypes. These wrappers resolve the functions from
 * kernelbase.dll on first use, so the executable also starts, and reports the problem
 * plainly, on a Windows that lacks them. Include after <windows.h>. */
#ifndef PS4RT_COMPAT_WIN_MEM_H
#define PS4RT_COMPAT_WIN_MEM_H
#ifdef _WIN32
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif
PVOID compat_VirtualAlloc2(HANDLE process, PVOID address, SIZE_T size, ULONG type, ULONG protect,
                           MEM_EXTENDED_PARAMETER *extended, ULONG extended_count);
PVOID compat_MapViewOfFile3(HANDLE section, HANDLE process, PVOID address, ULONG64 offset, SIZE_T size,
                            ULONG type, ULONG protect, MEM_EXTENDED_PARAMETER *extended, ULONG extended_count);
BOOL compat_UnmapViewOfFile2(HANDLE process, PVOID address, ULONG flags);
HANDLE compat_CreateFileMapping2(HANDLE file, SECURITY_ATTRIBUTES *security, ULONG access, ULONG protect,
                                 ULONG attributes, ULONG64 maximum_size, PCWSTR name,
                                 MEM_EXTENDED_PARAMETER *extended, ULONG extended_count);
#ifdef __cplusplus
}
#endif

#define VirtualAlloc2 compat_VirtualAlloc2
#define MapViewOfFile3 compat_MapViewOfFile3
#define UnmapViewOfFile2 compat_UnmapViewOfFile2
#define CreateFileMapping2 compat_CreateFileMapping2
#endif
#endif
