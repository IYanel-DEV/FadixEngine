// Pure checks for SSAO quality mapping and the sampleable-depth-format fallback
// selection. No GPU/window. Exits non-zero on failure.

#include "engine/render/RenderQuality.hpp"
#include "render/Ssao.hpp"

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
}

int main()
{
    using namespace fadix;

    std::cout << "SSAO quality mapping\n";
    const ssao::SsaoSettings low = ssao::FromQuality(MakeQualitySettings(RenderQualityPreset::Low));
    const ssao::SsaoSettings med = ssao::FromQuality(MakeQualitySettings(RenderQualityPreset::Medium));
    const ssao::SsaoSettings high = ssao::FromQuality(MakeQualitySettings(RenderQualityPreset::High));
    const ssao::SsaoSettings epic = ssao::FromQuality(MakeQualitySettings(RenderQualityPreset::Epic));

    Check(!low.Enabled, "Low disables SSAO");
    Check(med.Enabled && med.HalfResolution && med.SampleCount > 0 && med.SampleCount <= high.SampleCount,
        "Medium: half-res, low sample count");
    Check(high.Enabled && high.HalfResolution && high.BilateralBlur && high.SampleCount > med.SampleCount,
        "High: half-res, more samples, bilateral blur");
    Check(epic.Enabled && !epic.HalfResolution && epic.SampleCount >= high.SampleCount,
        "Epic: full-resolution, highest sample count");

    std::cout << "Depth-format fallback selection\n";
    // Preference order is D32 -> D24 -> D16.
    Check(ssao::SelectSampleableDepthFormat({true, true, true}) == rhi::Format::D32Float,
        "all supported -> D32 (highest precision)");
    Check(ssao::SelectSampleableDepthFormat({false, true, true}) == rhi::Format::D24UnormS8Uint,
        "no D32 -> D24");
    Check(ssao::SelectSampleableDepthFormat({false, false, true}) == rhi::Format::D16Unorm,
        "only D16 -> D16 fallback");
    Check(!ssao::SelectSampleableDepthFormat({false, false, false}).has_value(),
        "none supported -> no sampleable depth (caller falls back / disables)");

    if (g_Failures != 0)
    {
        std::cerr << g_Failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All SSAO checks passed\n";
    return 0;
}
