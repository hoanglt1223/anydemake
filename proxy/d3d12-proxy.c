#define CINTERFACE
#define COBJMACROS
#include "common.h"

#include <d3d12.h>
/* C4115: d3dcompiler.h names ID3D10Effect in a prototype without declaring it. */
#pragma warning(push)
#pragma warning(disable : 4115)
#include <d3dcompiler.h>
#pragma warning(pop)
#include <dxgi1_5.h>
#include <stdlib.h>
#include <string.h>

const wchar_t anydemake_dll_name[] = L"d3d12.dll";
#define FRAMES 3

/* ---- samplers ---------------------------------------------------------- */

typedef void(STDMETHODCALLTYPE *create_sampler_fn)(ID3D12Device *, const D3D12_SAMPLER_DESC *,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *create_sampler2_fn)(ID3D12Device11 *, const D3D12_SAMPLER_DESC2 *,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE);
typedef HRESULT(STDMETHODCALLTYPE *create_root_signature_fn)(ID3D12Device *, UINT, const void *, SIZE_T, REFIID,
                                                             void **);
typedef HRESULT(WINAPI *deserializer_fn)(const void *, SIZE_T, REFIID, void **);
typedef HRESULT(WINAPI *serialize_fn)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *, ID3DBlob **, ID3DBlob **);

static void STDMETHODCALLTYPE hk_CreateSampler(ID3D12Device *dev, const D3D12_SAMPLER_DESC *desc,
                                               D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
    create_sampler_fn orig = (create_sampler_fn)ORIG(dev, CreateSampler);
    if (!desc || in_post) {
        orig(dev, desc, dst);
        return;
    }
    D3D12_SAMPLER_DESC d = *desc;
    d.Filter = (D3D12_FILTER)demake_filter(d.Filter, &d.MaxAnisotropy);
    orig(dev, &d, dst);
}

static void STDMETHODCALLTYPE hk_CreateSampler2(ID3D12Device11 *dev, const D3D12_SAMPLER_DESC2 *desc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
    create_sampler2_fn orig = (create_sampler2_fn)ORIG(dev, CreateSampler2);
    if (!desc || in_post) {
        orig(dev, desc, dst);
        return;
    }
    D3D12_SAMPLER_DESC2 d = *desc;
    d.Filter = (D3D12_FILTER)demake_filter(d.Filter, &d.MaxAnisotropy);
    orig(dev, &d, dst);
}

/* Static samplers live inside serialized root signatures: deserialize,
   change the filters, and serialize again with the same version. */
static ID3DBlob *patch_root_signature(const void *blob, SIZE_T len)
{
    deserializer_fn deser_fn =
        (deserializer_fn)(void *)real_proc("D3D12CreateVersionedRootSignatureDeserializer");
    serialize_fn ser_fn = (serialize_fn)(void *)real_proc("D3D12SerializeVersionedRootSignature");
    ID3D12VersionedRootSignatureDeserializer *deser;
    if (!deser_fn || !ser_fn ||
        FAILED(deser_fn(blob, len, &IID_ID3D12VersionedRootSignatureDeserializer, (void **)&deser)))
        return NULL;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *src =
        ID3D12VersionedRootSignatureDeserializer_GetUnconvertedRootSignatureDesc(deser);
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC d = *src;
    ID3DBlob *out = NULL, *err = NULL;
    void *samplers = NULL;
    UINT n = 0;
    if (d.Version == D3D_ROOT_SIGNATURE_VERSION_1_2) {
        n = d.Desc_1_2.NumStaticSamplers;
        D3D12_STATIC_SAMPLER_DESC1 *s = n ? malloc(n * sizeof *s) : NULL;
        for (UINT i = 0; s && i < n; i++) {
            s[i] = d.Desc_1_2.pStaticSamplers[i];
            s[i].Filter = (D3D12_FILTER)demake_filter(s[i].Filter, &s[i].MaxAnisotropy);
        }
        d.Desc_1_2.pStaticSamplers = s;
        samplers = s;
    } else {
        /* 1.0 and 1.1 share the static sampler layout and its position. */
        n = d.Version == D3D_ROOT_SIGNATURE_VERSION_1_1 ? d.Desc_1_1.NumStaticSamplers : d.Desc_1_0.NumStaticSamplers;
        const D3D12_STATIC_SAMPLER_DESC *in =
            d.Version == D3D_ROOT_SIGNATURE_VERSION_1_1 ? d.Desc_1_1.pStaticSamplers : d.Desc_1_0.pStaticSamplers;
        D3D12_STATIC_SAMPLER_DESC *s = n ? malloc(n * sizeof *s) : NULL;
        for (UINT i = 0; s && i < n; i++) {
            s[i] = in[i];
            s[i].Filter = (D3D12_FILTER)demake_filter(s[i].Filter, &s[i].MaxAnisotropy);
        }
        if (d.Version == D3D_ROOT_SIGNATURE_VERSION_1_1)
            d.Desc_1_1.pStaticSamplers = s;
        else
            d.Desc_1_0.pStaticSamplers = s;
        samplers = s;
    }
    if (samplers && FAILED(ser_fn(&d, &out, &err)))
        out = NULL;
    if (err) {
        log_line("root signature: %.300s", (const char *)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    }
    free(samplers);
    ID3D12VersionedRootSignatureDeserializer_Release(deser);
    return out;
}

static HRESULT STDMETHODCALLTYPE hk_CreateRootSignature(ID3D12Device *dev, UINT node, const void *blob, SIZE_T len,
                                                        REFIID iid, void **out)
{
    create_root_signature_fn orig = (create_root_signature_fn)ORIG(dev, CreateRootSignature);
    ID3DBlob *patched = in_post ? NULL : patch_root_signature(blob, len);
    if (!patched)
        return orig(dev, node, blob, len, iid, out);
    HRESULT hr = orig(dev, node, ID3D10Blob_GetBufferPointer(patched), ID3D10Blob_GetBufferSize(patched), iid, out);
    ID3D10Blob_Release(patched);
    return SUCCEEDED(hr) ? hr : orig(dev, node, blob, len, iid, out);
}

/* ---- present-time post-process ----------------------------------------- */

typedef struct post12 {
    ID3D12Device *dev;
    ID3D12RootSignature *rs;
    ID3DBlob *vs, *ps;
    ID3D12PipelineState *pso[4];
    DXGI_FORMAT pso_format[4];
    ID3D12DescriptorHeap *srv_heap, *rtv_heap;
    UINT srv_step, rtv_step;
    ID3D12CommandAllocator *alloc[FRAMES];
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    UINT64 fence_value, frame_fence[FRAMES];
    HANDLE event;
    ID3D12Resource *copy;
    D3D12_RESOURCE_DESC copy_desc;
    UINT64 frame;
    UINT64 area; /* back buffer size of the device that owns the post-process */
    int broken;
} post12;

static post12 post;

/* Returns 0 when the GPU did not get there (removed device, 5 s timeout);
   the post-process then stays off rather than reuse busy allocators. */
static int wait_fence(UINT64 value)
{
    if (!value)
        return 1;
    UINT64 done = ID3D12Fence_GetCompletedValue(post.fence);
    if (done == UINT64_MAX || FAILED(ID3D12Device_GetDeviceRemovedReason(post.dev))) {
        post.broken = 1;
        return 0;
    }
    if (done >= value)
        return 1;
    if (FAILED(ID3D12Fence_SetEventOnCompletion(post.fence, value, post.event)) ||
        WaitForSingleObject(post.event, 5000) != WAIT_OBJECT_0) {
        log_line("post-process off: the GPU did not finish a frame in 5 s");
        post.broken = 1;
        return 0;
    }
    return 1;
}

static void post_release(void)
{
    if (post.fence)
        wait_fence(post.fence_value);
    for (int i = 0; i < 4; i++)
        if (post.pso[i]) ID3D12PipelineState_Release(post.pso[i]);
    for (int i = 0; i < FRAMES; i++)
        if (post.alloc[i]) ID3D12CommandAllocator_Release(post.alloc[i]);
    if (post.rs) ID3D12RootSignature_Release(post.rs);
    if (post.vs) ID3D10Blob_Release(post.vs);
    if (post.ps) ID3D10Blob_Release(post.ps);
    if (post.srv_heap) ID3D12DescriptorHeap_Release(post.srv_heap);
    if (post.rtv_heap) ID3D12DescriptorHeap_Release(post.rtv_heap);
    if (post.list) ID3D12GraphicsCommandList_Release(post.list);
    if (post.fence) ID3D12Fence_Release(post.fence);
    if (post.event) CloseHandle(post.event);
    if (post.copy) ID3D12Resource_Release(post.copy);
    if (post.dev) ID3D12Device_Release(post.dev);
    memset(&post, 0, sizeof post);
}

static HRESULT compile(pD3DCompile fn, const char *entry, const char *target, ID3DBlob **blob)
{
    ID3DBlob *err = NULL;
    HRESULT hr = fn(post_hlsl, post_hlsl_len, "anydemake", NULL, NULL, entry, target, 0, 0, blob, &err);
    if (err) {
        log_line("shader %s: %.300s", entry, (const char *)ID3D10Blob_GetBufferPointer(err));
        ID3D10Blob_Release(err);
    }
    return hr;
}

static int post_setup(ID3D12Device *dev)
{
    if (post.dev == dev)
        return !post.broken;
    post_release();
    post.dev = dev;
    ID3D12Device_AddRef(dev);
    post.broken = 1;

    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile fn = dc ? (pD3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    if (!fn || FAILED(compile(fn, "vs", "vs_5_0", &post.vs)) || FAILED(compile(fn, "ps", "ps_5_0", &post.ps))) {
        log_line("post-process off: cannot compile shaders");
        return 0;
    }

    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2] = {0};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rsd = {D3D_ROOT_SIGNATURE_VERSION_1_0};
    rsd.Desc_1_0.NumParameters = 2;
    rsd.Desc_1_0.pParameters = params;
    serialize_fn ser = (serialize_fn)(void *)real_proc("D3D12SerializeVersionedRootSignature");
    ID3DBlob *rs_blob = NULL;
    create_root_signature_fn create_rs = (create_root_signature_fn)ORIG(dev, CreateRootSignature);
    if (!create_rs)
        create_rs = (create_root_signature_fn)dev->lpVtbl->CreateRootSignature;
    int ok = ser && SUCCEEDED(ser(&rsd, &rs_blob, NULL)) &&
             SUCCEEDED(create_rs(dev, 0, ID3D10Blob_GetBufferPointer(rs_blob), ID3D10Blob_GetBufferSize(rs_blob),
                                 &IID_ID3D12RootSignature, (void **)&post.rs));
    if (rs_blob) ID3D10Blob_Release(rs_blob);

    D3D12_DESCRIPTOR_HEAP_DESC srv = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, FRAMES,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC rtv = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, FRAMES, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    ok = ok && SUCCEEDED(ID3D12Device_CreateDescriptorHeap(dev, &srv, &IID_ID3D12DescriptorHeap, (void **)&post.srv_heap)) &&
         SUCCEEDED(ID3D12Device_CreateDescriptorHeap(dev, &rtv, &IID_ID3D12DescriptorHeap, (void **)&post.rtv_heap));
    for (int i = 0; ok && i < FRAMES; i++)
        ok = SUCCEEDED(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                           &IID_ID3D12CommandAllocator, (void **)&post.alloc[i]));
    ok = ok &&
         SUCCEEDED(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, post.alloc[0], NULL,
                                                  &IID_ID3D12GraphicsCommandList, (void **)&post.list)) &&
         SUCCEEDED(ID3D12GraphicsCommandList_Close(post.list)) &&
         SUCCEEDED(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&post.fence)) &&
         (post.event = CreateEventW(NULL, FALSE, FALSE, NULL)) != NULL;
    if (!ok) {
        log_line("post-process off: D3D12 setup failed");
        return 0;
    }
    post.srv_step = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    post.rtv_step = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    post.broken = 0;
    return 1;
}

static ID3D12PipelineState *pso_for(DXGI_FORMAT format)
{
    int free_slot = -1;
    for (int i = 0; i < 4; i++) {
        if (post.pso[i] && post.pso_format[i] == format)
            return post.pso[i];
        if (!post.pso[i] && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        return NULL;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {0};
    d.pRootSignature = post.rs;
    d.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(post.vs);
    d.VS.BytecodeLength = ID3D10Blob_GetBufferSize(post.vs);
    d.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(post.ps);
    d.PS.BytecodeLength = ID3D10Blob_GetBufferSize(post.ps);
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = format;
    d.SampleDesc.Count = 1;
    if (FAILED(ID3D12Device_CreateGraphicsPipelineState(post.dev, &d, &IID_ID3D12PipelineState,
                                                        (void **)&post.pso[free_slot]))) {
        log_line("post-process: no pipeline for format %d", format);
        return NULL;
    }
    post.pso_format[free_slot] = format;
    return post.pso[free_slot];
}

static void barrier(ID3D12Resource *res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
                    D3D12_RESOURCE_BARRIER *out)
{
    memset(out, 0, sizeof *out);
    out->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    out->Transition.pResource = res;
    out->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    out->Transition.StateBefore = before;
    out->Transition.StateAfter = after;
}

static int ensure_copy(const D3D12_RESOURCE_DESC *bb)
{
    if (post.copy && post.copy_desc.Width == bb->Width && post.copy_desc.Height == bb->Height &&
        post.copy_desc.Format == bb->Format)
        return 1;
    if (!wait_fence(post.fence_value))
        return 0;
    if (post.copy) ID3D12Resource_Release(post.copy);
    post.copy = NULL;
    D3D12_RESOURCE_DESC d = *bb;
    d.MipLevels = 1;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_DEFAULT};
    if (FAILED(ID3D12Device_CreateCommittedResource(post.dev, &heap, D3D12_HEAP_FLAG_NONE, &d,
                                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                                                    &IID_ID3D12Resource, (void **)&post.copy)))
        return 0;
    post.copy_desc = d;
    return 1;
}

/* IDXGISwapChain::GetDevice returns the device, not the queue the swapchain
   presents on, so the queue is attached to the swapchain when the factory
   creates it and is released together with the swapchain. */
static const GUID queue_guid = {0x9e4b1d02, 0x5a37, 0x4c8e, {0x83, 0x1f, 0x6d, 0x29, 0xb4, 0x70, 0x0c, 0x55}};

static void remember_queue(IUnknown *sc, IUnknown *device)
{
    ID3D12CommandQueue *q;
    IDXGIObject *obj;
    if (!sc || !device || FAILED(IUnknown_QueryInterface(device, &IID_ID3D12CommandQueue, (void **)&q)))
        return;
    if (SUCCEEDED(IUnknown_QueryInterface(sc, &IID_IDXGIObject, (void **)&obj))) {
        IDXGIObject_SetPrivateDataInterface(obj, &queue_guid, (IUnknown *)q);
        IDXGIObject_Release(obj);
    }
    ID3D12CommandQueue_Release(q);
}

static ID3D12CommandQueue *queue_of(IDXGISwapChain *sc)
{
    IUnknown *u = NULL;
    UINT size = sizeof u;
    ID3D12CommandQueue *q = NULL;
    if (SUCCEEDED(IDXGISwapChain_GetPrivateData(sc, &queue_guid, &size, &u)) && u) {
        IUnknown_QueryInterface(u, &IID_ID3D12CommandQueue, (void **)&q);
        IUnknown_Release(u);
    }
    return q;
}

/* Runs on the game's queue right before its Present; the back buffer is in
   the PRESENT state there and goes back to it. */
static void post_process(IDXGISwapChain *sc)
{
    ID3D12CommandQueue *queue = NULL;
    IDXGISwapChain3 *sc3 = NULL;
    ID3D12Device *dev = NULL;
    ID3D12Resource *bb = NULL;
    if (!(queue = queue_of(sc)))
        return; /* a D3D11 swapchain; the d3d11.dll proxy handles it */
    EnterCriticalSection(&hook_lock);
    if (FAILED(IDXGISwapChain_QueryInterface(sc, &IID_IDXGISwapChain3, (void **)&sc3)) ||
        FAILED(ID3D12CommandQueue_GetDevice(queue, &IID_ID3D12Device, (void **)&dev)) ||
        FAILED(IDXGISwapChain3_GetBuffer(sc3, IDXGISwapChain3_GetCurrentBackBufferIndex(sc3), &IID_ID3D12Resource,
                                         (void **)&bb))) {
        if (!post.frame)
            log_line("post-process off: no D3D12 device or back buffer");
        goto out;
    }
    D3D12_RESOURCE_DESC d;
    ID3D12Resource_GetDesc(bb, &d);
    /* A second device must not make the state rebuild every frame; the biggest swapchain wins. */
    if (post.dev && post.dev != dev && d.Width * d.Height <= post.area)
        goto out;
    if (!post_setup(dev))
        goto out;
    post.area = d.Width * d.Height;
    ID3D12PipelineState *pso;
    if (d.SampleDesc.Count != 1 || !ensure_copy(&d) || !(pso = pso_for(d.Format))) {
        if (!post.frame)
            log_line("post-process off: back buffer %llux%u format %d not supported", d.Width, d.Height, d.Format);
        goto out;
    }
    if (!post.frame)
        log_line("post-process on: %llux%u format %d", d.Width, d.Height, d.Format);

    UINT i = (UINT)(post.frame % FRAMES);
    if (!wait_fence(post.frame_fence[i]))
        goto out;
    post.frame++;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu, rtv;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(post.srv_heap, &srv_cpu);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(post.srv_heap, &srv_gpu);
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(post.rtv_heap, &rtv);
    srv_cpu.ptr += (SIZE_T)i * post.srv_step;
    srv_gpu.ptr += (UINT64)i * post.srv_step;
    rtv.ptr += (SIZE_T)i * post.rtv_step;
    ID3D12Device_CreateShaderResourceView(dev, post.copy, NULL, srv_cpu);
    ID3D12Device_CreateRenderTargetView(dev, bb, NULL, rtv);

    ID3D12CommandAllocator_Reset(post.alloc[i]);
    ID3D12GraphicsCommandList *l = post.list;
    ID3D12GraphicsCommandList_Reset(l, post.alloc[i], pso);
    D3D12_RESOURCE_BARRIER b[2];
    barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE, &b[0]);
    barrier(post.copy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST, &b[1]);
    ID3D12GraphicsCommandList_ResourceBarrier(l, 2, b);
    ID3D12GraphicsCommandList_CopyResource(l, post.copy, bb);
    barrier(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET, &b[0]);
    barrier(post.copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &b[1]);
    ID3D12GraphicsCommandList_ResourceBarrier(l, 2, b);

    float c[4] = {(float)d.Width, (float)d.Height, (float)(cfg.pixelate > 1 ? cfg.pixelate : 1),
                  cfg.color_bits ? (float)((1u << (cfg.color_bits > 8 ? 8 : cfg.color_bits)) - 1) : 0.0f};
    D3D12_VIEWPORT vp = {0, 0, (float)d.Width, (float)d.Height, 0, 1};
    D3D12_RECT scissor = {0, 0, (LONG)d.Width, (LONG)d.Height};
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(l, post.rs);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(l, 1, &post.srv_heap);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(l, 0, 4, c, 0);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(l, 1, srv_gpu);
    ID3D12GraphicsCommandList_RSSetViewports(l, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(l, 1, &scissor);
    ID3D12GraphicsCommandList_OMSetRenderTargets(l, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(l, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(l, 3, 1, 0, 0);
    barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT, &b[0]);
    ID3D12GraphicsCommandList_ResourceBarrier(l, 1, b);
    if (SUCCEEDED(ID3D12GraphicsCommandList_Close(l))) {
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&l);
        ID3D12CommandQueue_Signal(queue, post.fence, ++post.fence_value);
        post.frame_fence[i] = post.fence_value;
    }
out:
    LeaveCriticalSection(&hook_lock);
    if (bb) ID3D12Resource_Release(bb);
    if (dev) ID3D12Device_Release(dev);
    if (sc3) IDXGISwapChain3_Release(sc3);
    ID3D12CommandQueue_Release(queue);
}

typedef HRESULT(STDMETHODCALLTYPE *present_fn)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *present1_fn)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);

static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain *sc, UINT sync, UINT flags)
{
    present_fn orig = (present_fn)ORIG(sc, Present);
    in_present++;
    if (in_present == 1 && post_enabled(flags))
    {
        in_post++;
        post_process(sc);
        in_post--;
    }
    if (in_present == 1)
        stats_frame();
    HRESULT hr = orig(sc, sync, flags);
    in_present--;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1 *sc, UINT sync, UINT flags,
                                             const DXGI_PRESENT_PARAMETERS *params)
{
    present1_fn orig = (present1_fn)ORIG(sc, Present1);
    in_present++;
    if (in_present == 1 && post_enabled(flags))
    {
        in_post++;
        post_process((IDXGISwapChain *)sc);
        in_post--;
    }
    if (in_present == 1)
        stats_frame();
    HRESULT hr = orig(sc, sync, flags, params);
    in_present--;
    return hr;
}

typedef HRESULT(STDMETHODCALLTYPE *create_sc_fn)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *,
                                                 IDXGISwapChain **);
typedef HRESULT(STDMETHODCALLTYPE *create_sc_hwnd_fn)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                                      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *,
                                                      IDXGISwapChain1 **);
typedef HRESULT(STDMETHODCALLTYPE *create_sc_core_fn)(IDXGIFactory2 *, IUnknown *, IUnknown *,
                                                      const DXGI_SWAP_CHAIN_DESC1 *, IDXGIOutput *,
                                                      IDXGISwapChain1 **);
typedef HRESULT(STDMETHODCALLTYPE *create_sc_comp_fn)(IDXGIFactory2 *, IUnknown *, const DXGI_SWAP_CHAIN_DESC1 *,
                                                      IDXGIOutput *, IDXGISwapChain1 **);

static HRESULT STDMETHODCALLTYPE hk_CreateSwapChain(IDXGIFactory *f, IUnknown *device, DXGI_SWAP_CHAIN_DESC *d,
                                                    IDXGISwapChain **out)
{
    HRESULT hr = ((create_sc_fn)ORIG(f, CreateSwapChain))(f, device, d, out);
    if (SUCCEEDED(hr) && out)
        remember_queue((IUnknown *)*out, device);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForHwnd(IDXGIFactory2 *f, IUnknown *device, HWND hwnd,
                                                           const DXGI_SWAP_CHAIN_DESC1 *d,
                                                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs,
                                                           IDXGIOutput *output, IDXGISwapChain1 **out)
{
    HRESULT hr = ((create_sc_hwnd_fn)ORIG(f, CreateSwapChainForHwnd))(f, device, hwnd, d, fs, output, out);
    if (SUCCEEDED(hr) && out)
        remember_queue((IUnknown *)*out, device);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForCoreWindow(IDXGIFactory2 *f, IUnknown *device, IUnknown *window,
                                                                 const DXGI_SWAP_CHAIN_DESC1 *d, IDXGIOutput *output,
                                                                 IDXGISwapChain1 **out)
{
    HRESULT hr = ((create_sc_core_fn)ORIG(f, CreateSwapChainForCoreWindow))(f, device, window, d, output, out);
    if (SUCCEEDED(hr) && out)
        remember_queue((IUnknown *)*out, device);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateSwapChainForComposition(IDXGIFactory2 *f, IUnknown *device,
                                                                  const DXGI_SWAP_CHAIN_DESC1 *d, IDXGIOutput *output,
                                                                  IDXGISwapChain1 **out)
{
    HRESULT hr = ((create_sc_comp_fn)ORIG(f, CreateSwapChainForComposition))(f, device, d, output, out);
    if (SUCCEEDED(hr) && out)
        remember_queue((IUnknown *)*out, device);
    return hr;
}

static void hook_factory(IDXGIFactory2 *factory)
{
    patch_slot((void **)&factory->lpVtbl->CreateSwapChain, (void *)hk_CreateSwapChain);
    patch_slot((void **)&factory->lpVtbl->CreateSwapChainForHwnd, (void *)hk_CreateSwapChainForHwnd);
    patch_slot((void **)&factory->lpVtbl->CreateSwapChainForCoreWindow, (void *)hk_CreateSwapChainForCoreWindow);
    patch_slot((void **)&factory->lpVtbl->CreateSwapChainForComposition, (void *)hk_CreateSwapChainForComposition);
}

static void note_adapter(ID3D12Device *dev, IDXGIFactory2 *factory)
{
    IDXGIFactory4 *f4;
    IDXGIAdapter *adapter;
    LUID luid;
    if (!cfg.log || FAILED(IDXGIFactory2_QueryInterface(factory, &IID_IDXGIFactory4, (void **)&f4)))
        return;
    ID3D12Device_GetAdapterLuid(dev, &luid);
    if (SUCCEEDED(IDXGIFactory4_EnumAdapterByLuid(f4, luid, &IID_IDXGIAdapter, (void **)&adapter))) {
        stats_adapter(adapter);
        IDXGIAdapter_Release(adapter);
    }
    IDXGIFactory4_Release(f4);
}

/* The game's swapchain comes from IDXGIFactory; its Present slot is found
   on a throwaway swapchain made for the same device, and the factory is
   hooked afterwards so the throwaway one is not remembered. */
static void hook_swapchains_of(ID3D12Device *dev)
{
    ID3D12CommandQueue *queue = NULL;
    IDXGIFactory2 *factory = NULL;
    IDXGISwapChain1 *sc = NULL;
    HWND hwnd = hidden_window();
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    DXGI_SWAP_CHAIN_DESC1 sd = {0};
    sd.Width = 64;
    sd.Height = 64;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (hwnd && SUCCEEDED(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue)) &&
        SUCCEEDED(CreateDXGIFactory2(0, &IID_IDXGIFactory2, (void **)&factory)) && (note_adapter(dev, factory), 1) &&
        SUCCEEDED(IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)queue, hwnd, &sd, NULL, NULL, &sc))) {
        /* Each interface of a D3D12 swapchain may have its own vtable. */
        static const IID *const iids[] = {&IID_IDXGISwapChain1, &IID_IDXGISwapChain2, &IID_IDXGISwapChain3,
                                          &IID_IDXGISwapChain4};
        for (int i = 0; i < 4; i++) {
            IDXGISwapChain1 *s;
            if (SUCCEEDED(IDXGISwapChain1_QueryInterface(sc, iids[i], (void **)&s))) {
                patch_slot((void **)&s->lpVtbl->Present, (void *)hk_Present);
                patch_slot((void **)&s->lpVtbl->Present1, (void *)hk_Present1);
                IDXGISwapChain1_Release(s);
            }
        }
        if (cfg.pixelate > 1 || cfg.color_bits)
            hook_factory(factory);
    } else {
        log_line("cannot create the probe swapchain; no post-process");
    }
    if (sc) IDXGISwapChain1_Release(sc);
    if (factory) IDXGIFactory2_Release(factory);
    if (queue) ID3D12CommandQueue_Release(queue);
    if (hwnd) DestroyWindow(hwnd);
}

static void hook_device(ID3D12Device *dev)
{
    if (cfg.sampler_mode != DEMAKE_SAMPLER_KEEP) {
        patch_slot((void **)&dev->lpVtbl->CreateSampler, (void *)hk_CreateSampler);
        patch_slot((void **)&dev->lpVtbl->CreateRootSignature, (void *)hk_CreateRootSignature);
        ID3D12Device11 *dev11;
        if (SUCCEEDED(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device11, (void **)&dev11))) {
            patch_slot((void **)&dev11->lpVtbl->CreateSampler2, (void *)hk_CreateSampler2);
            ID3D12Device11_Release(dev11);
        }
    }
    if (cfg.pixelate > 1 || cfg.color_bits || cfg.log)
        hook_swapchains_of(dev);
}

/* ---- exports ----------------------------------------------------------- */

typedef HRESULT(WINAPI *create_device_fn)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);

HRESULT WINAPI proxy_D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL fl, REFIID iid, void **out)
{
    create_device_fn fn = (create_device_fn)(void *)real_proc("D3D12CreateDevice");
    if (!fn)
        return E_FAIL;
    HRESULT hr = fn(adapter, fl, iid, out);
    /* out may be NULL: games call this to test support without a device. */
    if (hr == S_OK && out && *out) {
        ID3D12Device *dev;
        if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)*out, &IID_ID3D12Device, (void **)&dev))) {
            hook_device(dev);
            ID3D12Device_Release(dev);
        }
    }
    return hr;
}
