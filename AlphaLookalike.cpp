// alpha-lookalike: an Alpha-inspired colour grade for Cube World (Steam,
// 64-bit, D3D11). Original world-only grading, made with AI assistance.
// No ReShade integration.
//
// Like TemporalAA, it shadows 64-byte constant buffers (Map/Unmap/Update hooks)
// to recognise native world draws, remembers the scene render target, and when
// the first orthographic (HUD) draw arrives it grades the finished 3D scene in
// place, so the HUD itself is never tinted. TemporalAA calls
// FinishAlphaLookalike() so the grade happens before its resolve.
//
// Distance fog (Alpha/Omega-like): the scene depth and native projection give
// each pixel's view distance; distant world fades toward the sky's own colour,
// from FogStartPercent to FogEndPercent of the draw distance. It follows the
// camera, so it recedes as you walk; the sky itself is never fogged.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "cwsdk.h"
#include "NativeScene.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
namespace palette {
template <class T> void drop(T *&p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// ===========================================================================
// Hooked ID3D11DeviceContext methods and constant-buffer shadow copies
// ===========================================================================

// ID3D11DeviceContext vtable slots.
constexpr UINT SLOT_DRAW_INDEXED = 12, SLOT_DRAW = 13, SLOT_MAP = 14, SLOT_UNMAP = 15,
               SLOT_UPDATE_SUBRESOURCE = 48;

using DI = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT, INT);
using D = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, UINT, UINT);
using Map = HRESULT(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP,
                                         UINT, D3D11_MAPPED_SUBRESOURCE *);
using Unmap = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
using Update = void(STDMETHODCALLTYPE *)(ID3D11DeviceContext *, ID3D11Resource *, UINT,
                                         const D3D11_BOX *, const void *, UINT, UINT);
static Map oldMap;
static Unmap oldUnmap;
static Update oldUpdate;
static void **table = nullptr; // Hooked context vtable (set once attached).
// Private-data key for the shadow copy of each 64-byte constant buffer.
static const GUID cacheGuid = {
    0xa281de67, 0x9d35, 0x4ef2, {0xa1, 0x25, 0x54, 0x18, 0x81, 0x27, 0x22, 0x93}};
struct Mapped {
    void *data;
    UINT size;
};
static thread_local std::unordered_map<ID3D11Resource *, Mapped> mapped; // Currently mapped.
static void log(const char *s) {
    FILE *f = nullptr;
    fopen_s(&f, "Mods\\alpha-lookalike.log", "a");
    if (f) {
        fprintf(f, "%s\n", s);
        fclose(f);
    }
}
// 64 if `r` is a 64-byte constant buffer (a single matrix), otherwise 0.
static UINT bytes(ID3D11Resource *r) {
    ID3D11Buffer *b = nullptr;
    if (FAILED(r->QueryInterface(__uuidof(ID3D11Buffer), (void **)&b)))
        return 0;
    D3D11_BUFFER_DESC d{};
    b->GetDesc(&d);
    b->Release();
    return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && d.ByteWidth == 64 ? 64 : 0;
}
// Reads the shadow copy of a constant buffer's contents.
static bool readBuffer(ID3D11Buffer *b, void *out, UINT n) {
    UINT got = n;
    return b && SUCCEEDED(b->GetPrivateData(cacheGuid, &got, out)) && got == n;
}

// ===========================================================================
// State
// ===========================================================================

static bool enabled = true, internal = false, finished = false;
// Settings (see alpha-lookalike.ini).
static float saturation = .90f, cool = .04f;
static bool fogEnabled = true;
static float fogStart = .08f, fogEnd = .55f, fogStrength = .90f; // Fractions of draw distance.
// Culling: fog is opaque at `fogSolid` (fraction of draw distance) and native
// world draws wholly beyond it are skipped.
static bool fogCull = true;
static float fogSolid = .70f;
static float solidDistance = 0; // this frame, in view units; 0 = no culling
static unsigned culledDraws = 0, worldDraws = 0, cullFrames = 0;
static float fogColor[3] = {40 / 255.f, 70 / 255.f, 150 / 255.f}; // When no sky is visible.
static ID3D11Device *device = nullptr;
static ID3D11DeviceContext *context = nullptr;
static ID3D11DeviceContext1 *context1 = nullptr;
static ID3DDeviceContextState *state = nullptr; // Isolated pipeline state for our pass.
static ID3D11Texture2D *copy = nullptr;         // This frame's scene, as shader input.
static ID3D11ShaderResourceView *view = nullptr;
static ID3D11RenderTargetView *target = nullptr; // The scene target of this frame.
static ID3D11DepthStencilView *sceneDepth = nullptr; // ...and its depth buffer.
static float sceneProjection[16]{};                  // Native projection of the world.
static bool projectionKnown = false;
static ID3D11Texture2D *depthCopy = nullptr; // Readable copy of the scene depth.
static ID3D11ShaderResourceView *depthView = nullptr;
// The scene depth itself when it is already shader-readable (ReShade, or
// TemporalAA's readable-depth hook): read in place, no copy.
static ID3D11Texture2D *depthSource = nullptr;
static ID3D11ShaderResourceView *depthSourceView = nullptr;
static ID3D11ShaderResourceView *depthInput = nullptr; // This frame's fog depth.
static bool depthMultisampled = false;
static ID3D11PixelShader *psMsaa = nullptr; // ps with MSAA_DEPTH.
// Fog sky colour history: two 1x1 textures used alternately; skyPass reads one
// and writes the other each fogged frame.
static ID3D11PixelShader *skyPS = nullptr, *skyPSMsaa = nullptr;
static ID3D11Texture2D *skyTexture[2] = {};
static ID3D11ShaderResourceView *skyView[2] = {};
static ID3D11RenderTargetView *skyTarget[2] = {};
static int skyCurrent = 0;
static bool skyValid = false;
constexpr float SKY_BLEND = .15f; // share of the new sky colour per frame
static DXGI_FORMAT depthFamily = DXGI_FORMAT_UNKNOWN;
static D3D11_VIEWPORT viewport{};
static ID3D11VertexShader *vs = nullptr;
static ID3D11PixelShader *ps = nullptr;
static ID3D11Buffer *params = nullptr;
static ID3D11RasterizerState *raster = nullptr;
static DI oldDI = nullptr;
static D oldD = nullptr;
static UINT width = 0, height = 0;
static DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

// vs: full-screen triangle from SV_VertexID. ps: the colour grade, then fog.
static const char *code = R"(
cbuffer Grade:register(b0){
    float saturation,cool,fog,fogStrength;
    float4 fogColor;        // rgb: used when no sky pixel is visible.
    float4 depthToDistance; // Native projection P[10], P[11], P[14], P[15].
    float fogStart,fogEnd,skyHistoryValid,skyBlend;
    float4 solid;           // x: distance where fog becomes opaque, y: 1 when used
};
Texture2D<float4> scene:register(t0);
// The fog's sky colour: one texel, updated once per frame by skyPass.
Texture2D<float4> skyHistory:register(t2);
// Scene depth: multisampled when the game renders with MSAA (sample 0 is
// used; the grade reads the resolved colour), single-sample otherwise.
#ifdef MSAA_DEPTH
Texture2DMS<float> depth:register(t1);
float depthAt(int2 q){return depth.Load(q,0);}
#else
Texture2D<float> depth:register(t1);
float depthAt(int2 q){return depth.Load(int3(q,0));}
#endif

float4 vs(uint id:SV_VertexID):SV_POSITION{
    float2 p=float2((id<<1)&2,id&2);
    return float4(p*float2(2,-2)+float2(-1,1),0,1);
}

float3 grade(float3 original){
    float l=dot(original,float3(.2126,.7152,.0722));
    float3 c=lerp(l.xxx,original,saturation);
    // References have mint-green highlights, teal shade and warm golden paths.
    float green=saturate((original.g-max(original.r,original.b))*3);
    float highlight=smoothstep(.25,.75,l);
    c.r+=green*highlight*.065;
    c.b+=green*(.025+.035*(1-highlight));
    float warm=saturate((original.r-original.b)*2)*saturate((original.g-original.b)*2);
    c.r+=warm*highlight*.025;
    c.g+=warm*highlight*.035;
    // Modest cooler palette, stronger in shade; no exposure/gamma lift at night.
    float shade=1-smoothstep(.15,.8,l);
    c*=float3(1-cool*.6,1,1+cool*(.6+.4*shade));
    return saturate(c);
}
// The graded colour of the visible sky (depth 1) along the top of the screen,
// so fog matches day, night and weather. Computed once per frame into a single
// texel and eased over a few frames: the probe points flipping between sky and
// terrain as the camera moves must not make the fog colour jump. When no sky
// is visible the previous colour is kept (the configured colour until a sky
// has been seen).
float4 skyPass(float4 p:SV_POSITION):SV_TARGET{
    uint w,h;
    scene.GetDimensions(w,h);
    float3 sum=0;
    float count=0;
    [unroll]for(int i=0;i<8;i++){
        int2 q=int2((i+.5)*w/8,h*((i&1)?.06:.16));
        if(depthAt(q)>=1){
            sum+=grade(scene.Load(int3(q,0)).rgb);
            count+=1;
        }
    }
    float3 previous=skyHistoryValid>.5?skyHistory.Load(int3(0,0,0)).rgb:fogColor.rgb;
    if(count==0)
        return float4(previous,1);
    float3 current=sum/count;
    return float4(skyHistoryValid>.5?lerp(previous,current,skyBlend):current,1);
}
float4 ps(float4 p:SV_POSITION):SV_TARGET{
    float4 original=scene.Load(int3(p.xy,0));
    float3 c=grade(original.rgb);
    [branch]if(fog>.5){
        float d=depthAt(int2(p.xy));
        float denominator=d*depthToDistance.z-depthToDistance.x;
        if(d<1&&abs(denominator)>1e-8){
            // Linear view distance from the depth and the native projection.
            float distance=abs((depthToDistance.y-d*depthToDistance.w)/denominator);
            float amount=smoothstep(fogStart,fogEnd,distance)*fogStrength;
            // With culling, the fog becomes opaque at solid.x, so anything that
            // lies wholly beyond it is skipped without a visible change.
            if(solid.y>.5)
                amount=lerp(amount,1,saturate((distance-fogEnd)/max(solid.x-fogEnd,1e-3)));
            c=lerp(c,skyHistory.Load(int3(0,0,0)).rgb,amount);
        }
    }
    return float4(c,original.a);
})";

static bool fogConstants(float *out);
// Called once per presented frame.
static void reset() {
    if (++cullFrames % 3600 == 0 && worldDraws) {
        char s[160];
        sprintf_s(s, "Fog culling: %.1f%% of world draws skipped beyond the opaque fog (%u of %u "
                     "over 3600 frames).",
                  100. * culledDraws / worldDraws, culledDraws, worldDraws);
        log(s);
        culledDraws = worldDraws = 0;
    }
    solidDistance = 0; // Re-enable only after this frame's world camera is captured.
    drop(target);
    drop(sceneDepth);
    projectionKnown = false;
    finished = false;
}

// Creates the size-independent resources on first use (for context `c`).
static bool initialize(ID3D11DeviceContext *c) {
    if (device)
        return context == c && vs && ps && params && state && raster;
    context = c;
    context->AddRef();
    c->GetDevice(&device);
    ID3D11Device1 *d1 = nullptr;
    bool ok = SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device1), (void **)&d1)) &&
              SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), (void **)&context1));
    auto level = device->GetFeatureLevel();
    if (ok)
        ok = SUCCEEDED(d1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
                                                    __uuidof(ID3D11Device), nullptr, &state));
    drop(d1);
    static const D3D_SHADER_MACRO msaaDepth[] = {{"MSAA_DEPTH", "1"}, {nullptr, nullptr}};
    for (int i = 0; i < 5 && ok; i++) {
        bool pixel = i > 0, msaa = i == 2 || i == 4;
        ID3DBlob *b = nullptr, *error = nullptr;
        // Texture2DMS loads need shader model 4.1.
        ok = SUCCEEDED(D3DCompile(code, strlen(code), "NativePalette", msaa ? msaaDepth : nullptr,
                                  nullptr, !pixel ? "vs" : i >= 3 ? "skyPass" : "ps",
                                  msaa ? "ps_4_1" : pixel ? "ps_4_0" : "vs_4_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &error));
        if (error)
            log((char *)error->GetBufferPointer());
        if (ok)
            ok = SUCCEEDED(pixel ? device->CreatePixelShader(b->GetBufferPointer(),
                                                             b->GetBufferSize(), nullptr,
                                                             i == 1   ? &ps
                                                             : i == 2 ? &psMsaa
                                                             : i == 3 ? &skyPS
                                                                      : &skyPSMsaa)
                                 : device->CreateVertexShader(b->GetBufferPointer(),
                                                              b->GetBufferSize(), nullptr, &vs));
        drop(b);
        drop(error);
    }
    D3D11_TEXTURE2D_DESC skyDesc{1, 1, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0},
                                 D3D11_USAGE_DEFAULT,
                                 D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
    for (int i = 0; i < 2 && ok; i++)
        ok = SUCCEEDED(device->CreateTexture2D(&skyDesc, nullptr, &skyTexture[i])) &&
             SUCCEEDED(device->CreateShaderResourceView(skyTexture[i], nullptr, &skyView[i])) &&
             SUCCEEDED(device->CreateRenderTargetView(skyTexture[i], nullptr, &skyTarget[i]));
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 80;
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (ok)
        ok = SUCCEEDED(device->CreateBuffer(&cb, nullptr, &params));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (ok)
        ok = SUCCEEDED(device->CreateRasterizerState(&rd, &raster));
    return ok;
}

// (Re)creates the scene copy for the target's size/format.
static bool texture(ID3D11Texture2D *source, DXGI_FORMAT typed) {
    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    if (copy && width == d.Width && height == d.Height && format == typed)
        return true;
    drop(view);
    drop(copy);
    width = d.Width;
    height = d.Height;
    format = typed;
    d.Format = typed;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    return SUCCEEDED(device->CreateTexture2D(&d, nullptr, &copy)) &&
           SUCCEEDED(device->CreateShaderResourceView(copy, nullptr, &view));
}

// Makes this frame's scene depth readable for the fog: in place when the depth
// texture is shader-readable, otherwise a copy (single-sample or MSAA). Sets
// depthInput/depthMultisampled. Unsupported sizes/formats leave fog off.
static bool copyDepth(UINT w, UINT h) {
    depthInput = nullptr;
    if (!sceneDepth)
        return false;
    ID3D11Resource *resource = nullptr;
    sceneDepth->GetResource(&resource);
    ID3D11Texture2D *source = nullptr;
    bool ok = SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&source));
    drop(resource);
    if (!ok)
        return false;
    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    DXGI_FORMAT typeless = DXGI_FORMAT_UNKNOWN, readable = DXGI_FORMAT_UNKNOWN;
    switch (d.Format) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        typeless = DXGI_FORMAT_R24G8_TYPELESS;
        readable = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        break;
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        typeless = DXGI_FORMAT_R32_TYPELESS;
        readable = DXGI_FORMAT_R32_FLOAT;
        break;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        typeless = DXGI_FORMAT_R32G8X24_TYPELESS;
        readable = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        break;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        typeless = DXGI_FORMAT_R16_TYPELESS;
        readable = DXGI_FORMAT_R16_UNORM;
        break;
    default:
        break;
    }
    if (typeless == DXGI_FORMAT_UNKNOWN || d.Width != w || d.Height != h || d.ArraySize != 1 ||
        d.MipLevels != 1) {
        drop(source);
        return false;
    }
    depthMultisampled = d.SampleDesc.Count > 1;
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = readable;
    v.ViewDimension =
        depthMultisampled ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    v.Texture2D.MipLevels = 1;
    if (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
        if (source != depthSource) {
            drop(depthSourceView);
            drop(depthSource);
            if (FAILED(device->CreateShaderResourceView(source, &v, &depthSourceView))) {
                drop(source);
                return false;
            }
            depthSource = source;
            source->AddRef();
        }
        depthInput = depthSourceView;
        drop(source);
        return true;
    }
    D3D11_TEXTURE2D_DESC existing{};
    if (depthCopy)
        depthCopy->GetDesc(&existing);
    if (!depthCopy || existing.Width != w || existing.Height != h || depthFamily != typeless ||
        existing.SampleDesc.Count != d.SampleDesc.Count ||
        existing.SampleDesc.Quality != d.SampleDesc.Quality) {
        drop(depthView);
        drop(depthCopy);
        d.Format = typeless;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = d.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &depthCopy)) ||
            FAILED(device->CreateShaderResourceView(depthCopy, &v, &depthView))) {
            drop(depthView);
            drop(depthCopy);
            drop(source);
            return false;
        }
        depthFamily = typeless;
    }
    context->CopyResource(depthCopy, source);
    depthInput = depthView;
    drop(source);
    return true;
}
// Fog constants from the native projection: P[10], P[11], P[14], P[15] turn a
// depth value into view distance; the draw distance is its value at depth 1.
static bool fogConstants(float *out) {
    if (!fogEnabled || !projectionKnown || fogStrength <= 0)
        return false;
    const float *p = sceneProjection;
    float drawDistance = (p[11] - p[15]) / (p[14] - p[10]);
    if (!std::isfinite(drawDistance) || fabsf(drawDistance) < 1e-3f)
        return false;
    drawDistance = fabsf(drawDistance);
    out[0] = p[10];
    out[1] = p[11];
    out[2] = p[14];
    out[3] = p[15];
    out[4] = fogStart * drawDistance;
    out[5] = std::max(fogEnd * drawDistance, out[4] + 1e-3f);
    // Opaque fog (and culling beyond it) only at full strength settings that reach it.
    out[6] = fogCull ? std::max(fogSolid * drawDistance, out[5] + 1e-3f) : 0;
    return true;
}

// Grades the scene in `target` once per frame, in an isolated context state.
static bool apply() {
    if (internal || finished || !target || !initialize(context))
        return false;
    ID3D11Resource *resource = nullptr;
    target->GetResource(&resource);
    ID3D11Texture2D *source = nullptr;
    bool ok = SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&source));
    drop(resource);
    D3D11_RENDER_TARGET_VIEW_DESC rt{};
    target->GetDesc(&rt);
    D3D11_TEXTURE2D_DESC td{};
    if (source)
        source->GetDesc(&td);
    // Only ordinary full-size 2D scene targets; do not affect arrays/mips/offscreen previews.
    ok = ok &&
         (rt.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D ||
          rt.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMS) &&
         td.ArraySize == 1 && td.MipLevels == 1 && viewport.TopLeftX == 0 &&
         viewport.TopLeftY == 0 && viewport.Width == td.Width && viewport.Height == td.Height;
    if (ok)
        ok = texture(source, rt.Format);
    if (!ok) {
        drop(source);
        return false;
    }
    internal = true;
    ID3DDeviceContextState *saved = nullptr;
    context1->SwapDeviceContextState(state, &saved);
    // Copy (or MSAA-resolve) the scene so it can be read while writing `target`.
    if (td.SampleDesc.Count > 1)
        context->ResolveSubresource(copy, 0, source, 0, rt.Format);
    else
        context->CopyResource(copy, source);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(nullptr, nullptr, ~0u);
    context->OMSetDepthStencilState(nullptr, 0);
    context->RSSetState(raster);
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    context->VSSetShader(vs, nullptr, 0);
    float fog[7]{};
    bool fogged = fogConstants(fog) && copyDepth(td.Width, td.Height);
    float values[20] = {saturation, cool, fogged ? 1.f : 0.f, fogStrength,
                        fogColor[0], fogColor[1], fogColor[2], 1,
                        fog[0], fog[1], fog[2], fog[3],
                        fog[4], fog[5], skyValid ? 1.f : 0.f, SKY_BLEND,
                        fog[6], fog[6] > 0 ? 1.f : 0.f, 0, 0};
    context->UpdateSubresource(params, 0, nullptr, values, 0, 0);
    context->PSSetConstantBuffers(0, 1, &params);
    ID3D11ShaderResourceView *empty[3] = {};
    if (fogged) {
        // 1. Update the fog's sky colour (one texel) from this frame's sky.
        int next = 1 - skyCurrent;
        D3D11_VIEWPORT one{0, 0, 1, 1, 0, 1};
        context->OMSetRenderTargets(1, &skyTarget[next], nullptr);
        context->RSSetViewports(1, &one);
        context->PSSetShader(depthMultisampled ? skyPSMsaa : skyPS, nullptr, 0);
        ID3D11ShaderResourceView *skyInputs[3] = {view, depthInput, skyView[skyCurrent]};
        context->PSSetShaderResources(0, 3, skyInputs);
        oldD(context, 3, 0);
        context->PSSetShaderResources(0, 3, empty);
        skyCurrent = next;
        skyValid = true;
        context->OMSetRenderTargets(1, &target, nullptr);
        context->RSSetViewports(1, &viewport);
    }
    // 2. Grade (and fog) the scene.
    context->PSSetShader(fogged && depthMultisampled ? psMsaa : ps, nullptr, 0);
    ID3D11ShaderResourceView *inputs[3] = {view, fogged ? depthInput : nullptr,
                                           fogged ? skyView[skyCurrent] : nullptr};
    context->PSSetShaderResources(0, 3, inputs);
    oldD(context, 3, 0); // Full-screen triangle.
    context->PSSetShaderResources(0, 3, empty);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context1->SwapDeviceContextState(saved, nullptr);
    drop(saved);
    drop(source);
    drop(target);
    internal = false;
    finished = true;
    return true;
}

// Native world draws bind six vertex constant buffers of these sizes.
static bool hasNativeWorldBuffers(ID3D11DeviceContext *c) {
    ID3D11Buffer *worldBuffers[6]{};
    c->VSGetConstantBuffers(0, 6, worldBuffers);
    bool native = true;
    const UINT sizes[] = {64, 64, 64, 16, 320, 48};
    for (int i = 0; i < 6; i++) {
        D3D11_BUFFER_DESC d{};
        if (worldBuffers[i])
            worldBuffers[i]->GetDesc(&d);
        if (d.ByteWidth != sizes[i])
            native = false;
        drop(worldBuffers[i]);
    }
    return native;
}

// Runs before every draw: remembers the scene target on the first native world
// draw and grades the scene at the first orthographic draw into that target.
static void before(ID3D11DeviceContext *c) {
    if (!enabled || internal || finished)
        return;
    ID3D11Buffer *b = nullptr;
    c->VSGetConstantBuffers(0, 1, &b);
    float projection[16]{};
    bool known = readBuffer(b, projection, 64);
    drop(b);
    if (!known)
        return;
    bool orthographic = fabsf(projection[15]) > .5f;
    if (orthographic) {
        if (target) {
            ID3D11RenderTargetView *current = nullptr;
            c->OMGetRenderTargets(1, &current, nullptr);
            bool same = current == target;
            drop(current);
            if (same) {
                // MoonandShadow's world pass must come first.
                auto module = GetModuleHandleW(L"MoonandShadow.dll");
                auto finish = module
                                  ? (void(WINAPI *)())GetProcAddress(module, "FinishWorldLighting")
                                  : nullptr;
                if (finish)
                    finish();
                apply();
            }
        }
        return;
    }
    if (!hasNativeWorldBuffers(c))
        return;
    UINT n = 1;
    D3D11_VIEWPORT vp{};
    c->RSGetViewports(&n, &vp);
    if (!n || vp.Width < 320 || vp.Height < 180 || vp.TopLeftX || vp.TopLeftY)
        return; // Not the full-screen scene.
    if (!target) {
        // The sky can retain the solid shader's lighting buffers. Its camera
        // has no translation and can have a different far plane or no depth
        // target. Never let it claim the world's fog projection/depth.
        ID3D11Buffer *cameraBuffer = nullptr;
        c->VSGetConstantBuffers(1, 1, &cameraBuffer);
        float camera[16]{};
        bool cameraKnown = readBuffer(cameraBuffer, camera, 64);
        drop(cameraBuffer);
        if (!cameraKnown || fabsf(camera[3]) + fabsf(camera[7]) + fabsf(camera[11]) < .01f)
            return;
        context = c;
        viewport = vp;
        c->OMGetRenderTargets(1, &target, &sceneDepth);
        if (!target || !sceneDepth) {
            drop(target);
            drop(sceneDepth);
            return; // Keep looking for the depth-backed world scene.
        }
        memcpy(sceneProjection, projection, sizeof(sceneProjection));
        projectionKnown = true;
        float fog[7]{};
        solidDistance = fogConstants(fog) ? fog[6] : 0;
        static unsigned cameraReports = 0;
        if (cameraReports++ % 3600 == 0) {
            char message[224];
            sprintf_s(message,
                      "World fog: enabled=%d projection=(%.9g,%.9g,%.9g,%.9g) "
                      "start=%.3f end=%.3f opaque=%.3f (view units).",
                      fogEnabled, projection[10], projection[11], projection[14], projection[15],
                      fog[4], fog[5], fog[6]);
            log(message);
        }
    }
}

// True for a native world draw that lies wholly beyond the opaque fog: every
// pixel it would cover would be pure fog colour, so it is skipped. Uses a
// conservative bounding sphere: 78 blocks around a terrain chunk's centre (unit
// world scale), 128 voxels around the model's centre for models.
static bool culled(ID3D11DeviceContext *c) {
    if (solidDistance <= 0 || internal || finished || !hasNativeWorldBuffers(c))
        return false;
    ID3D11Buffer *b[3]{};
    c->VSGetConstantBuffers(0, 3, b);
    float projection[16], view[16], world[16];
    bool known = readBuffer(b[0], projection, 64) && readBuffer(b[1], view, 64) &&
                 readBuffer(b[2], world, 64);
    for (auto *x : b)
        drop(x);
    // The world camera: perspective, same focal lengths and depth range as the
    // scene (TemporalAA's jitter only changes the offset terms).
    auto close = [](float a, float b) { return fabsf(a - b) <= 1e-4f * std::max(1.f, fabsf(b)); };
    if (!known || fabsf(projection[15]) > .5f || !close(projection[0], sceneProjection[0]) ||
        !close(projection[5], sceneProjection[5]) || !close(projection[10], sceneProjection[10]) ||
        !close(projection[11], sceneProjection[11]) || !close(projection[14], sceneProjection[14]))
        return false; // orthographic UI, previews, other views
    float scale = 0;
    for (int column = 0; column < 3; ++column)
        scale = std::max(scale, sqrtf(world[column] * world[column] +
                                      world[4 + column] * world[4 + column] +
                                      world[8 + column] * world[8 + column]));
    bool terrain = fabsf(scale - 1) < 1e-3f;
    // Local centre and radius (native vertices span 0..255 units before world).
    // Terrain: a 64x64-block zone column slice, kept tall (128) to be safe.
    float localXY = terrain ? 32.f : 64.f, localZ = terrain ? 64.f : 64.f;
    float radius = terrain ? 78.f : 111.f * scale;
    float centre[3];
    for (int r = 0; r < 3; ++r)
        centre[r] = world[r * 4] * localXY + world[r * 4 + 1] * localXY +
                    world[r * 4 + 2] * localZ + world[r * 4 + 3];
    // Fog uses linear view depth, so the view-space z of the nearest point counts.
    float z = view[8] * centre[0] + view[9] * centre[1] + view[10] * centre[2] + view[11];
    ++worldDraws;
    if (fabsf(z) - radius <= solidDistance)
        return false;
    ++culledDraws;
    return true;
}

// ===========================================================================
// Hooks
// ===========================================================================

static void STDMETHODCALLTYPE di(ID3D11DeviceContext *c, UINT n, UINT s, INT b) {
    before(c);
    if (culled(c))
        return;
    oldDI(c, n, s, b);
}
static void STDMETHODCALLTYPE draw(ID3D11DeviceContext *c, UINT n, UINT s) {
    before(c);
    if (culled(c))
        return;
    oldD(c, n, s);
}
// Map/Unmap/Update: remember writes to 64-byte constant buffers.
static HRESULT STDMETHODCALLTYPE map(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub,
                                     D3D11_MAP t, UINT flags, D3D11_MAPPED_SUBRESOURCE *m) {
    auto hr = oldMap(c, r, sub, t, flags, m);
    if (!internal && SUCCEEDED(hr) && sub == 0 && t != D3D11_MAP_READ) {
        UINT n = bytes(r);
        if (n)
            mapped[r] = {m->pData, n};
    }
    return hr;
}
static void STDMETHODCALLTYPE unmap(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub) {
    auto it = mapped.find(r);
    if (it != mapped.end()) {
        r->SetPrivateData(cacheGuid, it->second.size, it->second.data);
        mapped.erase(it);
    }
    oldUnmap(c, r, sub);
}
static void STDMETHODCALLTYPE update(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub,
                                     const D3D11_BOX *box, const void *data, UINT row, UINT slice) {
    if (!internal && sub == 0 && !box && data) {
        UINT n = bytes(r);
        if (n)
            r->SetPrivateData(cacheGuid, n, data);
    }
    oldUpdate(c, r, sub, box, data, row, slice);
}
static bool patch(void **slot, void *fn) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
        return false;
    InterlockedExchangePointer(slot, fn);
    DWORD unused;
    VirtualProtect(slot, sizeof(void *), old, &unused);
    return true;
}

static void loadSettings() {
    const wchar_t *ini = L".\\Mods\\alpha-lookalike.ini";
    enabled = GetPrivateProfileIntW(L"Palette", L"Enabled", 1, ini) != 0;
    saturation =
        std::clamp((int)GetPrivateProfileIntW(L"Palette", L"SaturationPercent", 90, ini), 0, 100) *
        .01f;
    cool = std::clamp((int)GetPrivateProfileIntW(L"Palette", L"CoolTintPercent", 4, ini), 0, 10) *
           .01f;
    auto percent = [&](const wchar_t *name, int fallback, int high) {
        return std::clamp((int)GetPrivateProfileIntW(L"Fog", name, fallback, ini), 0, high) * .01f;
    };
    fogEnabled = GetPrivateProfileIntW(L"Fog", L"Enabled", 1, ini) != 0;
    fogStart = percent(L"StartPercent", 8, 100);
    fogEnd = std::max(percent(L"EndPercent", 55, 150), fogStart + .01f);
    fogStrength = percent(L"StrengthPercent", 90, 100);
    fogCull = GetPrivateProfileIntW(L"Fog", L"Cull", 1, ini) != 0;
    fogSolid = std::max(percent(L"SolidPercent", 70, 150), fogEnd + .01f);
    const wchar_t *channels[] = {L"ColorRed", L"ColorGreen", L"ColorBlue"};
    const int defaults[] = {40, 70, 150};
    for (int i = 0; i < 3; i++)
        fogColor[i] =
            std::clamp((int)GetPrivateProfileIntW(L"Fog", channels[i], defaults[i], ini), 0, 255) /
            255.f;
}

// Hooks the shared ID3D11DeviceContext vtable (taken from a temporary device).
// All hooks are installed or none are.
static void attach() {
    if (table || !nativeScene::supported())
        return;
    loadSettings();
    if (!enabled)
        return;
    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &d, nullptr, &c)))
        return;
    auto v = *(void ***)c;
    struct Hook {
        UINT slot;
        void *fn;
        void **original;
    };
    Hook hooks[] = {{SLOT_DRAW_INDEXED, (void *)di, (void **)&oldDI},
                    {SLOT_DRAW, (void *)draw, (void **)&oldD},
                    {SLOT_MAP, (void *)map, (void **)&oldMap},
                    {SLOT_UNMAP, (void *)unmap, (void **)&oldUnmap},
                    {SLOT_UPDATE_SUBRESOURCE, (void *)update, (void **)&oldUpdate}};
    for (auto &h : hooks)
        *h.original = v[h.slot];
    int done = 0;
    bool ok = true;
    for (auto &h : hooks) {
        if (!patch(v + h.slot, h.fn)) {
            ok = false;
            while (done--) {
                auto &u = hooks[done];
                patch(v + u.slot, *u.original);
            }
            break;
        }
        done++;
    }
    if (ok)
        table = v;
    drop(c);
    drop(d);
}
} // namespace palette

// ===========================================================================
// Cube World mod-loader entry points
// ===========================================================================

class AlphaLookalike : public GenericMod {
  public:
    void Initialize() override {
        palette::attach();
    }
    void OnPresent(IDXGISwapChain *, UINT, UINT) override {
        if (GetAsyncKeyState(VK_F6) & 1) {
            palette::fogEnabled = !palette::fogEnabled;
            palette::skyValid = false;
            palette::log(palette::fogEnabled ? "Fog enabled (F6)." : "Fog disabled (F6).");
        }
        palette::reset();
    }
};
EXPORT GenericMod *MakeMod() {
    return new AlphaLookalike();
}
EXPORT void ModPreInitialize() {}
EXPORT int ModMajorVersion() {
    return 7;
}
EXPORT int ModMinorVersion() {
    return 3;
}

// Called by TemporalAA so the grade is applied before its resolve.
extern "C" __declspec(dllexport) void WINAPI FinishAlphaLookalike() {
    palette::apply();
}
