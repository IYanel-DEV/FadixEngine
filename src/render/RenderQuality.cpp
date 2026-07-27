#include "engine/render/RenderQuality.hpp"

namespace fadix
{
RenderQualitySettings MakeQualitySettings(const RenderQualityPreset preset) noexcept
{
    RenderQualitySettings settings;
    switch (preset)
    {
    case RenderQualityPreset::Low:
        settings.ResolutionScale = 0.70F;
        settings.DirectionalShadowsEnabled = true;
        settings.ShadowResolutionCap = 1024;
        // Truthful for now: cascades not connected yet -> single map for every tier.
        settings.ShadowCascadeCount = 1;
        settings.ShadowFilterRadius = 1;
        // SSAO does not composite into ambient yet; keep it off in normal rendering.
        settings.AoEnabled = false;
        settings.AoHalfResolution = true;
        settings.AoSampleCount = 0;
        settings.AoBilateralBlur = false;
        // Local (spot/point) shadow maps are not rendered/sampled yet: budget 0.
        settings.SpotShadowBudget = 0;
        settings.PointShadowBudget = 0;
        settings.MaxPointLights = 4;
        settings.MaxSpotLights = 4;
        settings.MaxBloomPasses = 2;
        settings.PostProcessingEnabled = true;
        settings.DefaultAntiAlias = AntiAliasMode::Off;
        settings.FxaaEnabled = settings.DefaultAntiAlias != AntiAliasMode::Off;
        settings.AnisotropyLevel = 1;
        break;
    case RenderQualityPreset::Medium:
        settings.ResolutionScale = 0.85F;
        settings.DirectionalShadowsEnabled = true;
        settings.ShadowResolutionCap = 2048;
        settings.ShadowCascadeCount = 2;
        settings.ShadowFilterRadius = 1;
        settings.AoEnabled = false;
        settings.AoHalfResolution = true;
        settings.AoSampleCount = 8;
        settings.AoBilateralBlur = false;
        settings.SpotShadowBudget = 0;
        settings.PointShadowBudget = 0;
        settings.MaxPointLights = 6;
        settings.MaxSpotLights = 6;
        settings.MaxBloomPasses = 4;
        settings.PostProcessingEnabled = true;
        settings.DefaultAntiAlias = AntiAliasMode::Fxaa;
        settings.FxaaEnabled = settings.DefaultAntiAlias != AntiAliasMode::Off;
        settings.AnisotropyLevel = 4;
        break;
    case RenderQualityPreset::High:
        settings.ResolutionScale = 1.00F;
        settings.DirectionalShadowsEnabled = true;
        settings.ShadowResolutionCap = 2048;
        settings.ShadowCascadeCount = 4;
        settings.ShadowFilterRadius = 2;
        settings.AoEnabled = false;
        settings.AoHalfResolution = true;
        settings.AoSampleCount = 16;
        settings.AoBilateralBlur = true;
        settings.SpotShadowBudget = 0;
        settings.PointShadowBudget = 0;
        settings.MaxPointLights = 8;
        settings.MaxSpotLights = 8;
        settings.MaxBloomPasses = 5;
        settings.PostProcessingEnabled = true;
        // TAA is temporarily disabled until stationary/motion stability tests pass;
        // High uses stable FXAA for now.
        settings.DefaultAntiAlias = AntiAliasMode::Fxaa;
        settings.FxaaEnabled = settings.DefaultAntiAlias != AntiAliasMode::Off;
        settings.AnisotropyLevel = 8;
        break;
    case RenderQualityPreset::Epic:
        settings.ResolutionScale = 1.00F;
        settings.DirectionalShadowsEnabled = true;
        settings.ShadowResolutionCap = 4096;
        settings.ShadowCascadeCount = 4;
        settings.ShadowFilterRadius = 3;
        settings.AoEnabled = false;
        settings.AoHalfResolution = false; // full-resolution AO option
        settings.AoSampleCount = 24;
        settings.AoBilateralBlur = true;
        settings.SpotShadowBudget = 0;
        settings.PointShadowBudget = 0;
        settings.MaxPointLights = 8;
        settings.MaxSpotLights = 8;
        settings.MaxBloomPasses = 6;
        settings.PostProcessingEnabled = true;
        // TAA temporarily disabled (see High); Epic uses stable FXAA for now.
        settings.DefaultAntiAlias = AntiAliasMode::Fxaa;
        settings.FxaaEnabled = settings.DefaultAntiAlias != AntiAliasMode::Off;
        settings.AnisotropyLevel = 16;
        break;
    }
    return settings;
}

const char* ToString(const RenderQualityPreset preset) noexcept
{
    switch (preset)
    {
    case RenderQualityPreset::Low:
        return "low";
    case RenderQualityPreset::Medium:
        return "medium";
    case RenderQualityPreset::High:
        return "high";
    case RenderQualityPreset::Epic:
        return "epic";
    }
    return "high";
}

std::optional<RenderQualityPreset> ParseQualityPreset(const std::string_view text) noexcept
{
    if (text == "low")
    {
        return RenderQualityPreset::Low;
    }
    if (text == "medium")
    {
        return RenderQualityPreset::Medium;
    }
    if (text == "high")
    {
        return RenderQualityPreset::High;
    }
    if (text == "epic")
    {
        return RenderQualityPreset::Epic;
    }
    return std::nullopt;
}
}
