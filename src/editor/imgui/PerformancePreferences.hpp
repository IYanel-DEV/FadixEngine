#pragma once

#include <string>
#include <string_view>

namespace fadix::editor
{
enum class PerformancePreset
{
    LowSpec,
    Balanced,
    FullQuality
};

struct PerformancePreferences
{
    PerformancePreset ActivePreset{PerformancePreset::Balanced};

    float FpsForeground{60.0F};   // 0 = unlimited
    float FpsUnfocused{30.0F};
    float FpsMinimized{5.0F};

    // Background-work budget: max items processed per frame
    int ThumbnailsPerFrame{2};
    int ImportsPolledPerFrame{4};

    [[nodiscard]] static PerformancePreferences Defaults() noexcept;
    [[nodiscard]] static PerformancePreferences ForPreset(PerformancePreset preset) noexcept;
    void ApplyPreset(PerformancePreset preset) noexcept;
};

[[nodiscard]] bool ParsePerformancePreferences(
    std::string_view json, PerformancePreferences& out);
[[nodiscard]] std::string StringifyPerformancePreferences(
    const PerformancePreferences& prefs);
[[nodiscard]] const char* ToString(PerformancePreset preset) noexcept;
}
