// SPDX-License-Identifier: GPL-2.0-or-later
// Optional development logging bridge; implementation stays in the private toolkit.
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifndef PS4RT_DIAGNOSTICS
#define PS4RT_DIAGNOSTICS 0
#endif
enum Ps4RtLoadingKind {
    PS4RT_LOAD_THREAD_START, PS4RT_LOAD_THREAD_EXIT,
    PS4RT_LOAD_READ_BEGIN, PS4RT_LOAD_READ_END,
    PS4RT_LOAD_ULT_PUSH, PS4RT_LOAD_ULT_WAIT, PS4RT_LOAD_ULT_POP,
    PS4RT_LOAD_GFX_SUBMIT, PS4RT_LOAD_FLIP,
    PS4RT_LOAD_VO_WAIT, PS4RT_LOAD_VO_RESUME,
    PS4RT_LOAD_KIND_COUNT
};
#ifdef __cplusplus
extern "C" {
#endif
#if PS4RT_DIAGNOSTICS && !defined(_WIN32)
void ps4rt_loading_start(const char* user_dir) __attribute__((weak));
void ps4rt_loading_event(unsigned kind, uintptr_t object, int64_t detail,
                         uintptr_t caller) __attribute__((weak));
void ps4rt_loading_note(const char* format, ...)
    __attribute__((weak, format(printf, 1, 2)));
void ps4rt_loading_text(const void* text, size_t size) __attribute__((weak));
#define PS4RT_LOADING_EVENT(kind, object, detail) do { \
    if (ps4rt_loading_event) ps4rt_loading_event((kind), (uintptr_t)(object), \
        (int64_t)(detail), (uintptr_t)__builtin_return_address(0)); \
} while (0)
#define PS4RT_LOADING_NOTE(...) do { \
    if (ps4rt_loading_note) ps4rt_loading_note(__VA_ARGS__); \
} while (0)
#define PS4RT_LOADING_TEXT(text, size) do { \
    if (ps4rt_loading_text) ps4rt_loading_text((text), (size)); \
} while (0)
#else
#define PS4RT_LOADING_EVENT(kind, object, detail) ((void)0)
#define PS4RT_LOADING_NOTE(...) ((void)0)
#define PS4RT_LOADING_TEXT(text, size) ((void)0)
#endif
#ifdef __cplusplus
}
#endif
