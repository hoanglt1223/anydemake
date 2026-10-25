/* Loads a generated proxy d3d11.dll on WARP and checks what reaches the real
   runtime. The window it creates is never shown. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT(WINAPI *create_device_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
                                          UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            fprintf(stderr, "FAIL line %d: ", __LINE__);                                                              \
            fprintf(stderr, __VA_ARGS__);                                                                             \
            fprintf(stderr, "\n");                                                                                    \
            exit(1);                                                                                                  \
        }                                                                                                             \
    } while (0)

static UINT make_texture(D3D11_USAGE usage)
{
    enum { W = 256, MIPS = 9 };
    static unsigned char pixels[W * W * 4];
    D3D11_SUBRESOURCE_DATA init[MIPS];
    for (UINT m = 0; m < MIPS; m++) {
        init[m].pSysMem = pixels;
        init[m].SysMemPitch = (W >> m) * 4;
        init[m].SysMemSlicePitch = 0;
    }
    D3D11_TEXTURE2D_DESC d = {W, W, MIPS, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, usage, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D *t;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, init, &t)), "CreateTexture2D");
    ID3D11Texture2D_GetDesc(t, &d);
    ID3D11Texture2D_Release(t);
    return d.Width;
}

static void check_texture(UINT want_immutable, UINT want_default)
{
    UINT a = make_texture(D3D11_USAGE_IMMUTABLE), b = make_texture(D3D11_USAGE_DEFAULT);
    CHECK(a == want_immutable, "immutable width %u, want %u", a, want_immutable);
    CHECK(b == want_default, "default width %u, want %u", b, want_default);
    printf("texture ok: immutable %u, default %u\n", a, b);
}

static void check_sampler(int want_filter)
{
    D3D11_SAMPLER_DESC d = {D3D11_FILTER_ANISOTROPIC, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_WRAP,
                            D3D11_TEXTURE_ADDRESS_WRAP, 0, 16, D3D11_COMPARISON_NEVER, {0}, 0, D3D11_FLOAT32_MAX};
    ID3D11SamplerState *s;
    CHECK(SUCCEEDED(ID3D11Device_CreateSamplerState(dev, &d, &s)), "CreateSamplerState");
    ID3D11SamplerState_GetDesc(s, &d);
    ID3D11SamplerState_Release(s);
    CHECK((int)d.Filter == want_filter, "filter 0x%x, want 0x%x", d.Filter, want_filter);
    printf("sampler ok: filter 0x%x, anisotropy %u\n", d.Filter, d.MaxAnisotropy);
}

/* Fills the back buffer with noise, presents, and reads it back. A blit
   swapchain with one buffer keeps its contents across Present. */
static void check_present(UINT block, UINT bits)
{
    enum { W = 128, H = 96 };
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"potatomode-test";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);
    CHECK(hwnd, "window");

    IDXGIDevice *dxgi_dev;
    IDXGIAdapter *adapter;
    IDXGIFactory *factory;
    CHECK(SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi_dev)), "IDXGIDevice");
    CHECK(SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_dev, &adapter)), "adapter");
    CHECK(SUCCEEDED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory)), "factory");
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferDesc.Width = W;
    sd.BufferDesc.Height = H;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
    IDXGISwapChain *sc;
    CHECK(SUCCEEDED(IDXGIFactory_CreateSwapChain(factory, (IUnknown *)dev, &sd, &sc)), "CreateSwapChain");

    static unsigned char noise[W * H * 4], out[W * H * 4];
    for (int i = 0; i < W * H * 4; i++)
        noise[i] = (unsigned char)((i * 2654435761u) >> 13);
    ID3D11Texture2D *bb, *staging;
    CHECK(SUCCEEDED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb)), "GetBuffer");
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)bb, 0, NULL, noise, W * 4, 0);
    CHECK(SUCCEEDED(IDXGISwapChain_Present(sc, 0, 0)), "Present");

    D3D11_TEXTURE2D_DESC d = {W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0,
                              D3D11_CPU_ACCESS_READ, 0};
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &staging)), "staging");
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)bb);
    D3D11_MAPPED_SUBRESOURCE map;
    CHECK(SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map)), "Map");
    for (int y = 0; y < H; y++)
        memcpy(out + y * W * 4, (unsigned char *)map.pData + y * map.RowPitch, W * 4);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);

    if (block <= 1 && !bits) {
        CHECK(!memcmp(out, noise, sizeof out), "passthrough changed the frame");
    } else {
        UINT b = block > 1 ? block : 1;
        unsigned levels = bits ? (1u << bits) - 1 : 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                for (int c = 0; c < 3; c++) {
                    int bx = x / b * b + b / 2, by = y / b * b + b / 2;
                    if (bx >= W) bx = W - 1;
                    if (by >= H) by = H - 1;
                    unsigned char v = out[(y * W + x) * 4 + c];
                    unsigned char want = out[(by * W + bx) * 4 + c];
                    CHECK(v == want, "pixel %d,%d not uniform in its %ux%u block", x, y, b, b);
                    if (levels) {
                        unsigned step = (v * levels + 127) / 255;
                        CHECK(abs((int)(step * 255 / levels) - v) <= 1, "pixel %d,%d value %u not on %u levels", x, y, v,
                              levels + 1);
                    }
                }
        CHECK(memcmp(out, noise, sizeof out), "frame unchanged");
    }
    printf("present ok: block %u, bits %u\n", block, bits);
    ID3D11Texture2D_Release(staging);
    ID3D11Texture2D_Release(bb);
    IDXGISwapChain_Release(sc);
    IDXGIFactory_Release(factory);
    IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxgi_dev);
    DestroyWindow(hwnd);
}

static IDXGISwapChain *make_swapchain(UINT w, UINT h, HWND *hwnd_out)
{
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"potatomode-test-sc";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, w, h, NULL, NULL, wc.hInstance, NULL);
    CHECK(hwnd, "window");
    IDXGIDevice *dxgi_dev;
    IDXGIAdapter *adapter;
    IDXGIFactory *factory;
    CHECK(SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi_dev)), "IDXGIDevice");
    CHECK(SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_dev, &adapter)), "adapter");
    CHECK(SUCCEEDED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory)), "factory");
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferDesc.Width = w;
    sd.BufferDesc.Height = h;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain *sc;
    CHECK(SUCCEEDED(IDXGIFactory_CreateSwapChain(factory, (IUnknown *)dev, &sd, &sc)), "CreateSwapChain");
    IDXGIFactory_Release(factory);
    IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxgi_dev);
    *hwnd_out = hwnd;
    return sc;
}

static UINT target_width(UINT w, UINT h, UINT bind, DXGI_FORMAT format, ID3D11Texture2D **keep)
{
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, bind, 0, 0};
    ID3D11Texture2D *t;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &t)), "CreateTexture2D target");
    ID3D11Texture2D_GetDesc(t, &d);
    if (keep)
        *keep = t;
    else
        ID3D11Texture2D_Release(t);
    return d.Width;
}

/* Screen-sized targets shrink while GetDesc still reports the size the game
   asked for; a viewport over the left half of the screen must cover the left
   half of the smaller target, even when it was set before the target was
   bound. The real size shows in a whole-resource copy into a staging texture
   of the scaled size, which D3D11 drops when the sizes do not fit. */
static void check_scale(UINT pct)
{
    enum { W = 128, H = 96 };
    HWND hwnd;
    IDXGISwapChain *sc = make_swapchain(W, H, &hwnd);
    ID3D11Texture2D *bb;
    CHECK(SUCCEEDED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb)), "GetBuffer");
    UINT sw = W * pct / 100, sh = H * pct / 100;

    ID3D11Texture2D *rt;
    UINT got = target_width(W, H, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R8G8B8A8_UNORM, &rt);
    CHECK(got == W, "GetDesc of a screen target reports %u, want the requested %u", got, W);
    got = target_width(W, H, D3D11_BIND_DEPTH_STENCIL, DXGI_FORMAT_D24_UNORM_S8_UINT, NULL);
    CHECK(got == W, "GetDesc of a depth target reports %u, want the requested %u", got, W);
    got = target_width(64, 64, D3D11_BIND_RENDER_TARGET, DXGI_FORMAT_R8G8B8A8_UNORM, NULL);
    CHECK(got == 64, "non-screen target width %u, want 64", got);

    /* Overlays and anti-cheats sometimes give an object its own copy of the
       vtable, which still points at our hook; the hook must find the original. */
    ID3D11Texture2DVtbl copied = *rt->lpVtbl;
    ID3D11Texture2DVtbl *shared = rt->lpVtbl;
    rt->lpVtbl = &copied;
    D3D11_TEXTURE2D_DESC through_copy;
    ID3D11Texture2D_GetDesc(rt, &through_copy);
    rt->lpVtbl = shared;
    CHECK(through_copy.Width == W, "GetDesc through a copied vtable reports %u, want %u", through_copy.Width, W);

    static const char hlsl[] =
        "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
        "  float2 uv = float2((id << 1) & 2, id & 2);\n"
        "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
        "}\n"
        "float4 ps() : SV_Target { return float4(1, 1, 1, 1); }\n";
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile compile = dc ? (pD3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    CHECK(compile, "d3dcompiler_47.dll");
    ID3DBlob *vsb, *psb;
    CHECK(SUCCEEDED(compile(hlsl, sizeof hlsl - 1, "t", NULL, NULL, "vs", "vs_4_0", 0, 0, &vsb, NULL)), "vs");
    CHECK(SUCCEEDED(compile(hlsl, sizeof hlsl - 1, "t", NULL, NULL, "ps", "ps_4_0", 0, 0, &psb, NULL)), "ps");
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);
    ID3D11RenderTargetView *rtv;
    CHECK(SUCCEEDED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv)), "rtv");

    D3D11_VIEWPORT vp = {0, 0, W / 2, H, 0, 1};
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    float black[4] = {0, 0, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);

    UINT n = 1;
    D3D11_VIEWPORT back;
    ID3D11DeviceContext_RSGetViewports(ctx, &n, &back);
    CHECK(n == 1 && back.Width == W / 2, "RSGetViewports width %g, want %u as set", back.Width, W / 2);

    D3D11_TEXTURE2D_DESC d = {sw, sh, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0,
                              D3D11_CPU_ACCESS_READ, 0};
    ID3D11Texture2D *staging;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &staging)), "staging");
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)staging, 0, 0, 0, 0, (ID3D11Resource *)rt, 0,
                                              NULL);
    D3D11_MAPPED_SUBRESOURCE map;
    CHECK(SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &map)), "Map");
    for (UINT y = 0; y < sh; y++)
        for (UINT x = 0; x < sw; x++) {
            unsigned char v = ((unsigned char *)map.pData)[y * map.RowPitch + x * 4];
            unsigned char want = x < sw / 2 ? 255 : 0;
            CHECK(v == want, "pixel %u,%u is %u, want %u (viewport not scaled with the target)", x, y, v, want);
        }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    printf("scale ok: %u%% -> %ux%u\n", pct, sw, sh);
    ID3D11Texture2D_Release(staging);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(rt);
    ID3D11Texture2D_Release(bb);
    IDXGISwapChain_Release(sc);
    DestroyWindow(hwnd);
}

/* Presents for a while so the log gets at least one fps/VRAM line. */
static void present_for(UINT seconds)
{
    HWND hwnd;
    IDXGISwapChain *sc = make_swapchain(64, 64, &hwnd);
    ULONGLONG end = GetTickCount64() + seconds * 1000ull;
    UINT n = 0;
    while (GetTickCount64() < end) {
        CHECK(SUCCEEDED(IDXGISwapChain_Present(sc, 0, 0)), "Present");
        n++;
        Sleep(2);
    }
    printf("presented %u frames\n", n);
    IDXGISwapChain_Release(sc);
    DestroyWindow(hwnd);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: d3d11-probe <proxy d3d11.dll> texture <w immutable> <w default> | sampler <filter> | "
                        "present <block> <bits> | scale <percent>\n");
        return 2;
    }
    HMODULE proxy = LoadLibraryW(argv[1]);
    CHECK(proxy, "cannot load %ls", argv[1]);
    create_device_fn create = (create_device_fn)(void *)GetProcAddress(proxy, "D3D11CreateDevice");
    CHECK(create, "no D3D11CreateDevice export");
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    CHECK(SUCCEEDED(create(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx)),
          "D3D11CreateDevice on WARP");

    if (!wcscmp(argv[2], L"texture") && argc == 5)
        check_texture(_wtoi(argv[3]), _wtoi(argv[4]));
    else if (!wcscmp(argv[2], L"sampler") && argc == 4)
        check_sampler((int)wcstol(argv[3], NULL, 0));
    else if (!wcscmp(argv[2], L"present-for") && argc == 4)
        present_for(_wtoi(argv[3]));
    else if (!wcscmp(argv[2], L"scale") && argc == 4)
        check_scale(_wtoi(argv[3]));
    else if (!wcscmp(argv[2], L"present") && argc == 5)
        check_present(_wtoi(argv[3]), _wtoi(argv[4]));
    else
        CHECK(0, "bad arguments");

    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    return 0;
}
