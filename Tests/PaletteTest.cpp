#include "../AlphaLookalike.cpp"
#include <cassert>
#include <cstdio>
#include <array>
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
    {
        // Distance fog: a standard perspective (near 0.5, far 100) and a depth
        // buffer cleared to a known distance.
        const UINT w = 640, h = 360;
        ID3D11Texture2D *t = nullptr, *readback = nullptr, *depthTexture = nullptr;
        ID3D11RenderTargetView *rt = nullptr;
        ID3D11DepthStencilView *dsv = nullptr;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
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
        td.Usage = D3D11_USAGE_DEFAULT;
        td.CPUAccessFlags = 0;
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &depthTexture)));
        assert(SUCCEEDED(d->CreateDepthStencilView(depthTexture, nullptr, &dsv)));
        const float n = .5f, f = 100.f;
        float projection[16]{};
        projection[0] = projection[5] = 1;
        projection[10] = f / (f - n);
        projection[11] = -n * f / (f - n);
        projection[14] = 1;
        auto run = [&](float depthValue) {
            float color[] = {.8f, .2f, .1f, 1};
            c->ClearRenderTargetView(rt, color);
            c->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, depthValue, 0);
            palette::target = rt;
            rt->AddRef();
            palette::sceneDepth = dsv;
            dsv->AddRef();
            memcpy(palette::sceneProjection, projection, 64);
            palette::projectionKnown = true;
            palette::viewport = {0, 0, (float)w, (float)h, 0, 1};
            palette::finished = false;
            c->OMSetRenderTargets(1, &rt, nullptr);
            assert(palette::apply());
            palette::drop(palette::sceneDepth);
            c->OMSetRenderTargets(0, nullptr, nullptr);
            c->CopyResource(readback, t);
            D3D11_MAPPED_SUBRESOURCE map{};
            assert(SUCCEEDED(c->Map(readback, 0, D3D11_MAP_READ, 0, &map)));
            auto pixel = (unsigned char *)map.pData + (h / 2) * map.RowPitch + (w / 2) * 4;
            int rgb[3] = {pixel[0], pixel[1], pixel[2]};
            c->Unmap(readback, 0);
            return std::array<int, 3>{rgb[0], rgb[1], rgb[2]};
        };
        palette::fogEnabled = false;
        auto graded = run(.5f);
        palette::fogEnabled = true;
        // Sky (depth 1) is never fogged.
        assert(run(1.f) == graded);
        // Near (2 units) stays clear; 30 units blends toward the fog colour.
        float z = 2;
        assert(run(projection[10] + projection[11] / z) == graded);
        z = 30;
        auto distant = run(projection[10] + projection[11] / z);
        float s = (z - palette::fogStart * f) / ((palette::fogEnd - palette::fogStart) * f);
        float amount = s * s * (3 - 2 * s) * palette::fogStrength;
        for (int i = 0; i < 3; i++) {
            int expected = int(roundf(graded[i] + (palette::fogColor[i] * 255 - graded[i]) * amount));
            assert(abs(distant[i] - expected) <= 3);
        }
        // Without a projection or depth, fog is off and the grade is unchanged.
        palette::fogEnabled = true;
        palette::target = rt;
        rt->AddRef();
        palette::projectionKnown = false;
        palette::finished = false;
        c->OMSetRenderTargets(1, &rt, nullptr);
        assert(palette::apply());
        c->OMSetRenderTargets(0, nullptr, nullptr);
        palette::drop(dsv);
        palette::drop(depthTexture);
        palette::drop(rt);
        palette::drop(readback);
        palette::drop(t);
        printf("Distance fog: sky untouched, near clear, far blended (%d,%d,%d -> %d,%d,%d) PASS\n",
               graded[0], graded[1], graded[2], distant[0], distant[1], distant[2]);
    }
    {
        // MSAA scene (4x) with MSAA depth, as the game renders: fog must apply,
        // both when the depth is shader-readable (read in place) and when it
        // has to be copied. The result must match the single-sample fog.
        const UINT w = 640, h = 360;
        const float n = .5f, f = 100.f;
        float projection[16]{};
        projection[0] = projection[5] = 1;
        projection[10] = f / (f - n);
        projection[11] = -n * f / (f - n);
        projection[14] = 1;
        for (bool readable : {true, false}) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.SampleDesc = {4, 0};
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            ID3D11Texture2D *t = nullptr, *resolved = nullptr, *readback = nullptr,
                            *depthTexture = nullptr;
            ID3D11RenderTargetView *rt = nullptr;
            ID3D11DepthStencilView *dsv = nullptr;
            assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &t)));
            assert(SUCCEEDED(d->CreateRenderTargetView(t, nullptr, &rt)));
            td.SampleDesc = {1, 0};
            td.BindFlags = 0;
            assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &resolved)));
            td.Usage = D3D11_USAGE_STAGING;
            td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &readback)));
            td.Usage = D3D11_USAGE_DEFAULT;
            td.CPUAccessFlags = 0;
            td.SampleDesc = {4, 0};
            td.Format = readable ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_D24_UNORM_S8_UINT;
            td.BindFlags = D3D11_BIND_DEPTH_STENCIL | (readable ? D3D11_BIND_SHADER_RESOURCE : 0);
            assert(SUCCEEDED(d->CreateTexture2D(&td, nullptr, &depthTexture)));
            D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
            dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
            assert(SUCCEEDED(d->CreateDepthStencilView(depthTexture, &dv, &dsv)));
            auto run = [&](float depthValue, bool fogOn) {
                float color[] = {.8f, .2f, .1f, 1};
                c->ClearRenderTargetView(rt, color);
                c->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, depthValue, 0);
                palette::fogEnabled = fogOn;
                palette::target = rt;
                rt->AddRef();
                palette::sceneDepth = dsv;
                dsv->AddRef();
                memcpy(palette::sceneProjection, projection, 64);
                palette::projectionKnown = true;
                palette::viewport = {0, 0, (float)w, (float)h, 0, 1};
                palette::finished = false;
                c->OMSetRenderTargets(1, &rt, nullptr);
                assert(palette::apply());
                palette::drop(palette::sceneDepth);
                c->OMSetRenderTargets(0, nullptr, nullptr);
                c->ResolveSubresource(resolved, 0, t, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
                c->CopyResource(readback, resolved);
                D3D11_MAPPED_SUBRESOURCE map{};
                assert(SUCCEEDED(c->Map(readback, 0, D3D11_MAP_READ, 0, &map)));
                auto pixel = (unsigned char *)map.pData + (h / 2) * map.RowPitch + (w / 2) * 4;
                std::array<int, 3> rgb{pixel[0], pixel[1], pixel[2]};
                c->Unmap(readback, 0);
                return rgb;
            };
            auto graded = run(.5f, false);
            float z = 30;
            auto distant = run(projection[10] + projection[11] / z, true);
            float s = (z - palette::fogStart * f) / ((palette::fogEnd - palette::fogStart) * f);
            float amount = s * s * (3 - 2 * s) * palette::fogStrength;
            for (int i = 0; i < 3; i++) {
                int expected =
                    int(roundf(graded[i] + (palette::fogColor[i] * 255 - graded[i]) * amount));
                assert(abs(distant[i] - expected) <= 3);
            }
            assert(distant != graded);
            assert(palette::depthMultisampled);
            assert(readable ? palette::depthInput == palette::depthSourceView
                            : palette::depthInput == palette::depthView);
            palette::drop(dsv);
            palette::drop(depthTexture);
            palette::drop(rt);
            palette::drop(readback);
            palette::drop(resolved);
            palette::drop(t);
            printf("MSAA fog (%s depth): (%d,%d,%d) -> (%d,%d,%d) PASS\n",
                   readable ? "in-place" : "copied", graded[0], graded[1], graded[2], distant[0],
                   distant[1], distant[2]);
        }
    }
    palette::drop(c);
    palette::drop(d);
}
