#include "render/Ssao.hpp"

#include <algorithm>

namespace fadix::ssao
{
SsaoSettings FromQuality(const RenderQualitySettings& quality) noexcept
{
    SsaoSettings settings;
    settings.Enabled = quality.AoEnabled;
    settings.HalfResolution = quality.AoHalfResolution;
    settings.SampleCount = std::clamp(quality.AoSampleCount, 0, 64);
    settings.BilateralBlur = quality.AoBilateralBlur;
    return settings;
}

std::array<rhi::Format, 3> DepthFormatPreference() noexcept
{
    // Highest precision first, most-widely-supported fallback last.
    return {rhi::Format::D32Float, rhi::Format::D24UnormS8Uint, rhi::Format::D16Unorm};
}

std::optional<rhi::Format> SelectSampleableDepthFormat(const std::array<bool, 3>& supported) noexcept
{
    const std::array<rhi::Format, 3> order = DepthFormatPreference();
    for (std::size_t i = 0; i < order.size(); ++i)
    {
        if (supported[i])
        {
            return order[i];
        }
    }
    return std::nullopt;
}
}
