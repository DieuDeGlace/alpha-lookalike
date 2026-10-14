// alpha-lookalike: an Alpha-inspired colour grade for Cube World (Steam,
// 64-bit, D3D11). Original world-only grading, made with AI assistance.
// No ReShade integration.
//
// Like TemporalAA, it shadows 64-byte constant buffers (Map/Unmap/Update hooks)
// to recognise native world draws, remembers the scene render target, and when
// the first orthographic (HUD) draw arrives it grades the finished 3D scene in
// place, so the HUD itself is never tinted. TemporalAA calls
// FinishAlphaLookalike() so the grade happens before its resolve.
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
static ID3D11Device *device = nullptr;
static ID3D11DeviceContext *context = nullptr;
static ID3D11DeviceContext1 *context1 = nullptr;
static ID3DDeviceContextState *state = nullptr; // Isolated pipeline state for our pass.
static ID3D11Texture2D *copy = nullptr;         // This frame's scene, as shader input.
static ID3D11ShaderResourceView *view = nullptr;
static ID3D11RenderTargetView *target = nullptr; // The scene target of this frame.
static D3D11_VIEWPORT viewport{};
static ID3D11VertexShader *vs = nullptr;
static ID3D11PixelShader *ps = nullptr;
static ID3D11Buffer *params = nullptr;
static ID3D11RasterizerState *raster = nullptr;
static DI oldDI = nullptr;
static D oldD = nullptr;
static UINT width = 0, height = 0;
static DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

// vs: full-screen triangle from SV_VertexID. ps: the colour grade.
static const char *code = R"(
cbuffer Grade:register(b0){float saturation,cool,pad0,pad1;};
Texture2D<float4> scene:register(t0);

float4 vs(uint id:SV_VertexID):SV_POSITION{
    float2 p=float2((id<<1)&2,id&2);
    return float4(p*float2(2,-2)+float2(-1,1),0,1);
}

float4 ps(float4 p:SV_POSITION):SV_TARGET{
    float4 original=scene.Load(int3(p.xy,0));
    float l=dot(original.rgb,float3(.2126,.7152,.0722));
    float3 c=lerp(l.xxx,original.rgb,saturation);
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
    return float4(saturate(c),original.a);
})";

// Called once per presented frame.
static void reset() {
    drop(target);
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
    for (int i = 0; i < 2 && ok; i++) {
        bool pixel = i == 1;
        ID3DBlob *b = nullptr, *error = nullptr;
        ok = SUCCEEDED(D3DCompile(code, strlen(code), "NativePalette", nullptr, nullptr,
                                  pixel ? "ps" : "vs", pixel ? "ps_4_0" : "vs_4_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &error));
        if (error)
            log((char *)error->GetBufferPointer());
        if (ok)
            ok = SUCCEEDED(pixel ? device->CreatePixelShader(b->GetBufferPointer(),
                                                             b->GetBufferSize(), nullptr, &ps)
                                 : device->CreateVertexShader(b->GetBufferPointer(),
                                                              b->GetBufferSize(), nullptr, &vs));
        drop(b);
        drop(error);
    }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 16;
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
    context->PSSetShader(ps, nullptr, 0);
    float values[4] = {saturation, cool, 0, 0};
    context->UpdateSubresource(params, 0, nullptr, values, 0, 0);
    context->PSSetConstantBuffers(0, 1, &params);
    context->PSSetShaderResources(0, 1, &view);
    oldD(context, 3, 0); // Full-screen triangle.
    ID3D11ShaderResourceView *empty = nullptr;
    context->PSSetShaderResources(0, 1, &empty);
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
        context = c;
        viewport = vp;
        c->OMGetRenderTargets(1, &target, nullptr);
    }
}

// ===========================================================================
// Hooks
// ===========================================================================

static void STDMETHODCALLTYPE di(ID3D11DeviceContext *c, UINT n, UINT s, INT b) {
    before(c);
    oldDI(c, n, s, b);
}
static void STDMETHODCALLTYPE draw(ID3D11DeviceContext *c, UINT n, UINT s) {
    before(c);
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
