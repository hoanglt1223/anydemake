#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demake-config.h"

#pragma section(".anydmk", read)
__declspec(allocate(".anydmk")) const volatile demake_config g_baked = {
    DEMAKE_CONFIG_MAGIC, sizeof(demake_config),
    0, 0, 64, DEMAKE_SAMPLER_KEEP, 0, 0, 0,
};

static demake_config cfg;
static HMODULE real_d3d11;
static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION lock;
static HANDLE log_file = INVALID_HANDLE_VALUE;
static LONG textures_shrunk, samplers_changed, logged_lines;
static LONG64 texels_saved;

static void log_line(const char *fmt, ...)
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

static void open_log(void)
{
    HMODULE self;
    wchar_t path[MAX_PATH];
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&open_log, &self);
    DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!len || len >= MAX_PATH || !slash || slash - path + 16 >= MAX_PATH)
        return;
    wcscpy(slash + 1, L"anydemake.log");
    log_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static BOOL CALLBACK init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once, (void)param, (void)ctx;
    memcpy(&cfg, (const void *)&g_baked, sizeof cfg);
    InitializeCriticalSection(&lock);
    if (cfg.log)
        open_log();
    log_line("anydemake: mip_skip=%u (default usage %u, min %u) sampler=%u pixelate=%u color_bits=%u",
             cfg.mip_skip, cfg.mip_skip_default, cfg.min_texture_size, cfg.sampler_mode, cfg.pixelate,
             cfg.color_bits);

    wchar_t path[MAX_PATH];
    UINT len = GetSystemDirectoryW(path, MAX_PATH);
    if (len && len + 11 < MAX_PATH) {
        wcscat(path, L"\\d3d11.dll");
        real_d3d11 = LoadLibraryW(path);
    }
    if (!real_d3d11)
        log_line("anydemake: cannot load the system d3d11.dll");
    return TRUE;
}

static void ensure_init(void)
{
    InitOnceExecuteOnce(&init_once, init, NULL, NULL);
}

/* Vtables are shared by every object of a class, and different classes (for
   example flip and blit swapchains) have different originals, so originals
   are kept per patched slot. */
typedef struct patched_slot {
    void **slot;
    void *orig;
} patched_slot;

static patched_slot patched[32];
static int patched_count;

static void *orig_for(void **slot)
{
    for (int i = 0; i < patched_count; i++)
        if (patched[i].slot == slot)
            return patched[i].orig;
    return NULL;
}

static void patch_slot(void **slot, void *hook)
{
    EnterCriticalSection(&lock);
    if (*slot != hook && !orig_for(slot) && patched_count < (int)(sizeof patched / sizeof patched[0])) {
        DWORD old;
        if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) {
            patched[patched_count].slot = slot;
            patched[patched_count].orig = *slot;
            patched_count++;
            *slot = hook;
            VirtualProtect(slot, sizeof(void *), old, &old);
        }
    }
    LeaveCriticalSection(&lock);
}

#define ORIG(obj, method) orig_for((void **)&(obj)->lpVtbl->method)

/* ---- textures ---------------------------------------------------------- */

static int is_block_compressed(DXGI_FORMAT f)
{
    return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) ||
           (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB);
}

static UINT full_mip_count(UINT w, UINT h)
{
    UINT m = 1, d = w > h ? w : h;
    while (d > 1) {
        d >>= 1;
        m++;
    }
    return m;
}

typedef HRESULT(STDMETHODCALLTYPE *create_texture2d_fn)(ID3D11Device *, const D3D11_TEXTURE2D_DESC *,
                                                        const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);

static UINT texture_skip(const D3D11_TEXTURE2D_DESC *d, const D3D11_SUBRESOURCE_DATA *init, UINT mips)
{
    if (!cfg.mip_skip || !init || mips < 2 || d->BindFlags != D3D11_BIND_SHADER_RESOURCE || d->CPUAccessFlags ||
        d->SampleDesc.Count != 1 || (d->MiscFlags & ~D3D11_RESOURCE_MISC_TEXTURECUBE))
        return 0;
    if (d->Usage != D3D11_USAGE_IMMUTABLE && !(d->Usage == D3D11_USAGE_DEFAULT && cfg.mip_skip_default))
        return 0;
    UINT skip = cfg.mip_skip < mips - 1 ? cfg.mip_skip : mips - 1;
    int bc = is_block_compressed(d->Format);
    for (; skip; skip--) {
        UINT w = d->Width >> skip, h = d->Height >> skip;
        if (w < cfg.min_texture_size || h < cfg.min_texture_size)
            continue;
        if (bc && (w % 4 || h % 4))
            continue;
        break;
    }
    return skip;
}

static HRESULT STDMETHODCALLTYPE hk_CreateTexture2D(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *desc,
                                                    const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture2D **out)
{
    create_texture2d_fn orig = (create_texture2d_fn)ORIG(dev, CreateTexture2D);
    if (!desc)
        return orig(dev, desc, init, out);
    UINT mips = desc->MipLevels ? desc->MipLevels : full_mip_count(desc->Width, desc->Height);
    UINT skip = texture_skip(desc, init, mips);
    if (!skip)
        return orig(dev, desc, init, out);

    D3D11_TEXTURE2D_DESC d = *desc;
    d.Width >>= skip;
    d.Height >>= skip;
    d.MipLevels = mips - skip;
    D3D11_SUBRESOURCE_DATA *sub = malloc(sizeof *sub * d.MipLevels * d.ArraySize);
    if (!sub)
        return orig(dev, desc, init, out);
    for (UINT a = 0; a < d.ArraySize; a++)
        for (UINT m = 0; m < d.MipLevels; m++)
            sub[a * d.MipLevels + m] = init[a * mips + m + skip];
    HRESULT hr = orig(dev, &d, sub, out);
    free(sub);
    if (FAILED(hr))
        return orig(dev, desc, init, out);

    LONG n = InterlockedIncrement(&textures_shrunk);
    InterlockedAdd64(&texels_saved,
                     ((LONG64)desc->Width * desc->Height - (LONG64)d.Width * d.Height) * desc->ArraySize);
    if (n <= 20 || (n % 500) == 0)
        log_line("texture #%ld: %ux%u -> %ux%u (format %d)", n, desc->Width, desc->Height, d.Width, d.Height,
                 desc->Format);
    return hr;
}

/* ---- samplers ---------------------------------------------------------- */

typedef HRESULT(STDMETHODCALLTYPE *create_sampler_fn)(ID3D11Device *, const D3D11_SAMPLER_DESC *,
                                                      ID3D11SamplerState **);

static HRESULT STDMETHODCALLTYPE hk_CreateSamplerState(ID3D11Device *dev, const D3D11_SAMPLER_DESC *desc,
                                                       ID3D11SamplerState **out)
{
    create_sampler_fn orig = (create_sampler_fn)ORIG(dev, CreateSamplerState);
    if (!desc || cfg.sampler_mode == DEMAKE_SAMPLER_KEEP || D3D11_DECODE_FILTER_REDUCTION(desc->Filter) > 1)
        return orig(dev, desc, out);
    D3D11_SAMPLER_DESC d = *desc;
    int cmp = D3D11_DECODE_IS_COMPARISON_FILTER(desc->Filter);
    if (cfg.sampler_mode == DEMAKE_SAMPLER_POINT)
        d.Filter = cmp ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_POINT;
    else
        d.Filter = cmp ? D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT : D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    d.MaxAnisotropy = 1;
    InterlockedIncrement(&samplers_changed);
    return orig(dev, &d, out);
}

/* ---- present-time post-process ----------------------------------------- */

static const char post_hlsl[] =
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

typedef struct post_state {
    ID3D11Device *dev; /* not owned; identity only */
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Buffer *cb;
    ID3DDeviceContextState *ctx_state;
    ID3D11Texture2D *copy;
    D3D11_TEXTURE2D_DESC copy_desc;
    int broken;
} post_state;

static post_state post;

static void post_release(void)
{
    if (post.vs) ID3D11VertexShader_Release(post.vs);
    if (post.ps) ID3D11PixelShader_Release(post.ps);
    if (post.cb) ID3D11Buffer_Release(post.cb);
    if (post.ctx_state) ID3DDeviceContextState_Release(post.ctx_state);
    if (post.copy) ID3D11Texture2D_Release(post.copy);
    memset(&post, 0, sizeof post);
}

static HRESULT compile(pD3DCompile fn, const char *entry, const char *target, ID3DBlob **blob)
{
    ID3DBlob *err = NULL;
    HRESULT hr = fn(post_hlsl, sizeof post_hlsl - 1, "anydemake", NULL, NULL, entry, target, 0, 0, blob, &err);
    if (err) {
        log_line("shader %s: %.300s", entry, (const char *)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    }
    return hr;
}

static int post_setup(ID3D11Device *dev)
{
    if (post.dev == dev)
        return !post.broken;
    post_release();
    post.dev = dev;
    post.broken = 1;

    D3D_FEATURE_LEVEL fl = ID3D11Device_GetFeatureLevel(dev);
    ID3D11Device1 *dev1 = NULL;
    if (fl < D3D_FEATURE_LEVEL_10_0 || FAILED(ID3D11Device_QueryInterface(dev, &IID_ID3D11Device1, (void **)&dev1))) {
        log_line("post-process off: needs feature level 10_0 and D3D11.1");
        return 0;
    }
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile fn = dc ? (pD3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    ID3DBlob *vsb = NULL, *psb = NULL;
    int ok = fn && SUCCEEDED(compile(fn, "vs", "vs_4_0", &vsb)) && SUCCEEDED(compile(fn, "ps", "ps_4_0", &psb)) &&
             SUCCEEDED(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb),
                                                       ID3D10Blob_GetBufferSize(vsb), NULL, &post.vs)) &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb),
                                                      ID3D10Blob_GetBufferSize(psb), NULL, &post.ps));
    if (vsb) ID3D10Blob_Release(vsb);
    if (psb) ID3D10Blob_Release(psb);

    D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    UINT flags = (ID3D11Device_GetCreationFlags(dev) & D3D11_CREATE_DEVICE_SINGLETHREADED)
                     ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                     : 0;
    ok = ok && SUCCEEDED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &post.cb)) &&
         SUCCEEDED(ID3D11Device1_CreateDeviceContextState(dev1, flags, &fl, 1, D3D11_SDK_VERSION,
                                                          &IID_ID3D11Device, NULL, &post.ctx_state));
    ID3D11Device1_Release(dev1);
    if (!ok) {
        log_line("post-process off: setup failed");
        return 0;
    }
    post.broken = 0;
    return 1;
}

static void post_process(IDXGISwapChain *sc)
{
    ID3D11Texture2D *bb = NULL;
    ID3D11Device *dev = NULL;
    if (FAILED(IDXGISwapChain_GetDevice(sc, &IID_ID3D11Device, (void **)&dev)))
        return; /* D3D12 swapchain */
    if (FAILED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb)))
        goto out;
    EnterCriticalSection(&lock);
    if (!post_setup(dev))
        goto unlock;

    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(bb, &d);
    if (d.SampleDesc.Count != 1 || !(d.BindFlags & D3D11_BIND_RENDER_TARGET))
        goto unlock;
    if (!post.copy || post.copy_desc.Width != d.Width || post.copy_desc.Height != d.Height ||
        post.copy_desc.Format != d.Format) {
        if (post.copy) ID3D11Texture2D_Release(post.copy);
        post.copy = NULL;
        D3D11_TEXTURE2D_DESC cd = {d.Width, d.Height, 1, 1, d.Format, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_SHADER_RESOURCE, 0, 0};
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &cd, NULL, &post.copy)))
            goto unlock;
        post.copy_desc = cd;
    }

    ID3D11ShaderResourceView *srv = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DeviceContext *ctx = NULL;
    ID3D11DeviceContext1 *ctx1 = NULL;
    ID3DDeviceContextState *prev = NULL;
    ID3D11Device_GetImmediateContext(dev, &ctx);
    if (SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)post.copy, NULL, &srv)) &&
        SUCCEEDED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv)) &&
        SUCCEEDED(ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext1, (void **)&ctx1))) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)post.copy, (ID3D11Resource *)bb);
        ID3D11DeviceContext1_SwapDeviceContextState(ctx1, post.ctx_state, &prev);

        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)post.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            float *c = map.pData;
            c[0] = (float)d.Width;
            c[1] = (float)d.Height;
            c[2] = (float)(cfg.pixelate > 1 ? cfg.pixelate : 1);
            c[3] = cfg.color_bits ? (float)((1u << (cfg.color_bits > 8 ? 8 : cfg.color_bits)) - 1) : 0.0f;
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)post.cb, 0);
        }
        D3D11_VIEWPORT vp = {0, 0, (float)d.Width, (float)d.Height, 0, 1};
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(ctx, post.vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, post.ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &post.cb);
        ID3D11DeviceContext_Draw(ctx, 3, 0);

        ID3D11ShaderResourceView *null_srv = NULL;
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &null_srv);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
        ID3D11DeviceContext1_SwapDeviceContextState(ctx1, prev, NULL);
    }
    if (prev) ID3DDeviceContextState_Release(prev);
    if (ctx1) ID3D11DeviceContext1_Release(ctx1);
    if (ctx) ID3D11DeviceContext_Release(ctx);
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (srv) ID3D11ShaderResourceView_Release(srv);
unlock:
    LeaveCriticalSection(&lock);
out:
    if (bb) ID3D11Texture2D_Release(bb);
    ID3D11Device_Release(dev);
}

static int post_enabled(UINT flags)
{
    return (cfg.pixelate > 1 || cfg.color_bits) && !(flags & DXGI_PRESENT_TEST);
}

typedef HRESULT(STDMETHODCALLTYPE *present_fn)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *present1_fn)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);

/* Some runtimes implement Present on top of Present1; this keeps the effect
   from running twice per frame. */
static __declspec(thread) int in_present;

static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain *sc, UINT sync, UINT flags)
{
    present_fn orig = (present_fn)ORIG(sc, Present);
    if (!in_present && post_enabled(flags))
        post_process(sc);
    in_present++;
    HRESULT hr = orig(sc, sync, flags);
    in_present--;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1 *sc, UINT sync, UINT flags,
                                             const DXGI_PRESENT_PARAMETERS *params)
{
    present1_fn orig = (present1_fn)ORIG(sc, Present1);
    if (!in_present && post_enabled(flags))
        post_process((IDXGISwapChain *)sc);
    in_present++;
    HRESULT hr = orig(sc, sync, flags, params);
    in_present--;
    return hr;
}

static void hook_swapchain(IDXGISwapChain *sc)
{
    if (!(cfg.pixelate > 1 || cfg.color_bits))
        return;
    patch_slot((void **)&sc->lpVtbl->Present, (void *)hk_Present);
    IDXGISwapChain1 *sc1;
    if (SUCCEEDED(IDXGISwapChain_QueryInterface(sc, &IID_IDXGISwapChain1, (void **)&sc1))) {
        patch_slot((void **)&sc1->lpVtbl->Present1, (void *)hk_Present1);
        IDXGISwapChain1_Release(sc1);
    }
}

/* Most games create their swapchain through IDXGIFactory, not through this
   DLL, so the Present slot is found on a throwaway swapchain instead. */
static void hook_swapchains_of(ID3D11Device *dev)
{
    if (!(cfg.pixelate > 1 || cfg.color_bits))
        return;
    IDXGIDevice *dxgi_dev = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory *factory = NULL;
    IDXGISwapChain *sc = NULL;
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"anydemake-probe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
    if (hwnd && SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi_dev)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_dev, &adapter)) &&
        SUCCEEDED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory))) {
        /* Flip and blit swapchains may be different classes with separate vtables. */
        static const DXGI_SWAP_EFFECT effects[] = {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_DISCARD};
        for (int i = 0; i < 2; i++) {
            DXGI_SWAP_CHAIN_DESC sd = {0};
            sd.BufferDesc.Width = 64;
            sd.BufferDesc.Height = 64;
            sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = i == 0 ? 2 : 1;
            sd.OutputWindow = hwnd;
            sd.Windowed = TRUE;
            sd.SwapEffect = effects[i];
            if (SUCCEEDED(IDXGIFactory_CreateSwapChain(factory, (IUnknown *)dev, &sd, &sc))) {
                hook_swapchain(sc);
                IDXGISwapChain_Release(sc);
                sc = NULL;
            } else {
                log_line("cannot create probe swapchain %d", i);
            }
        }
    }
    if (factory) IDXGIFactory_Release(factory);
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxgi_dev) IDXGIDevice_Release(dxgi_dev);
    if (hwnd) DestroyWindow(hwnd);
}

static void hook_device(ID3D11Device *dev)
{
    if (!dev)
        return;
    if (cfg.mip_skip)
        patch_slot((void **)&dev->lpVtbl->CreateTexture2D, (void *)hk_CreateTexture2D);
    if (cfg.sampler_mode != DEMAKE_SAMPLER_KEEP)
        patch_slot((void **)&dev->lpVtbl->CreateSamplerState, (void *)hk_CreateSamplerState);
    hook_swapchains_of(dev);
}

/* ---- exports ----------------------------------------------------------- */

typedef HRESULT(WINAPI *create_device_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
                                          UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *create_device_sc_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                             const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *,
                                             IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
                                             ID3D11DeviceContext **);
typedef HRESULT(WINAPI *on12_fn)(IUnknown *, UINT, const D3D_FEATURE_LEVEL *, UINT, IUnknown *const *, UINT, UINT,
                                 ID3D11Device **, ID3D11DeviceContext **, D3D_FEATURE_LEVEL *);

static FARPROC real(const char *name)
{
    ensure_init();
    return real_d3d11 ? GetProcAddress(real_d3d11, name) : NULL;
}

HRESULT WINAPI proxy_D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                       const D3D_FEATURE_LEVEL *levels, UINT nlevels, UINT sdk,
                                       ID3D11Device **dev, D3D_FEATURE_LEVEL *fl, ID3D11DeviceContext **ctx)
{
    create_device_fn fn = (create_device_fn)(void *)real("D3D11CreateDevice");
    if (!fn)
        return E_FAIL;
    HRESULT hr = fn(adapter, type, sw, flags, levels, nlevels, sdk, dev, fl, ctx);
    if (SUCCEEDED(hr) && dev)
        hook_device(*dev);
    return hr;
}

HRESULT WINAPI proxy_D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE sw,
                                                   UINT flags, const D3D_FEATURE_LEVEL *levels, UINT nlevels,
                                                   UINT sdk, const DXGI_SWAP_CHAIN_DESC *sd, IDXGISwapChain **sc,
                                                   ID3D11Device **dev, D3D_FEATURE_LEVEL *fl,
                                                   ID3D11DeviceContext **ctx)
{
    create_device_sc_fn fn = (create_device_sc_fn)(void *)real("D3D11CreateDeviceAndSwapChain");
    if (!fn)
        return E_FAIL;
    HRESULT hr = fn(adapter, type, sw, flags, levels, nlevels, sdk, sd, sc, dev, fl, ctx);
    if (SUCCEEDED(hr)) {
        if (dev)
            hook_device(*dev);
        if (sc && *sc && (cfg.pixelate > 1 || cfg.color_bits))
            hook_swapchain(*sc);
    }
    return hr;
}

HRESULT WINAPI proxy_D3D11On12CreateDevice(IUnknown *d12, UINT flags, const D3D_FEATURE_LEVEL *levels,
                                           UINT nlevels, IUnknown *const *queues, UINT nqueues, UINT node,
                                           ID3D11Device **dev, ID3D11DeviceContext **ctx, D3D_FEATURE_LEVEL *fl)
{
    on12_fn fn = (on12_fn)(void *)real("D3D11On12CreateDevice");
    if (!fn)
        return E_FAIL;
    HRESULT hr = fn(d12, flags, levels, nlevels, queues, nqueues, node, dev, ctx, fl);
    if (SUCCEEDED(hr) && dev && cfg.sampler_mode != DEMAKE_SAMPLER_KEEP)
        patch_slot((void **)&(*dev)->lpVtbl->CreateSamplerState, (void *)hk_CreateSamplerState);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst;
    /* Plain WriteFile is safe during process termination, unlike the CRT. */
    if (reason == DLL_PROCESS_DETACH && log_file != INVALID_HANDLE_VALUE) {
        log_line("summary: %ld textures shrunk, %lld texels saved, %ld samplers changed", textures_shrunk,
                 texels_saved, samplers_changed);
        if (!reserved)
            CloseHandle(log_file);
    }
    return TRUE;
}
