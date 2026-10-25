/* Loads a generated proxy d3d11.dll on WARP and checks what reaches the real
   runtime. The window it creates is never shown. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
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
    wc.lpszClassName = L"anydemake-test";
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

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: d3d11-probe <proxy d3d11.dll> texture <w immutable> <w default> | sampler <filter> | "
                        "present <block> <bits>\n");
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
    else if (!wcscmp(argv[2], L"present") && argc == 5)
        check_present(_wtoi(argv[3]), _wtoi(argv[4]));
    else
        CHECK(0, "bad arguments");

    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    return 0;
}
