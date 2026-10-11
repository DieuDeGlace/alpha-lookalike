#include "../AlphaLookalike.cpp"
#include <cassert>
#include <cstdio>
int main() {
    ID3D11Device *d = nullptr;
    ID3D11DeviceContext *c = nullptr;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &d, nullptr, &c)));
    palette::oldD = (palette::D)(*(void ***)c)[13];
    assert(palette::initialize(c));
    for (auto size : {std::pair<UINT, UINT>{1280, 720}, {1920, 1080}, {640, 360}}) {
        ID3D11Texture2D *t = nullptr, *readback = nullptr;
        ID3D11RenderTargetView *rt = nullptr;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = size.first;
        td.Height = size.second;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &t)));
        assert(SUCCEEDED(d->CreateRenderTargetView(t, nullptr, &rt)));
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &readback)));
        float color[] = {.8f, .2f, .1f, 1};
        c->ClearRenderTargetView(rt, color);
        c->OMSetRenderTargets(1, &rt, nullptr);
        palette::target = rt;
        rt->AddRef();
        palette::viewport = {0, 0, (float)size.first, (float)size.second, 0, 1};
        palette::finished = false;
        assert(palette::apply());
        assert(!palette::target);
        ID3D11RenderTargetView *restored = nullptr;
        c->OMGetRenderTargets(1, &restored, nullptr);
        assert(restored == rt);
        palette::drop(restored);
        c->CopyResource(readback, t);
        D3D11_MAPPED_SUBRESOURCE map{};
        assert(SUCCEEDED(c->Map(readback, 0, D3D11_MAP_READ, 0, &map)));
        auto pixel = (unsigned char *)map.pData;
        float l = .8f * .2126f + .2f * .7152f + .1f * .0722f;
        float shade = 1 - std::clamp((l - .15f) / .65f, 0.f, 1.f) *
                              std::clamp((l - .15f) / .65f, 0.f, 1.f) *
                              (3 - 2 * std::clamp((l - .15f) / .65f, 0.f, 1.f));
        float warm = std::clamp((.8f - .1f) * 2, 0.f, 1.f) * std::clamp((.2f - .1f) * 2, 0.f, 1.f);
        float h = std::clamp((l - .25f) / .5f, 0.f, 1.f);
        h = h * h * (3 - 2 * h);
        float expected[] = {
            ((l + (.8f - l) * palette::saturation + warm * h * .025f) * (1 - palette::cool * .6f)),
            l + (.2f - l) * palette::saturation + warm * h * .035f,
            (l + (.1f - l) * palette::saturation) * (1 + palette::cool * (.6f + .4f * shade))};
        for (int i = 0; i < 3; i++)
            assert(abs((int)pixel[i] - (int)roundf(expected[i] * 255)) <= 2);
        assert(pixel[3] == 255);
        c->Unmap(readback, 0);
        // HUD drawn after grading retains its ungraded color.
        c->ClearRenderTargetView(rt, color);
        c->CopyResource(readback, t);
        assert(SUCCEEDED(c->Map(readback, 0, D3D11_MAP_READ, 0, &map)));
        pixel = (unsigned char *)map.pData;
        assert(pixel[0] == 204);
        c->Unmap(readback, 0);
        ID3DDeviceContextState *saved = nullptr;
        palette::context1->SwapDeviceContextState(palette::state, &saved);
        ID3D11RenderTargetView *parked = nullptr;
        c->OMGetRenderTargets(1, &parked, nullptr);
        assert(!parked);
        ID3D11ShaderResourceView *srv = nullptr;
        c->PSGetShaderResources(0, 1, &srv);
        assert(!srv);
        palette::context1->SwapDeviceContextState(saved, nullptr);
        palette::drop(saved);
        c->OMSetRenderTargets(0, nullptr, nullptr);
        palette::drop(rt);
        palette::drop(readback);
        palette::drop(t);
        printf("Palette colors, state restoration and target release: %ux%u PASS\n", size.first,
               size.second);
    }
    palette::drop(c);
    palette::drop(d);
}
