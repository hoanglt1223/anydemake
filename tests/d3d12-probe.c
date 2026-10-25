/* Loads a generated proxy d3d12.dll on the WARP adapter and checks what
   reaches the real runtime. The window it creates is never shown. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#pragma warning(push)
#pragma warning(disable : 4115)
#include <d3dcompiler.h>
#pragma warning(pop)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT(WINAPI *create_device_fn)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT(WINAPI *serialize_fn)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **,
                                      ID3DBlob **);

#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            fprintf(stderr, "FAIL line %d: ", __LINE__);                                                              \
            fprintf(stderr, __VA_ARGS__);                                                                             \
            fprintf(stderr, "\n");                                                                                    \
            exit(1);                                                                                                  \
        }                                                                                                             \
    } while (0)

static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence;
static UINT64 fence_value;
static IDXGIFactory4 *factory;
static pD3DCompile compile;

static void submit_and_wait(void)
{
    CHECK(SUCCEEDED(ID3D12GraphicsCommandList_Close(list)), "Close");
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    ID3D12CommandQueue_Signal(queue, fence, ++fence_value);
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    ID3D12Fence_SetEventOnCompletion(fence, fence_value, ev);
    WaitForSingleObject(ev, 10000);
    CloseHandle(ev);
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
}

static ID3D12Resource *buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_FLAGS flags,
                              D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap = {type};
    D3D12_RESOURCE_DESC d = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                             D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags};
    ID3D12Resource *r;
    CHECK(SUCCEEDED(ID3D12Device_CreateCommittedResource(dev, &heap, D3D12_HEAP_FLAG_NONE, &d, state, NULL,
                                                         &IID_ID3D12Resource, (void **)&r)),
          "buffer");
    return r;
}

static void transition(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

/* Samples a 2x1 texture (0, 1) at u = 0.6 through a heap sampler and a
   static sampler, both anisotropic: filtered gives 0.7, point gives 1. */
static void check_sampler(HMODULE proxy, float want_heap, float want_static)
{
    static const char hlsl[] = "Texture2D<float> t : register(t0);\n"
                               "SamplerState heap_s : register(s0);\n"
                               "SamplerState static_s : register(s1);\n"
                               "RWStructuredBuffer<float> o : register(u0);\n"
                               "[numthreads(1, 1, 1)] void main() {\n"
                               "  o[0] = t.SampleLevel(heap_s, float2(0.6, 0.5), 0);\n"
                               "  o[1] = t.SampleLevel(static_s, float2(0.6, 0.5), 0);\n"
                               "}\n";
    ID3DBlob *cs;
    CHECK(SUCCEEDED(compile(hlsl, sizeof hlsl - 1, "t", NULL, NULL, "main", "cs_5_0", 0, 0, &cs, NULL)), "cs");

    D3D12_DESCRIPTOR_RANGE srv_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_DESCRIPTOR_RANGE smp_range = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER p[3] = {0};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 1;
    p[0].DescriptorTable.pDescriptorRanges = &srv_range;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &smp_range;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_STATIC_SAMPLER_DESC ss = {D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                    D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 16,
                                    D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK, 0,
                                    D3D12_FLOAT32_MAX, 1, 0, D3D12_SHADER_VISIBILITY_ALL};
    D3D12_ROOT_SIGNATURE_DESC rsd = {3, p, 1, &ss, 0};
    /* Called through the proxy's forwarded export. */
    serialize_fn ser = (serialize_fn)(void *)GetProcAddress(proxy, "D3D12SerializeRootSignature");
    ID3DBlob *rs_blob, *err = NULL;
    CHECK(ser && SUCCEEDED(ser(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &err)), "serialize root signature");
    ID3D12RootSignature *rs;
    CHECK(SUCCEEDED(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(rs_blob),
                                                     ID3D10Blob_GetBufferSize(rs_blob), &IID_ID3D12RootSignature,
                                                     (void **)&rs)),
          "CreateRootSignature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {rs, {ID3D10Blob_GetBufferPointer(cs), ID3D10Blob_GetBufferSize(cs)}};
    ID3D12PipelineState *pso;
    CHECK(SUCCEEDED(ID3D12Device_CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso)),
          "pso");

    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 2, 1, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *tex;
    CHECK(SUCCEEDED(ID3D12Device_CreateCommittedResource(dev, &heap, D3D12_HEAP_FLAG_NONE, &td,
                                                         D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
                                                         (void **)&tex)),
          "texture");
    ID3D12Resource *upload = buffer(D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_GENERIC_READ);
    float *up;
    ID3D12Resource_Map(upload, 0, NULL, (void **)&up);
    up[0] = 0;
    up[1] = 1;
    ID3D12Resource_Unmap(upload, 0, NULL);
    D3D12_TEXTURE_COPY_LOCATION dst = {tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src = {upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    src.PlacedFootprint.Footprint.Width = 2;
    src.PlacedFootprint.Footprint.Height = 1;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = 256;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    transition(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ID3D12DescriptorHeap *srv_heap, *smp_heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    CHECK(SUCCEEDED(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&srv_heap)), "heap");
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    CHECK(SUCCEEDED(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&smp_heap)), "heap");
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu, smp_gpu;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srv_heap, &cpu);
    ID3D12Device_CreateShaderResourceView(dev, tex, NULL, cpu);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(smp_heap, &cpu);
    D3D12_SAMPLER_DESC sd = {D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                             D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 16,
                             D3D12_COMPARISON_FUNC_NEVER, {0}, 0, D3D12_FLOAT32_MAX};
    ID3D12Device_CreateSampler(dev, &sd, cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srv_heap, &srv_gpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(smp_heap, &smp_gpu);

    ID3D12Resource *out = buffer(D3D12_HEAP_TYPE_DEFAULT, 8, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *readback = buffer(D3D12_HEAP_TYPE_READBACK, 8, D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12DescriptorHeap *heaps[2] = {srv_heap, smp_heap};
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 2, heaps);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, srv_gpu);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 1, smp_gpu);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 2, ID3D12Resource_GetGPUVirtualAddress(out));
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    transition(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, out, 0, 8);
    submit_and_wait();

    float *r;
    D3D12_RANGE range = {0, 8};
    ID3D12Resource_Map(readback, 0, &range, (void **)&r);
    printf("sampler: heap %.3f static %.3f\n", r[0], r[1]);
    CHECK(fabsf(r[0] - want_heap) < 0.02f, "heap sampler gave %.3f, want %.3f", r[0], want_heap);
    CHECK(fabsf(r[1] - want_static) < 0.02f, "static sampler gave %.3f, want %.3f", r[1], want_static);
    ID3D12Resource_Unmap(readback, 0, NULL);
}

/* Fills back buffer 0 with noise, presents, and reads it back; a sequential
   flip swapchain keeps the presented contents. */
static void check_present(UINT block, UINT bits)
{
    enum { W = 128, H = 96 };
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"anydemake-test12";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);
    CHECK(hwnd, "window");
    DXGI_SWAP_CHAIN_DESC1 sd = {0};
    sd.Width = W;
    sd.Height = H;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    IDXGISwapChain1 *sc1;
    CHECK(SUCCEEDED(IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)queue, hwnd, &sd, NULL, NULL, &sc1)),
          "CreateSwapChainForHwnd");
    IDXGISwapChain3 *sc;
    CHECK(SUCCEEDED(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&sc)), "IDXGISwapChain3");
    UINT index = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
    ID3D12Resource *bb;
    CHECK(SUCCEEDED(IDXGISwapChain3_GetBuffer(sc, index, &IID_ID3D12Resource, (void **)&bb)), "GetBuffer");

    static unsigned char noise[W * H * 4], out[W * H * 4];
    for (int i = 0; i < W * H * 4; i++)
        noise[i] = (unsigned char)((i * 2654435761u) >> 13);
    enum { PITCH = 512 }; /* W * 4 rounded up to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT */
    ID3D12Resource *upload = buffer(D3D12_HEAP_TYPE_UPLOAD, PITCH * H, D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_GENERIC_READ);
    unsigned char *up;
    ID3D12Resource_Map(upload, 0, NULL, (void **)&up);
    for (int y = 0; y < H; y++)
        memcpy(up + y * PITCH, noise + y * W * 4, W * 4);
    ID3D12Resource_Unmap(upload, 0, NULL);
    D3D12_TEXTURE_COPY_LOCATION tex = {bb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION buf = {upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    buf.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    buf.PlacedFootprint.Footprint.Width = W;
    buf.PlacedFootprint.Footprint.Height = H;
    buf.PlacedFootprint.Footprint.Depth = 1;
    buf.PlacedFootprint.Footprint.RowPitch = PITCH;
    transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &tex, 0, 0, 0, &buf, NULL);
    transition(bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    submit_and_wait();
    CHECK(SUCCEEDED(IDXGISwapChain3_Present(sc, 0, 0)), "Present");

    ID3D12Resource *readback = buffer(D3D12_HEAP_TYPE_READBACK, PITCH * H, D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
    buf.pResource = readback;
    transition(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &buf, 0, 0, 0, &tex, NULL);
    transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    submit_and_wait();
    unsigned char *rb;
    ID3D12Resource_Map(readback, 0, NULL, (void **)&rb);
    for (int y = 0; y < H; y++)
        memcpy(out + y * W * 4, rb + y * PITCH, W * 4);
    ID3D12Resource_Unmap(readback, 0, NULL);

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
                    CHECK(v == out[(by * W + bx) * 4 + c], "pixel %d,%d not uniform in its %ux%u block", x, y, b, b);
                    if (levels) {
                        unsigned step = (v * levels + 127) / 255;
                        CHECK(abs((int)(step * 255 / levels) - v) <= 1, "pixel %d,%d value %u not on %u levels", x,
                              y, v, levels + 1);
                    }
                }
        CHECK(memcmp(out, noise, sizeof out), "frame unchanged");
    }
    printf("present ok: block %u, bits %u\n", block, bits);
    ID3D12Resource_Release(bb);
    IDXGISwapChain3_Release(sc);
    IDXGISwapChain1_Release(sc1);
    DestroyWindow(hwnd);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: d3d12-probe <proxy d3d12.dll> sampler <heap> <static> | present <block> <bits>\n");
        return 2;
    }
    HMODULE proxy = LoadLibraryW(argv[1]);
    CHECK(proxy, "cannot load %ls", argv[1]);
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    compile = dc ? (pD3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    CHECK(compile, "d3dcompiler_47.dll");

    /* A forwarded export called before anything else: the stub must load the real DLL first. */
    typedef HRESULT(WINAPI * get_debug_fn)(REFIID, void **);
    get_debug_fn get_debug = (get_debug_fn)(void *)GetProcAddress(proxy, "D3D12GetDebugInterface");
    void *debug = NULL;
    CHECK(get_debug, "no D3D12GetDebugInterface export");
    HRESULT hr = get_debug(&IID_ID3D12Debug, &debug);
    CHECK(hr != E_NOTIMPL, "D3D12GetDebugInterface was not forwarded");
    if (debug)
        IUnknown_Release((IUnknown *)debug);

    CHECK(SUCCEEDED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory)), "factory");
    IDXGIAdapter *warp;
    CHECK(SUCCEEDED(IDXGIFactory4_EnumWarpAdapter(factory, &IID_IDXGIAdapter, (void **)&warp)), "WARP adapter");
    create_device_fn create = (create_device_fn)(void *)GetProcAddress(proxy, "D3D12CreateDevice");
    CHECK(create, "no D3D12CreateDevice export");
    CHECK(SUCCEEDED(create((IUnknown *)warp, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev)),
          "D3D12CreateDevice on WARP");
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(SUCCEEDED(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue)), "queue");
    CHECK(SUCCEEDED(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        &IID_ID3D12CommandAllocator, (void **)&alloc)),
          "allocator");
    CHECK(SUCCEEDED(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                                   &IID_ID3D12GraphicsCommandList, (void **)&list)),
          "list");
    CHECK(SUCCEEDED(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)),
          "fence");

    if (!wcscmp(argv[2], L"sampler") && argc == 5)
        check_sampler(proxy, (float)_wtof(argv[3]), (float)_wtof(argv[4]));
    else if (!wcscmp(argv[2], L"present") && argc == 5)
        check_present(_wtoi(argv[3]), _wtoi(argv[4]));
    else
        CHECK(0, "bad arguments");
    return 0;
}
