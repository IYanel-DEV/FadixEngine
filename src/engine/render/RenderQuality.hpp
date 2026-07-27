#pragma once

#include <optional>
#include <string_view>
#include <cstdint>

namespace fadix
{
enum class AntiAliasMode : std::uint8_t
{
    Auto = 0,
    Off = 1,
    Fxaa = 2,
    Taa = 3
};

inline constexpr unsigned AntiAliasModeCount = 4;

[[nodiscard]] inline AntiAliasMode SanitizeAntiAliasMode(const unsigned value) noexcept
{
    return value < AntiAliasModeCount ? static_cast<AntiAliasMode>(value) : AntiAliasMode::Auto;
}

[[nodiscard]] inline AntiAliasMode ResolveAntiAlias(
    const AntiAliasMode env, const AntiAliasMode qualityDefault) noexcept
{
    return env == AntiAliasMode::Auto ? qualityDefault : env;
}

// Named graphics-quality tiers. Higher tiers spend more GPU on detail and
// performance headroom; none of them change artistic look (exposure/colors).
enum class RenderQualityPreset
{
    Low,
    Medium,
    High,
    Epic
};

// Plain value type describing what a quality tier permits. Every field caps or
// scales a feature that already exists in the renderer; it never invents new
// scene data and never mutates components. Resolution scale is applied by the
// viewport panel after DPI; the rest are consumed by the renderer per frame.
struct RenderQualitySettings
{
    float ResolutionScale{1.0F};        // internal render target size vs. logical
    bool DirectionalShadowsEnabled{true};
    int ShadowResolutionCap{2048};      // upper bound on the directional shadow map (per cascade)
    int ShadowCascadeCount{4};          // directional shadow cascades, 1..4
    int ShadowFilterRadius{2};          // PCF kernel radius in texels (1 = 3x3)
    bool AoEnabled{true};               // screen-space ambient occlusion pass
    bool AoHalfResolution{true};        // run AO at half res (false = full for Epic)
    int AoSampleCount{16};              // AO samples per pixel
    bool AoBilateralBlur{true};         // depth-aware AO blur
    int SpotShadowBudget{4};            // max shadow-casting spot lights per frame
    int PointShadowBudget{1};           // max shadow-casting point lights per frame
    int MaxPointLights{8};              // <= shader array size (8)
    int MaxSpotLights{8};               // <= shader array size (8)
    int MaxBloomPasses{5};              // cap on PostProcessSettings::BloomPasses
    bool PostProcessingEnabled{true};
    bool FxaaEnabled{true};
    AntiAliasMode DefaultAntiAlias{AntiAliasMode::Fxaa};
    int AnisotropyLevel{1};             // 1 = trilinear; >1 enables anisotropy
};

// Pure preset construction. No I/O, no GPU, no globals — safe to smoke-test.
[[nodiscard]] RenderQualitySettings MakeQualitySettings(RenderQualityPreset preset) noexcept;

// Stable lowercase token for persistence ("low"/"medium"/"high"/"epic").
[[nodiscard]] const char* ToString(RenderQualityPreset preset) noexcept;

// Inverse of ToString; case-sensitive on the lowercase tokens above.
[[nodiscard]] std::optional<RenderQualityPreset> ParseQualityPreset(std::string_view text) noexcept;
}
