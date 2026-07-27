#pragma once

#include "engine/render/RenderQuality.hpp"
#include "engine/rhi/Types.hpp"

#include <array>
#include <optional>

namespace fadix::ssao
{
// Resolved screen-space AO parameters for a frame. Quality controls cost
// (enable/resolution/samples/blur); the artistic radius/intensity/power/bias
// come from EnvironmentComponent. AO is applied ONLY to ambient/IBL light.
struct SsaoSettings
{
    bool Enabled{false};
    bool HalfResolution{true};
    int SampleCount{16};
    bool BilateralBlur{true};

    float Radius{0.5F};      // world-space sampling radius
    float Intensity{1.0F};   // occlusion strength multiplier
    float Power{1.5F};       // contrast curve on the AO term
    float Bias{0.025F};      // range check bias to fight self-occlusion
    float FadeStart{25.0F};  // view distance where AO begins to fade out
    float FadeEnd{45.0F};    // view distance where AO is fully gone

    bool Debug{false};       // AO-only visualization
};

// Cost-only fields from the active quality profile.
[[nodiscard]] SsaoSettings FromQuality(const RenderQualitySettings& quality) noexcept;

// Preferred-to-fallback sampleable depth format order. The renderer queries the
// device for each and takes the first supported; returns none if the platform
// cannot sample any depth format (caller then uses a color depth-copy or skips).
[[nodiscard]] std::array<rhi::Format, 3> DepthFormatPreference() noexcept;

// Pure fallback selection used by the renderer and the smoke: given which
// formats the device reports as sampleable-depth-capable (in the preference
// order above), returns the first supported, or nullopt.
[[nodiscard]] std::optional<rhi::Format> SelectSampleableDepthFormat(
    const std::array<bool, 3>& supported) noexcept;
}
