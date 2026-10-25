// Stationary N=2 sequence with a nonuniform NR edit on the sky. Uniform sky residuals
// cannot expose the old alternation between detailed full frames and smooth carried frames.
#include "temporal_d3d12_fixture.h"
#include "core/temporal/machine.h"

#include <algorithm>
#include <iostream>

using namespace temporal_d3d12;
using namespace ofps::core::temporal;

namespace {
void CopyAnswer(Recording &r, ID3D12Resource *fresh, ID3D12Resource *output)
{
    BarrierExternal(r.list.Get(), fresh, kRead, D3D12_RESOURCE_STATE_COPY_SOURCE);
    BarrierExternal(r.list.Get(), output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    r.list->CopyResource(output, fresh);
    BarrierExternal(r.list.Get(), output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    BarrierExternal(r.list.Get(), fresh, D3D12_RESOURCE_STATE_COPY_SOURCE, kRead);
}

bool RunCadence(ID3D12Device *device, ID3D12CommandQueue *queue, const ofps::core::gpu::Shaders &shaders,
                bool inverted, int route, unsigned phaseFrames, bool skyOn)
{
    Recording r;
    if (!r.Create(device)) return false;
    const auto color = r.Paint(device, kColor, {0.5f, 0.5f, 0.5f, 1.0f}, {}, false);
    const auto motion = r.Paint(device, DXGI_FORMAT_R16G16_FLOAT, {}, {}, false);
    const auto fresh = r.Paint(device, kColor, {0.625f, 0.5f, 0.625f, 0.75f}, {0.875f, 0.75f, 0.5f, 0.75f}, true);
    const auto depth = r.Paint(device, DXGI_FORMAT_R32_FLOAT, {inverted ? 0.0f : 1.0f, 0, 0, 0}, {}, false);
    if (!color || !motion || !fresh || !depth) return false;
    // Geometry at the left edge, while the model's block of detail lies wholly on sky.
    auto rtv = r.heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(r.painted - 1) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_RECT wall{0, 0, 16, kH};
    const float wallDepth[4] = {0.5f, 0, 0, 0};
    BarrierExternal(r.list.Get(), depth.Get(), kRead, D3D12_RESOURCE_STATE_RENDER_TARGET);
    r.list->ClearRenderTargetView(rtv, wallDepth, 1, &wall);
    BarrierExternal(r.list.Get(), depth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);

    FrameInputs in{};
    in.color = color.Get(); in.depth = depth.Get(); in.motion = motion.Get();
    in.colorView = kColor; in.depthView = DXGI_FORMAT_R32_FLOAT; in.motionView = DXGI_FORMAT_R16G16_FLOAT;
    in.colorRect = in.depthRect = in.motionRect = {0, 0, kW, kH};
    in.hostInputState = in.depthState = in.motionState = kRead;
    in.depthInverted = inverted;
    in.residualBlend = 0.5f;
    in.phaseInFrames = phaseFrames;
    in.skyField = skyOn;
    in.compose = route != 2;
    if (route == 1) { in.gridWidth = kW / 2; in.gridHeight = kH / 2; }
    Machine machine;
    char error[256]{};
    if (!machine.Initialize(device, shaders, kW, kH, kColor, kColor, kW, kH, DXGI_FORMAT_R32_FLOAT, kW, kH, error, sizeof(error)))
        return false;
    machine.SetUsePoint(r.Point(), 0);
    std::vector<ComPtr<ID3D12Resource>> outputs(5);
    for (auto &output : outputs) {
        output = Texture(device, kColor, D3D12_RESOURCE_FLAG_NONE);
        if (!output) return false;
    }
    // Include the first full frame (no old residual), then an established full/carry pair.
    for (int pass = 0; pass < 2; ++pass) {
        if (pass) machine.RecordAccumulate(r.list.Get(), in);
        machine.RecordResidual(r.list.Get(), in, fresh.Get(), kRead);
        CopyAnswer(r, fresh.Get(), outputs[pass * 2].Get());
        machine.RecordApply(r.list.Get(), in, outputs[pass * 2].Get(), D3D12_RESOURCE_STATE_COMMON, 0, 0);
        machine.RecordAccumulate(r.list.Get(), in);
        machine.RecordReproject(r.list.Get(), in, outputs[pass * 2 + 1].Get(), D3D12_RESOURCE_STATE_COMMON, 0, 0);
    }
    // The diagnostic raw-full override must retain the actual model answer.
    CopyAnswer(r, fresh.Get(), outputs[4].Get());
    in.rawFull = true;
    machine.RecordApply(r.list.Get(), in, outputs[4].Get(), D3D12_RESOURCE_STATE_COMMON, 0, 0);
    if (machine.TakeExhausted()) return false;
    const auto pictures = Execute(device, queue, r, outputs);
    if (pictures.size() != outputs.size()) return false;
    float jump = 0.0f;
    for (UINT y = 0; y < kH; ++y) {
        for (UINT x = 0; x < kW; ++x) {
            const std::size_t t = (static_cast<std::size_t>(y) * kW + x) * 4;
            const bool patch = x >= 40 && x < 140 && y >= 30 && y < 90;
            const float expected[4] = {patch ? 0.875f : 0.625f, patch ? 0.75f : 0.5f, patch ? 0.5f : 0.625f, 0.75f};
            for (int c = 0; c < 4; ++c) {
                if (pictures[4][t + c] != expected[c]) return false;
                for (int pass = 0; pass < 2; ++pass) {
                    // No phase-in: geometry, including model alpha, must survive the copy exactly.
                    if (x < 16 && (pass == 0 || phaseFrames == 0) && pictures[pass * 2][t + c] != expected[c]) return false;
                    if (!skyOn && (pass == 0 || phaseFrames == 0) && pictures[pass * 2][t + c] != expected[c]) return false;
                    if (x >= 16 && c < 3)
                        jump = std::max(jump, std::abs(pictures[pass * 2][t + c] - pictures[pass * 2 + 1][t + c]));
                }
            }
        }
    }
    // Control: ordinary carry retains the detailed edit. With the sky policy on, both
    // full and carried sky must instead agree with the smooth field (within fp16 rounding).
    if (skyOn && jump > 0.002f) {
        std::cerr << "FAIL: stationary sky cadence jump " << jump << " route " << route << '\n';
        return false;
    }
    return true;
}
} // namespace

int TestTemporalSkyCadence(ID3D12Device *device, ID3D12CommandQueue *queue, const char *directory)
{
    ofps::core::gpu::Shaders shaders;
    if (!LoadShaders(directory, shaders, true) || !LoadSkyShaders(directory, shaders)) return 1;
    for (const bool inverted : {false, true})
        for (int route = 0; route < 3; ++route)
            for (const unsigned phase : {0u, 1u})
                for (const bool sky : {false, true})
                    if (!RunCadence(device, queue, shaders, inverted, route, phase, sky)) {
                        std::cerr << "FAIL: sky cadence (Z=" << inverted << ", route=" << route
                                  << ", phase=" << phase << ", sky=" << sky << ")\n";
                        return 1;
                    }
    std::cout << "PASS: stationary N=2 sky cadence, native/grid/direct, both depth conventions, with/without phase-in\n";
    return 0;
}
