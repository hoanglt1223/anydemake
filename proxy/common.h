#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "demake-config.h"

extern demake_config cfg;
extern HMODULE real_dll;
extern CRITICAL_SECTION hook_lock;
extern volatile LONG stat_textures, stat_samplers, stat_targets;
extern volatile LONG64 stat_texels;
extern __declspec(thread) int in_present;
/* Set only while PotatoMode itself draws; hooks pass those calls straight through.
   Overlays (Steam, RTSS, Discord) draw inside Present too and must still be tracked. */
extern __declspec(thread) int in_post;
extern const char post_hlsl[];
extern const size_t post_hlsl_len;

/* Defined by each proxy: d3d11.dll or d3d12.dll. */
extern const wchar_t potatomode_dll_name[];

/* Loads the config and System32\<potatomode_dll_name> once; safe to call from every export. */
void proxy_init(void);
FARPROC real_proc(const char *name);
void log_line(const char *fmt, ...);

/* Vtables are shared by every object of a class, and different classes (for
   example flip and blit swapchains) have different originals, so originals
   are kept per patched slot. */
void patch_slot(void **slot, void *hook);
/* The original behind a hooked slot. Falls back to the first original seen for
   that hook when another tool gave the object a copied vtable that still points
   at the hook, and to the slot's current value when it was never patched. */
void *orig_call(void **slot, void *hook);
#define ORIG(obj, method) orig_call((void **)&(obj)->lpVtbl->method, (void *)hk_##method)

/* D3D11_FILTER and D3D12_FILTER share their encoding. */
int demake_filter(int filter, UINT *max_anisotropy);
int post_enabled(UINT present_flags);
/* With logging on: fps, frame times and VRAM use every 5 s, so runs with
   and without PotatoMode can be compared. adapter is an IDXGIAdapter3. */
void stats_adapter(void *dxgi_adapter);
void stats_frame(void);
HWND hidden_window(void);
