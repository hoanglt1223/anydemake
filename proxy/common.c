#define CINTERFACE
#define COBJMACROS
#include "common.h"

#include <dxgi1_4.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#pragma section(".anydmk", read)
__declspec(allocate(".anydmk")) const volatile demake_config g_baked = {
    DEMAKE_CONFIG_MAGIC, sizeof(demake_config), 0, 0, 64, DEMAKE_SAMPLER_KEEP, 0, 0, 0, 100,
};

demake_config cfg;
HMODULE real_dll;
CRITICAL_SECTION hook_lock;
volatile LONG stat_textures, stat_samplers, stat_targets;
volatile LONG64 stat_texels;
/* Some runtimes implement Present on top of Present1; this keeps the effect
   from running twice per frame. */
__declspec(thread) int in_present;
__declspec(thread) int in_post;

extern const char *const anydemake_forward_names[];
extern void *anydemake_forward_table[];
extern const int anydemake_forward_count;

static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
static HANDLE log_file = INVALID_HANDLE_VALUE;

const char post_hlsl[] =
    "cbuffer C : register(b0) { float2 size; float block; float levels; };\n"
    "Texture2D src : register(t0);\n"
    "void vs(uint id : SV_VertexID, out float4 pos : SV_Position) {\n"
    "  float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 p = floor(pos.xy / block) * block + block * 0.5;\n"
    "  float4 c = src.Load(int3(min(p, size - 0.5), 0));\n"
    "  if (levels > 0) c.rgb = floor(c.rgb * levels + 0.5) / levels;\n"
    "  return c;\n"
    "}\n";
const size_t post_hlsl_len = sizeof post_hlsl - 1;

void log_line(const char *fmt, ...)
{
    if (log_file == INVALID_HANDLE_VALUE)
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof buf - 3)
        n = (int)sizeof buf - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    DWORD written;
    WriteFile(log_file, buf, (DWORD)n, &written, NULL);
}

static void open_log(const wchar_t *name)
{
    HMODULE self;
    wchar_t path[MAX_PATH];
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&open_log, &self);
    DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!len || len >= MAX_PATH || !slash || slash - path + 32 >= MAX_PATH)
        return;
    /* anydemake-d3d11.log and anydemake-d3d12.log: both proxies may load in one game. */
    _snwprintf(slash + 1, 31, L"anydemake-%.5s.log", name);
    log_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static HRESULT WINAPI forward_missing(void)
{
    return E_NOTIMPL;
}

static BOOL CALLBACK init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once, (void)param, (void)ctx;
    const wchar_t *name = anydemake_dll_name;
    memcpy(&cfg, (const void *)&g_baked, sizeof cfg);
    InitializeCriticalSection(&hook_lock);
    if (cfg.log)
        open_log(name);
    log_line("anydemake %ls: mip_skip=%u (default usage %u, min %u) sampler=%u pixelate=%u color_bits=%u "
             "render_scale=%u%%",
             name, cfg.mip_skip, cfg.mip_skip_default, cfg.min_texture_size, cfg.sampler_mode, cfg.pixelate,
             cfg.color_bits, cfg.render_scale);

    wchar_t path[MAX_PATH];
    UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len && len + 2 + wcslen(name) < MAX_PATH) {
        wcscat(path, L"\\");
        wcscat(path, name);
        real_dll = LoadLibraryW(path);
    }
    if (!real_dll)
        log_line("cannot load the system %ls", name);
    for (int i = 0; i < anydemake_forward_count; i++) {
        FARPROC p = real_dll ? GetProcAddress(real_dll, anydemake_forward_names[i]) : NULL;
        anydemake_forward_table[i] = p ? (void *)p : (void *)forward_missing;
    }
    return TRUE;
}

void proxy_init(void)
{
    InitOnceExecuteOnce(&init_once, init, NULL, NULL);
}

FARPROC real_proc(const char *name)
{
    proxy_init();
    return real_dll ? GetProcAddress(real_dll, name) : NULL;
}

void *anydemake_resolve(int index)
{
    proxy_init();
    return anydemake_forward_table[index];
}

static struct {
    void **slot;
    void *orig;
    void *hook;
} patched[96];
static volatile LONG patched_count;

/* Readers run without the lock: entries are complete before the count grows. */
void *orig_call(void **slot, void *hook)
{
    LONG n = patched_count;
    void *fallback = NULL;
    for (LONG i = 0; i < n; i++) {
        if (patched[i].slot == slot)
            return patched[i].orig;
        if (!fallback && patched[i].hook == hook)
            fallback = patched[i].orig;
    }
    if (fallback)
        return fallback;
    return *slot != hook ? *slot : NULL;
}

void patch_slot(void **slot, void *hook)
{
    EnterCriticalSection(&hook_lock);
    int known = 0;
    for (LONG i = 0; i < patched_count; i++)
        known |= patched[i].slot == slot;
    if (*slot != hook && !known && patched_count < (LONG)(sizeof patched / sizeof patched[0])) {
        DWORD old;
        if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
            patched[patched_count].slot = slot;
            patched[patched_count].orig = *slot;
            patched[patched_count].hook = hook;
            InterlockedIncrement(&patched_count);
            *slot = hook;
            VirtualProtect(slot, sizeof(void *), old, &old);
        }
    }
    LeaveCriticalSection(&hook_lock);
}

int demake_filter(int filter, UINT *max_anisotropy)
{
    enum { POINT = 0, LINEAR_MIP_POINT = 0x14, COMPARISON = 0x80, REDUCTION_SHIFT = 7 };
    int reduction = (filter >> REDUCTION_SHIFT) & 3;
    if (cfg.sampler_mode == DEMAKE_SAMPLER_KEEP || reduction > 1) /* min/max reduction filters stay */
        return filter;
    InterlockedIncrement(&stat_samplers);
    *max_anisotropy = 1;
    return (reduction ? COMPARISON : 0) | (cfg.sampler_mode == DEMAKE_SAMPLER_POINT ? POINT : LINEAR_MIP_POINT);
}

int post_enabled(UINT present_flags)
{
    return (cfg.pixelate > 1 || cfg.color_bits) && !(present_flags & 0x1 /* DXGI_PRESENT_TEST */);
}

static IDXGIAdapter3 *adapter3;
static LARGE_INTEGER freq, last_report, last_frame;
static LONG frames;
static double worst_ms;

void stats_adapter(void *dxgi_adapter)
{
    IDXGIAdapter3 *a;
    if (!cfg.log || !dxgi_adapter || adapter3 ||
        FAILED(IUnknown_QueryInterface((IUnknown *)dxgi_adapter, &IID_IDXGIAdapter3, (void **)&a)))
        return;
    if (InterlockedCompareExchangePointer((void **)&adapter3, a, NULL))
        IDXGIAdapter3_Release(a);
}

void stats_frame(void)
{
    if (!cfg.log)
        return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        last_report = last_frame = now;
        return;
    }
    double ms = (double)(now.QuadPart - last_frame.QuadPart) * 1000.0 / freq.QuadPart;
    last_frame = now;
    if (ms > worst_ms)
        worst_ms = ms;
    frames++;
    double elapsed = (double)(now.QuadPart - last_report.QuadPart) / freq.QuadPart;
    if (elapsed < 5.0)
        return;
    DXGI_QUERY_VIDEO_MEMORY_INFO mem = {0};
    if (adapter3)
        IDXGIAdapter3_QueryVideoMemoryInfo(adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mem);
    log_line("fps %.1f, frame %.2f ms average, %.2f ms worst, VRAM %.0f MB", frames / elapsed,
             elapsed * 1000.0 / frames, worst_ms, mem.CurrentUsage / (1024.0 * 1024.0));
    frames = 0;
    worst_ms = 0;
    last_report = now;
}

HWND hidden_window(void)
{
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"anydemake-probe";
    RegisterClassW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst;
    /* Plain WriteFile is safe during process termination, unlike the CRT. */
    if (reason == DLL_PROCESS_DETACH && log_file != INVALID_HANDLE_VALUE) {
        log_line("summary: %ld textures shrunk (%lld texels saved), %ld render targets scaled, %ld samplers changed",
                 stat_textures, stat_texels, stat_targets, stat_samplers);
        if (!reserved)
            CloseHandle(log_file);
    }
    return TRUE;
}
