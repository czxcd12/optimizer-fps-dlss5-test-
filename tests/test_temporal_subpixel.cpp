// Subpixel motion must not reject matching colour at hair/building edges merely
// because the history colour was rounded while the residual was interpolated.
#include "temporal_grid_scenes.h"
#include <algorithm>
#include <cstdio>

using namespace temporal_grid_scenes;

void TestSilhouetteColour(Gpu &gpu, const std::vector<char> &shader, const std::vector<char> &legacy);

int main(int argc, char **argv)
{
    try {
        Require(argc == 3, "usage: subpixel_tests <shader directory> <legacy shader directory>");
        Gpu gpu;
        const auto shader = ReadFile(std::filesystem::path(argv[1]) / "temporal_Reproject_cs.dxbc");
        const auto legacy = ReadFile(std::filesystem::path(argv[2]) / "temporal_Reproject_cs.dxbc");
        TestSilhouetteColour(gpu, shader, legacy);
        float legacyLoss = 0.0f;
        for (float shift : {-0.75f, -0.5f, -0.25f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
            for (float ratio : {0.0f, 1.0f})
                for (bool disoccluded : {false, true}) {
                    Image now(kW, kH), then(kW, kH);
                    std::vector<float> depth(kW * kH), depthF(kW * kH), chain(kW * kH * 2);
                    auto value = [](int x) { return (x & 1) ? 0.75f : 0.125f; };
                    for (UINT y = 0; y < kH; ++y)
                        for (UINT x = 0; x < kW; ++x) {
                            const float q = float(x) + shift;
                            const int base = int(std::floor(q));
                            const float t = q - float(base);
                            const float c = disoccluded ? 4.0f : value(base) * (1.0f - t) + value(base + 1) * t;
                            now.at(x, y) = {c, c, c, 0.75f};
                            then.at(x, y) = {value(int(x)), value(int(x)), value(int(x)), 0.75f};
                            const UINT i = y * kW + x;
                            // Include an actual guide silhouette amid fine window-like detail.
                            depth[i] = x < kW / 2 ? 0.2f : 0.8f;
                            depthF[i] = float(x) < float(kW / 2) + shift ? 0.2f : 0.8f;
                            chain[i * 2] = shift;
                        }
                    std::vector<ComPtr<ID3D11ShaderResourceView>> keep;
                    Views views{};
                    auto set = [&](int slot, ComPtr<ID3D11ShaderResourceView> v) { views[slot] = v.Get(); keep.push_back(v); };
                    set(0, gpu.Srv(now)); set(7, gpu.Srv(then));
                    set(2, gpu.Srv(Image(kW, kH, {0.125f, 0.125f, 0.125f, 0.0f})));
                    set(4, gpu.Srv(kW, kH, DXGI_FORMAT_R32G32_FLOAT, chain.data(), 2 * sizeof(float)));
                    set(5, gpu.Srv(kW, kH, DXGI_FORMAT_R32_FLOAT, depth.data(), sizeof(float)));
                    set(6, gpu.Srv(kW, kH, DXGI_FORMAT_R32_FLOAT, depthF.data(), sizeof(float)));
                    set(11, gpu.Srv(Image(kW, kH, {0, 0, 0, 1}))); // use current depth
                    const Root root = Constants({0, 1, 0, 0.05f}, {0.08f, 1, 0, 0}, {0, float(kLowW), float(kLowH), 1}, {0, 24, ratio, 1});
                    const auto out = Run(gpu, shader, views, root, {kW, kH}, Native(kW, kH));
                    const auto old = Run(gpu, legacy, views, root, {kW, kH}, Native(kW, kH));
                    for (UINT x = 32; x < kW - 32; ++x) {
                        const auto &add = out[1].at(x, kH / 2);
                        if (disoccluded) {
                            Require(add[3] <= 0.001f, "changed surface was incorrectly accepted");
                            Require(std::abs(add[0]) < 0.001f, "changed surface inherited stale detail");
                        } else {
                            if (std::abs(add[3] - 1.0f) >= 0.001f)
                                std::fprintf(stderr, "FAIL: x=%u shift=%g ratio=%g acceptance=%g addition=%g\n", x, shift, ratio, add[3], add[0]);
                            Require(std::abs(add[3] - 1.0f) < 0.001f, "matching subpixel edge was rejected");
                            Require(std::abs(add[0] - 0.125f) < 0.001f, "matching subpixel edge lost NR detail");
                            legacyLoss = std::max(legacyLoss, std::abs(old[1].at(x, kH / 2)[0] - 0.125f));
                        }
                        Require(out[0].at(x, kH / 2)[3] == 0.75f, "carry changed alpha");
                    }
                }
        Require(legacyLoss > 0.05f, "regression did not expose the old point-sampled gate");
        std::printf("PASS: subpixel edge carry, both directions/domains, silhouette and disocclusion; old loss %g\n", legacyLoss);
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
