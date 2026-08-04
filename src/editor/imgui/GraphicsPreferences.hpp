#pragma once

#include "engine/camera/EditorMode.hpp"
#include "engine/render/RenderQuality.hpp"

#include <string>
#include <string_view>

namespace fadix::editor
{
struct GraphicsPreferences
{
    RenderQualityPreset SceneQuality{RenderQualityPreset::Low};
    RenderQualityPreset GameQuality{RenderQualityPreset::High};
    // 0 = follow preset ResolutionScale
    float ResolutionScaleOverride{0.0F};
    AntiAliasMode AaOverride{AntiAliasMode::Auto}; // Auto = no editor override
    int ShadowCascadeCap{0};      // 0 = follow quality
    int ShadowResolutionCap{0};   // 0 = follow quality
    int SpotShadowBudgetCap{0};   // 0 = follow quality
    int PointShadowBudgetCap{0};  // 0 = follow quality
    ViewportProjectionMode ProjectionMode{ViewportProjectionMode::Perspective};

    [[nodiscard]] static GraphicsPreferences Defaults() noexcept;
    void ResetToDefaults() noexcept { *this = Defaults(); }

    // Mutates a copy of MakeQualitySettings(preset) with caps/overrides.
    // AaOverride is not stored on RenderQualitySettings — applied later when
    // building PostProcessSettings (ResolveAntiAlias with env + quality default).
    [[nodiscard]] RenderQualitySettings ApplyTo(RenderQualityPreset preset) const noexcept;
};

[[nodiscard]] bool ParseGraphicsPreferences(std::string_view json, GraphicsPreferences& out);
[[nodiscard]] std::string StringifyGraphicsPreferences(const GraphicsPreferences& prefs);
}
