// Headless checks for the pure render-quality preset construction. No GPU, no
// window: verifies the four tiers carry the intended caps, that the tiers are
// monotone where they should be, and that preset tokens round-trip through the
// persistence helpers. Exits non-zero if any check fails.

#include "engine/render/RenderQuality.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace
{
int g_Failures = 0;

void Check(const bool condition, const std::string& label)
{
    if (condition)
    {
        std::cout << "  ok   " << label << '\n';
    }
    else
    {
        std::cerr << "  FAIL " << label << '\n';
        ++g_Failures;
    }
}

bool NearlyEqual(const float a, const float b)
{
    return std::fabs(a - b) < 1.0e-4F;
}
}

int main()
{
    using namespace fadix;

    const RenderQualitySettings low = MakeQualitySettings(RenderQualityPreset::Low);
    const RenderQualitySettings medium = MakeQualitySettings(RenderQualityPreset::Medium);
    const RenderQualitySettings high = MakeQualitySettings(RenderQualityPreset::High);
    const RenderQualitySettings epic = MakeQualitySettings(RenderQualityPreset::Epic);

    std::cout << "Render quality presets\n";

    // Suggested starting presets from the spec.
    Check(NearlyEqual(low.ResolutionScale, 0.70F), "Low resolution scale 70%");
    Check(low.ShadowResolutionCap == 1024, "Low shadow cap 1024");
    Check(low.MaxPointLights == 4 && low.MaxSpotLights == 4, "Low 4 point / 4 spot");
    Check(low.MaxBloomPasses == 2, "Low 2 bloom passes");

    Check(NearlyEqual(medium.ResolutionScale, 0.85F), "Medium resolution scale 85%");
    Check(medium.ShadowResolutionCap == 2048, "Medium shadow cap 2048");
    Check(medium.MaxPointLights == 6 && medium.MaxSpotLights == 6, "Medium 6 point / 6 spot");
    Check(medium.MaxBloomPasses == 4, "Medium 4 bloom passes");

    Check(NearlyEqual(high.ResolutionScale, 1.0F), "High resolution scale 100%");
    Check(high.ShadowResolutionCap == 2048, "High shadow cap 2048");
    Check(high.MaxPointLights == 8 && high.MaxSpotLights == 8, "High 8 point / 8 spot");
    Check(high.MaxBloomPasses == 5, "High 5 bloom passes");

    Check(NearlyEqual(epic.ResolutionScale, 1.0F), "Epic resolution scale 100%");
    Check(epic.ShadowResolutionCap == 4096, "Epic shadow cap 4096");
    Check(epic.MaxPointLights == 8 && epic.MaxSpotLights == 8, "Epic 8 point / 8 spot");
    Check(epic.MaxBloomPasses == 6, "Epic 6 bloom passes");

    // Real cascaded shadows: cascade count scales with the tier.
    Check(low.ShadowCascadeCount == 1, "Low 1 cascade");
    Check(medium.ShadowCascadeCount == 2, "Medium 2 cascades");
    Check(high.ShadowCascadeCount == 4 && epic.ShadowCascadeCount == 4, "High/Epic 4 cascades");
    Check(low.ShadowFilterRadius == 1 && high.ShadowFilterRadius >= 2 &&
              epic.ShadowFilterRadius >= high.ShadowFilterRadius,
        "PCF radius widens Low->Epic");
    Check(epic.ShadowResolutionCap == 4096, "Epic device-safe max shadow cap");

    // SSAO does not composite into ambient yet: disabled in every tier.
    Check(!low.AoEnabled && !medium.AoEnabled && !high.AoEnabled && !epic.AoEnabled,
        "SSAO disabled in all tiers (not composited yet)");

    // Local (spot/point) shadow maps are not rendered/sampled yet: budget 0.
    Check(low.SpotShadowBudget == 0 && medium.SpotShadowBudget == 0 &&
              high.SpotShadowBudget == 0 && epic.SpotShadowBudget == 0,
        "spot shadow budget 0 in all tiers (not rendered yet)");
    Check(low.PointShadowBudget == 0 && medium.PointShadowBudget == 0 &&
              high.PointShadowBudget == 0 && epic.PointShadowBudget == 0,
        "point shadow budget 0 in all tiers (not rendered yet)");

    // Every tier keeps post-processing on so tonemap/exposure/colors are shared.
    Check(low.PostProcessingEnabled && high.PostProcessingEnabled, "post-processing stays enabled");

    // Light caps never exceed the shader array size (8).
    Check(epic.MaxPointLights <= 8 && epic.MaxSpotLights <= 8, "light caps within shader limit");

    // Monotone (non-decreasing) detail as tiers rise.
    Check(low.ResolutionScale <= medium.ResolutionScale &&
              medium.ResolutionScale <= high.ResolutionScale &&
              high.ResolutionScale <= epic.ResolutionScale,
        "resolution scale is non-decreasing");
    Check(low.ShadowResolutionCap <= epic.ShadowResolutionCap, "shadow cap grows Low->Epic");
    Check(low.AnisotropyLevel <= high.AnisotropyLevel, "anisotropy grows Low->High");

    // Persistence tokens round-trip.
    const RenderQualityPreset all[] = {RenderQualityPreset::Low, RenderQualityPreset::Medium,
        RenderQualityPreset::High, RenderQualityPreset::Epic};
    for (const RenderQualityPreset preset : all)
    {
        const auto parsed = ParseQualityPreset(ToString(preset));
        Check(parsed.has_value() && *parsed == preset,
            std::string{"round-trip "} + ToString(preset));
    }
    Check(!ParseQualityPreset("ultra").has_value(), "unknown token rejected");

    // TAA is temporarily disabled until stationary/motion stability tests pass;
    // High/Epic use stable FXAA for now.
    Check(low.DefaultAntiAlias == AntiAliasMode::Off, "low AA off");
    Check(medium.DefaultAntiAlias == AntiAliasMode::Fxaa, "medium AA fxaa");
    Check(high.DefaultAntiAlias == AntiAliasMode::Fxaa, "high AA fxaa (TAA disabled)");
    Check(epic.DefaultAntiAlias == AntiAliasMode::Fxaa, "epic AA fxaa (TAA disabled)");
    Check(ResolveAntiAlias(AntiAliasMode::Auto, high.DefaultAntiAlias) == AntiAliasMode::Fxaa, "auto->fxaa");
    Check(ResolveAntiAlias(AntiAliasMode::Off, high.DefaultAntiAlias) == AntiAliasMode::Off, "override off");

    if (g_Failures != 0)
    {
        std::cerr << g_Failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All render-quality checks passed\n";
    return 0;
}
