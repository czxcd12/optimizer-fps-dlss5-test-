// A small AA change must not switch valid silhouette detail fully on/off.
// Unlike the fractional-motion test, motion is zero: both old/new sampling agree.
#include "temporal_grid_scenes.h"
#include <algorithm>
#include <cstdio>
using namespace temporal_grid_scenes;

void TestSilhouetteColour(Gpu &gpu, const std::vector<char> &shader, const std::vector<char> &legacy)
{
    float oldMinimum = 1.0f, oldMaximum = 0.0f;
    for (float ratio : {0.0f, 1.0f})
        for (bool reversed : {false, true})
            for (int scene = 0; scene < 3; ++scene)
                for (float centre : {0.63f, 0.66f}) {
                    // 0: valid variation on the right-hand surface. 1: changed
                    // uniform surface. 2: matching colours only ACROSS the edge.
                    Image now(kW, kH), history(kW, kH, {0.5f, 0.5f, 0.5f, 1});
                    std::vector<float> depth(kW * kH);
                    std::vector<float> chain(kW * kH * 2, 0.0f);
                    for (UINT y = 0; y < kH; ++y)
                        for (UINT x = 0; x < kW; ++x) {
                            const bool own = x >= kW / 2;
                            const float c = scene == 0 ? (own ? ((x + y) % 2 ? 0.8f : 0.4f) : 4.0f)
                                                       : (scene == 2 && !own ? 0.5f : 4.0f);
                            now.at(x, y) = {c, c, c, 0.75f};
                            const float z = own ? 0.8f : 0.2f;
                            depth[y * kW + x] = reversed ? 1.0f - z : z;
                        }
                    if (scene == 0) now.at(kW / 2, kH / 2) = {centre, centre, centre, 0.75f};
                    Views views{};
                    std::vector<ComPtr<ID3D11ShaderResourceView>> keep;
                    auto set = [&](int slot, ComPtr<ID3D11ShaderResourceView> v) { views[slot] = v.Get(); keep.push_back(v); };
                    set(0, gpu.Srv(now)); set(7, gpu.Srv(history));
                    set(2, gpu.Srv(Image(kW, kH, {0.125f, 0.125f, 0.125f, 0})));
                    set(4, gpu.Srv(kW, kH, DXGI_FORMAT_R32G32_FLOAT, chain.data(), 2 * sizeof(float)));
                    set(5, gpu.Srv(kW, kH, DXGI_FORMAT_R32_FLOAT, depth.data(), sizeof(float)));
                    views[6] = views[5];
                    set(11, gpu.Srv(Image(kW, kH, {0, 0, 0, 1})));
                    const Root root = Constants({0, 1, 0, 0.05f}, {0.08f, 1, 0, 0},
                        {0, float(kLowW), float(kLowH), 1}, {0, 24, ratio, 1});
                    const auto result = Run(gpu, shader, views, root, {kW, kH}, Native(kW, kH));
                    const auto old = Run(gpu, legacy, views, root, {kW, kH}, Native(kW, kH));
                    const Texel add = result[1].at(kW / 2, kH / 2);
                    if (scene == 0) {
                        Require(std::abs(add[0] - 0.125f) < 0.001f && std::abs(add[3] - 1.0f) < 0.001f,
                                "silhouette AA variation dropped valid detail");
                        const float oldAdd = old[1].at(kW / 2, kH / 2)[0];
                        oldMinimum = std::min(oldMinimum, oldAdd); oldMaximum = std::max(oldMaximum, oldAdd);
                    } else {
                        Require(std::abs(add[0]) < 0.001f && add[3] < 0.001f,
                                "silhouette borrowed colour from a changed or different-depth surface");
                    }
                    Require(result[0].at(kW / 2, kH / 2)[3] == 0.75f, "silhouette changed alpha");
                }
    Require(oldMaximum - oldMinimum > 0.1f, "silhouette regression did not expose the old on/off jump");
    std::printf("PASS: silhouette colour stability, depth isolation and changed surfaces; legacy jump %g\n", oldMaximum - oldMinimum);
}
