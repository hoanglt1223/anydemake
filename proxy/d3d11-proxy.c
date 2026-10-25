#define CINTERFACE
#define COBJMACROS
#include "common.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <stdlib.h>
#include <string.h>

const wchar_t potatify_dll_name[] = L"d3d11.dll";

static int render_scaling(void)
{
    return cfg.render_scale && cfg.render_scale < 100;
}

static int post_configured(void)
{
    return cfg.pixelate > 1 || cfg.color_bits;
}

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

/* Render targets the size of the screen are created smaller; the scale is
   stored on the resource so viewports can follow it when it is bound. */
static const GUID scaled_guid = {0x6f1d2b7a, 0x3c55, 0x4e0b, {0x9a, 0x61, 0x2d, 0x0e, 0x7b, 0x44, 0xa1, 0x93}};
typedef struct scale_data {
    float sx, sy;
    UINT width, height; /* what the game asked for, reported back by GetDesc */
} scale_data;
static volatile LONG screen_w, screen_h;
/* Set when the game copies between scaled and unscaled resources: new targets stay full size. */
static volatile LONG scaling_off;

static int resource_scale(ID3D11Resource *res, scale_data *out)
{
    UINT size = sizeof *out;
    return SUCCEEDED(ID3D11Resource_GetPrivateData(res, &scaled_guid, &size, out)) && size == sizeof *out;
}

typedef void(STDMETHODCALLTYPE *get_desc_fn)(ID3D11Texture2D *, D3D11_TEXTURE2D_DESC *);

/* Engines size their viewports from GetDesc; they must see the size they asked for. */
static void STDMETHODCALLTYPE hk_GetDesc(ID3D11Texture2D *tex, D3D11_TEXTURE2D_DESC *desc)
{
    ((get_desc_fn)ORIG(tex, GetDesc))(tex, desc);
    scale_data s;
    if (desc && !in_post && resource_scale((ID3D11Resource *)tex, &s)) {
        desc->Width = s.width;
        desc->Height = s.height;
    }
}

static int is_screen_target(const D3D11_TEXTURE2D_DESC *d, const D3D11_SUBRESOURCE_DATA *init)
{
    enum { SHARED_FLAGS = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
                          D3D11_RESOURCE_MISC_GDI_COMPATIBLE | D3D11_RESOURCE_MISC_SHARED_NTHANDLE };
    return render_scaling() && !scaling_off && !init && screen_w && d->Width == (UINT)screen_w && d->Height == (UINT)screen_h &&
           (d->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) &&
           !(d->BindFlags & D3D11_BIND_UNORDERED_ACCESS) && !d->CPUAccessFlags &&
           d->Usage == D3D11_USAGE_DEFAULT && !(d->MiscFlags & SHARED_FLAGS);
}

typedef HRESULT(STDMETHODCALLTYPE *create_texture2d_fn)(ID3D11Device *, const D3D11_TEXTURE2D_DESC *,
                                                        const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);

static HRESULT create_scaled_target(create_texture2d_fn orig, ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *desc,
                                    ID3D11Texture2D **out)
{
    D3D11_TEXTURE2D_DESC d = *desc;
    d.Width = desc->Width * cfg.render_scale / 100;
    d.Height = desc->Height * cfg.render_scale / 100;
    if (!d.Width) d.Width = 1;
    if (!d.Height) d.Height = 1;
    if (d.MipLevels != 1)
        d.MipLevels = 0 == d.MipLevels ? 0 : min(d.MipLevels, full_mip_count(d.Width, d.Height));
    HRESULT hr = orig(dev, &d, NULL, out);
    if (FAILED(hr) || !out)
        return orig(dev, desc, NULL, out);
    scale_data s = {(float)d.Width / desc->Width, (float)d.Height / desc->Height, desc->Width, desc->Height};
    ID3D11Texture2D_SetPrivateData(*out, &scaled_guid, sizeof s, &s);
    patch_slot((void **)&(*out)->lpVtbl->GetDesc, (void *)hk_GetDesc);
    LONG n = InterlockedIncrement(&stat_targets);
    if (n <= 20)
        log_line("render target #%ld: %ux%u -> %ux%u (format %d, bind 0x%x)", n, desc->Width, desc->Height, d.Width,
                 d.Height, desc->Format, desc->BindFlags);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_CreateTexture2D(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *desc,
                                                    const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture2D **out)
{
    create_texture2d_fn orig = (create_texture2d_fn)ORIG(dev, CreateTexture2D);
    if (!desc || in_post)
        return orig(dev, desc, init, out);
    if (is_screen_target(desc, init))
        return create_scaled_target(orig, dev, desc, out);
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

    LONG n = InterlockedIncrement(&stat_textures);
    InterlockedAdd64(&stat_texels,
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
    if (!desc || in_post)
        return orig(dev, desc, out);
    D3D11_SAMPLER_DESC d = *desc;
    d.Filter = (D3D11_FILTER)demake_filter(d.Filter, &d.MaxAnisotropy);
    return orig(dev, &d, out);
}

/* ---- viewports follow scaled targets ----------------------------------- */

/* The game's viewports and scissors per context, kept on the context as a
   private-data interface so it is freed with the context and never shared
   with a later context at the same address. */
typedef struct ctx_state {
    IUnknown iface;
    volatile LONG refs;
    float sx, sy;
    UINT nvp, nsc;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
} ctx_state;

static const GUID ctx_guid = {0x2c8a5e31, 0x77d4, 0x4f19, {0xb0, 0x3e, 0x58, 0x6a, 0x12, 0xc9, 0x40, 0xd7}};

static ULONG STDMETHODCALLTYPE st_add_ref(IUnknown *u)
{
    return (ULONG)InterlockedIncrement(&((ctx_state *)u)->refs);
}

static ULONG STDMETHODCALLTYPE st_release(IUnknown *u)
{
    LONG n = InterlockedDecrement(&((ctx_state *)u)->refs);
    if (!n)
        free(u);
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE st_query(IUnknown *u, REFIID iid, void **out)
{
    if (!IsEqualIID(iid, &IID_IUnknown)) {
        *out = NULL;
        return E_NOINTERFACE;
    }
    st_add_ref(u);
    *out = u;
    return S_OK;
}

static IUnknownVtbl st_vtbl = {st_query, st_add_ref, st_release};

/* Returns a borrowed pointer: the context holds the reference and is alive during the call. */
static ctx_state *state_for(ID3D11DeviceContext *ctx)
{
    IUnknown *u = NULL;
    UINT size = sizeof u;
    if (SUCCEEDED(ID3D11DeviceContext_GetPrivateData(ctx, &ctx_guid, &size, &u)) && u) {
        IUnknown_Release(u);
        return (ctx_state *)u;
    }
    ctx_state *st = calloc(1, sizeof *st);
    if (!st)
        return NULL;
    st->iface.lpVtbl = &st_vtbl;
    st->refs = 1;
    st->sx = st->sy = 1;
    HRESULT hr = ID3D11DeviceContext_SetPrivateDataInterface(ctx, &ctx_guid, &st->iface);
    st_release(&st->iface);
    return SUCCEEDED(hr) ? st : NULL;
}

static void STDMETHODCALLTYPE hk_RSSetViewports(ID3D11DeviceContext *ctx, UINT n, const D3D11_VIEWPORT *vp);
static void STDMETHODCALLTYPE hk_RSSetScissorRects(ID3D11DeviceContext *ctx, UINT n, const D3D11_RECT *sc);

typedef void(STDMETHODCALLTYPE *set_viewports_fn)(ID3D11DeviceContext *, UINT, const D3D11_VIEWPORT *);
typedef void(STDMETHODCALLTYPE *set_scissors_fn)(ID3D11DeviceContext *, UINT, const D3D11_RECT *);

static void apply_viewports(ID3D11DeviceContext *ctx, ctx_state *st)
{
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    for (UINT i = 0; i < st->nvp; i++) {
        vp[i] = st->vp[i];
        vp[i].TopLeftX *= st->sx;
        vp[i].Width *= st->sx;
        vp[i].TopLeftY *= st->sy;
        vp[i].Height *= st->sy;
    }
    ((set_viewports_fn)ORIG(ctx, RSSetViewports))(ctx, st->nvp, vp);
}

static void apply_scissors(ID3D11DeviceContext *ctx, ctx_state *st)
{
    D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    for (UINT i = 0; i < st->nsc; i++) {
        sc[i].left = (LONG)(st->sc[i].left * st->sx);
        sc[i].right = (LONG)(st->sc[i].right * st->sx + 0.999f);
        sc[i].top = (LONG)(st->sc[i].top * st->sy);
        sc[i].bottom = (LONG)(st->sc[i].bottom * st->sy + 0.999f);
    }
    ((set_scissors_fn)ORIG(ctx, RSSetScissorRects))(ctx, st->nsc, sc);
}

static void view_scale(ID3D11View *view, float *sx, float *sy)
{
    ID3D11Resource *res;
    ID3D11View_GetResource(view, &res);
    scale_data s;
    if (resource_scale(res, &s)) {
        *sx = s.sx;
        *sy = s.sy;
    }
    ID3D11Resource_Release(res);
}

static volatile LONG warned_mixed;

static void targets_changed(ID3D11DeviceContext *ctx, UINT n, ID3D11RenderTargetView *const *rtvs,
                            ID3D11DepthStencilView *dsv)
{
    ctx_state *st = state_for(ctx);
    if (!st)
        return;
    float sx = 1, sy = 1;
    ID3D11View *first = NULL;
    for (UINT i = 0; i < n && rtvs && !first; i++)
        first = (ID3D11View *)rtvs[i];
    if (first)
        view_scale(first, &sx, &sy);
    if (dsv) {
        float dx = 1, dy = 1;
        view_scale((ID3D11View *)dsv, &dx, &dy);
        if (!first) {
            sx = dx;
            sy = dy;
        } else if ((dx != sx || dy != sy) && !InterlockedExchange(&warned_mixed, 1)) {
            log_line("a scaled and a full-size target are bound together; set the render resolution to Full "
                     "if the picture is broken");
        }
    }
    if (sx == st->sx && sy == st->sy)
        return;
    st->sx = sx;
    st->sy = sy;
    if (st->nvp)
        apply_viewports(ctx, st);
    if (st->nsc)
        apply_scissors(ctx, st);
}

typedef void(STDMETHODCALLTYPE *om_set_fn)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *,
                                           ID3D11DepthStencilView *);
typedef void(STDMETHODCALLTYPE *om_set_uav_fn)(ID3D11DeviceContext *, UINT, ID3D11RenderTargetView *const *,
                                               ID3D11DepthStencilView *, UINT, UINT,
                                               ID3D11UnorderedAccessView *const *, const UINT *);
typedef void(STDMETHODCALLTYPE *get_viewports_fn)(ID3D11DeviceContext *, UINT *, D3D11_VIEWPORT *);
typedef void(STDMETHODCALLTYPE *get_scissors_fn)(ID3D11DeviceContext *, UINT *, D3D11_RECT *);
typedef void(STDMETHODCALLTYPE *clear_state_fn)(ID3D11DeviceContext *);

static void STDMETHODCALLTYPE hk_OMSetRenderTargets(ID3D11DeviceContext *ctx, UINT n,
                                                    ID3D11RenderTargetView *const *rtvs, ID3D11DepthStencilView *dsv)
{
    ((om_set_fn)ORIG(ctx, OMSetRenderTargets))(ctx, n, rtvs, dsv);
    if (!in_post)
        targets_changed(ctx, n, rtvs, dsv);
}

static void STDMETHODCALLTYPE hk_OMSetRenderTargetsAndUnorderedAccessViews(
    ID3D11DeviceContext *ctx, UINT n, ID3D11RenderTargetView *const *rtvs, ID3D11DepthStencilView *dsv, UINT uav_start,
    UINT nuav, ID3D11UnorderedAccessView *const *uavs, const UINT *counts)
{
    ((om_set_uav_fn)ORIG(ctx, OMSetRenderTargetsAndUnorderedAccessViews))(ctx, n, rtvs, dsv, uav_start, nuav, uavs,
                                                                           counts);
    if (!in_post && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
        targets_changed(ctx, n, rtvs, dsv);
}

static void STDMETHODCALLTYPE hk_RSSetViewports(ID3D11DeviceContext *ctx, UINT n, const D3D11_VIEWPORT *vp)
{
    ctx_state *st = in_post ? NULL : state_for(ctx);
    if (!st || !vp || n > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) {
        ((set_viewports_fn)ORIG(ctx, RSSetViewports))(ctx, n, vp);
        return;
    }
    st->nvp = n;
    memcpy(st->vp, vp, n * sizeof *vp);
    apply_viewports(ctx, st);
}

static void STDMETHODCALLTYPE hk_RSSetScissorRects(ID3D11DeviceContext *ctx, UINT n, const D3D11_RECT *sc)
{
    ctx_state *st = in_post ? NULL : state_for(ctx);
    if (!st || !sc || n > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) {
        ((set_scissors_fn)ORIG(ctx, RSSetScissorRects))(ctx, n, sc);
        return;
    }
    st->nsc = n;
    memcpy(st->sc, sc, n * sizeof *sc);
    apply_scissors(ctx, st);
}

/* Games read back what they set, not the scaled values. */
static void STDMETHODCALLTYPE hk_RSGetViewports(ID3D11DeviceContext *ctx, UINT *n, D3D11_VIEWPORT *vp)
{
    ctx_state *st = state_for(ctx);
    if (!st || !n) {
        ((get_viewports_fn)ORIG(ctx, RSGetViewports))(ctx, n, vp);
        return;
    }
    if (vp)
        memcpy(vp, st->vp, min(*n, st->nvp) * sizeof *vp);
    *n = vp ? min(*n, st->nvp) : st->nvp;
}

static void STDMETHODCALLTYPE hk_RSGetScissorRects(ID3D11DeviceContext *ctx, UINT *n, D3D11_RECT *sc)
{
    ctx_state *st = state_for(ctx);
    if (!st || !n) {
        ((get_scissors_fn)ORIG(ctx, RSGetScissorRects))(ctx, n, sc);
        return;
    }
    if (sc)
        memcpy(sc, st->sc, min(*n, st->nsc) * sizeof *sc);
    *n = sc ? min(*n, st->nsc) : st->nsc;
}

static void STDMETHODCALLTYPE hk_ClearState(ID3D11DeviceContext *ctx)
{
    ((clear_state_fn)ORIG(ctx, ClearState))(ctx);
    ctx_state *st = state_for(ctx);
    if (st) {
        st->sx = st->sy = 1;
        st->nvp = st->nsc = 0;
    }
}

typedef void(STDMETHODCALLTYPE *copy_resource_fn)(ID3D11DeviceContext *, ID3D11Resource *, ID3D11Resource *);
typedef void(STDMETHODCALLTYPE *resolve_fn)(ID3D11DeviceContext *, ID3D11Resource *, UINT, ID3D11Resource *, UINT,
                                            DXGI_FORMAT);

static void check_copy(ID3D11Resource *dst, ID3D11Resource *src)
{
    scale_data a, b;
    int sa = dst && resource_scale(dst, &a), sb = src && resource_scale(src, &b);
    if (sa != sb && !InterlockedExchange(&scaling_off, 1))
        log_line("the game copies between a scaled and a full-size target; render scaling is off for new targets "
                 "(set the render resolution to Full for this game)");
}

static void STDMETHODCALLTYPE hk_CopyResource(ID3D11DeviceContext *ctx, ID3D11Resource *dst, ID3D11Resource *src)
{
    if (!in_post)
        check_copy(dst, src);
    ((copy_resource_fn)ORIG(ctx, CopyResource))(ctx, dst, src);
}

static void STDMETHODCALLTYPE hk_ResolveSubresource(ID3D11DeviceContext *ctx, ID3D11Resource *dst, UINT dsub,
                                                    ID3D11Resource *src, UINT ssub, DXGI_FORMAT format)
{
    if (!in_post)
        check_copy(dst, src);
    ((resolve_fn)ORIG(ctx, ResolveSubresource))(ctx, dst, dsub, src, ssub, format);
}

static void hook_context(ID3D11DeviceContext *ctx)
{
    patch_slot((void **)&ctx->lpVtbl->CopyResource, (void *)hk_CopyResource);
    patch_slot((void **)&ctx->lpVtbl->ResolveSubresource, (void *)hk_ResolveSubresource);
    patch_slot((void **)&ctx->lpVtbl->OMSetRenderTargets, (void *)hk_OMSetRenderTargets);
    patch_slot((void **)&ctx->lpVtbl->OMSetRenderTargetsAndUnorderedAccessViews,
               (void *)hk_OMSetRenderTargetsAndUnorderedAccessViews);
    patch_slot((void **)&ctx->lpVtbl->RSSetViewports, (void *)hk_RSSetViewports);
    patch_slot((void **)&ctx->lpVtbl->RSSetScissorRects, (void *)hk_RSSetScissorRects);
    patch_slot((void **)&ctx->lpVtbl->RSGetViewports, (void *)hk_RSGetViewports);
    patch_slot((void **)&ctx->lpVtbl->RSGetScissorRects, (void *)hk_RSGetScissorRects);
    patch_slot((void **)&ctx->lpVtbl->ClearState, (void *)hk_ClearState);
}

/* ---- present-time post-process ----------------------------------------- */

typedef struct post_state {
    ID3D11Device *dev; /* identity only; the shaders below keep it alive */
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Buffer *cb;
    ID3DDeviceContextState *ctx_state;
    ID3D11Texture2D *copy;
    D3D11_TEXTURE2D_DESC copy_desc;
    UINT64 area; /* back buffer size of the device that owns the post-process */
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
    HRESULT hr = fn(post_hlsl, post_hlsl_len, "potatify", NULL, NULL, entry, target, 0, 0, blob, &err);
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

/* Runs with in_post set, so the hooks above pass these calls straight through. */
static void post_process(IDXGISwapChain *sc)
{
    ID3D11Texture2D *bb = NULL;
    ID3D11Device *dev = NULL;
    if (FAILED(IDXGISwapChain_GetDevice(sc, &IID_ID3D11Device, (void **)&dev)))
        return; /* a D3D12 swapchain; the d3d12.dll proxy handles it */
    if (FAILED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb)))
        goto out;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(bb, &d);
    EnterCriticalSection(&hook_lock);
    /* A second device (launcher overlay, video player) must not make the
       state rebuild every frame; the biggest swapchain wins. */
    if (post.dev && post.dev != dev && (UINT64)d.Width * d.Height <= post.area)
        goto unlock;
    if (!post_setup(dev))
        goto unlock;
    post.area = (UINT64)d.Width * d.Height;
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
    LeaveCriticalSection(&hook_lock);
out:
    if (bb) ID3D11Texture2D_Release(bb);
    ID3D11Device_Release(dev);
}

typedef HRESULT(STDMETHODCALLTYPE *present_fn)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *present1_fn)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
typedef HRESULT(STDMETHODCALLTYPE *get_buffer_fn)(IDXGISwapChain *, UINT, REFIID, void **);

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

static void note_screen_size(IDXGISwapChain *sc)
{
    DXGI_SWAP_CHAIN_DESC d;
    if (SUCCEEDED(IDXGISwapChain_GetDesc(sc, &d)) &&
        ((LONG)d.BufferDesc.Width != screen_w || (LONG)d.BufferDesc.Height != screen_h)) {
        InterlockedExchange(&screen_w, d.BufferDesc.Width);
        InterlockedExchange(&screen_h, d.BufferDesc.Height);
        log_line("screen %ux%u", d.BufferDesc.Width, d.BufferDesc.Height);
    }
}

/* Games fetch back buffer 0 right after creating or resizing the swapchain
   and before creating their own screen-sized targets. */
static HRESULT STDMETHODCALLTYPE hk_GetBuffer(IDXGISwapChain *sc, UINT index, REFIID iid, void **out)
{
    HRESULT hr = ((get_buffer_fn)ORIG(sc, GetBuffer))(sc, index, iid, out);
    if (SUCCEEDED(hr) && !in_post)
        note_screen_size(sc);
    return hr;
}

typedef HRESULT(STDMETHODCALLTYPE *resize_buffers_fn)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static HRESULT STDMETHODCALLTYPE hk_ResizeBuffers(IDXGISwapChain *sc, UINT count, UINT w, UINT h, DXGI_FORMAT format,
                                                  UINT flags)
{
    HRESULT hr = ((resize_buffers_fn)ORIG(sc, ResizeBuffers))(sc, count, w, h, format, flags);
    if (SUCCEEDED(hr))
        note_screen_size(sc);
    return hr;
}

static void hook_swapchain(IDXGISwapChain *sc)
{
    if (render_scaling()) {
        patch_slot((void **)&sc->lpVtbl->GetBuffer, (void *)hk_GetBuffer);
        patch_slot((void **)&sc->lpVtbl->ResizeBuffers, (void *)hk_ResizeBuffers);
    }
    if (!post_configured() && !cfg.log)
        return;
    patch_slot((void **)&sc->lpVtbl->Present, (void *)hk_Present);
    IDXGISwapChain1 *sc1;
    if (SUCCEEDED(IDXGISwapChain_QueryInterface(sc, &IID_IDXGISwapChain1, (void **)&sc1))) {
        patch_slot((void **)&sc1->lpVtbl->Present1, (void *)hk_Present1);
        IDXGISwapChain1_Release(sc1);
    }
}

/* Most games create their swapchain through IDXGIFactory, not through this
   DLL, so the swapchain slots are found on throwaway swapchains instead. */
static void hook_swapchains_of(ID3D11Device *dev)
{
    IDXGIDevice *dxgi_dev = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory *factory = NULL;
    HWND hwnd = hidden_window();
    if (hwnd && SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxgi_dev)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_dev, &adapter)) &&
        SUCCEEDED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&factory))) {
        stats_adapter(adapter);
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
            IDXGISwapChain *sc;
            if (SUCCEEDED(IDXGIFactory_CreateSwapChain(factory, (IUnknown *)dev, &sd, &sc))) {
                hook_swapchain(sc);
                IDXGISwapChain_Release(sc);
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

typedef HRESULT(STDMETHODCALLTYPE *create_deferred_fn)(ID3D11Device *, UINT, ID3D11DeviceContext **);

static HRESULT STDMETHODCALLTYPE hk_CreateDeferredContext(ID3D11Device *dev, UINT flags, ID3D11DeviceContext **out)
{
    HRESULT hr = ((create_deferred_fn)ORIG(dev, CreateDeferredContext))(dev, flags, out);
    if (SUCCEEDED(hr) && out && *out)
        hook_context(*out);
    return hr;
}

static void hook_device(ID3D11Device *dev, int find_swapchains)
{
    if (!dev)
        return;
    if (cfg.mip_skip || render_scaling())
        patch_slot((void **)&dev->lpVtbl->CreateTexture2D, (void *)hk_CreateTexture2D);
    if (cfg.sampler_mode != DEMAKE_SAMPLER_KEEP)
        patch_slot((void **)&dev->lpVtbl->CreateSamplerState, (void *)hk_CreateSamplerState);
    if (render_scaling()) {
        ID3D11DeviceContext *ctx;
        ID3D11Device_GetImmediateContext(dev, &ctx);
        hook_context(ctx);
        ID3D11DeviceContext_Release(ctx);
        patch_slot((void **)&dev->lpVtbl->CreateDeferredContext, (void *)hk_CreateDeferredContext);
    }
    if (find_swapchains && (post_configured() || render_scaling() || cfg.log))
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

HRESULT WINAPI proxy_D3D11CreateDevice(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                                       const D3D_FEATURE_LEVEL *levels, UINT nlevels, UINT sdk,
                                       ID3D11Device **dev, D3D_FEATURE_LEVEL *fl, ID3D11DeviceContext **ctx)
{
    create_device_fn fn = (create_device_fn)(void *)real_proc("D3D11CreateDevice");
    if (!fn)
        return E_FAIL;
    HRESULT hr = fn(adapter, type, sw, flags, levels, nlevels, sdk, dev, fl, ctx);
    if (SUCCEEDED(hr) && dev)
        hook_device(*dev, 1);
    return hr;
}

HRESULT WINAPI proxy_D3D11CreateDeviceAndSwapChain(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE sw,
                                                   UINT flags, const D3D_FEATURE_LEVEL *levels, UINT nlevels,
                                                   UINT sdk, const DXGI_SWAP_CHAIN_DESC *sd, IDXGISwapChain **sc,
                                                   ID3D11Device **dev, D3D_FEATURE_LEVEL *fl,
                                                   ID3D11DeviceContext **ctx)
{
    create_device_sc_fn fn = (create_device_sc_fn)(void *)real_proc("D3D11CreateDeviceAndSwapChain");
    if (!fn)
        return E_FAIL;
    HRESULT hr = fn(adapter, type, sw, flags, levels, nlevels, sdk, sd, sc, dev, fl, ctx);
    if (SUCCEEDED(hr)) {
        if (dev)
            hook_device(*dev, 1);
        if (sc && *sc) {
            hook_swapchain(*sc);
            note_screen_size(*sc);
        }
    }
    return hr;
}

/* D3D12 games use this for UI layers; only sampler changes apply there. */
HRESULT WINAPI proxy_D3D11On12CreateDevice(IUnknown *d12, UINT flags, const D3D_FEATURE_LEVEL *levels,
                                           UINT nlevels, IUnknown *const *queues, UINT nqueues, UINT node,
                                           ID3D11Device **dev, ID3D11DeviceContext **ctx, D3D_FEATURE_LEVEL *fl)
{
    on12_fn fn = (on12_fn)(void *)real_proc("D3D11On12CreateDevice");
    if (!fn)
        return E_FAIL;
    HRESULT hr = fn(d12, flags, levels, nlevels, queues, nqueues, node, dev, ctx, fl);
    if (SUCCEEDED(hr) && dev && *dev && cfg.sampler_mode != DEMAKE_SAMPLER_KEEP)
        patch_slot((void **)&(*dev)->lpVtbl->CreateSamplerState, (void *)hk_CreateSamplerState);
    return hr;
}
