#include "editor/imgui/PerformancePreferences.hpp"

#include "project/ProjectJson.hpp"

#include <cmath>
#include <optional>

namespace fadix::editor
{
namespace
{
[[nodiscard]] std::optional<PerformancePreset> ParsePreset(const std::string_view text) noexcept
{
    if (text == "lowspec") return PerformancePreset::LowSpec;
    if (text == "balanced") return PerformancePreset::Balanced;
    if (text == "fullquality") return PerformancePreset::FullQuality;
    return std::nullopt;
}

[[nodiscard]] const char* PresetToken(const PerformancePreset preset) noexcept
{
    switch (preset)
    {
    case PerformancePreset::LowSpec: return "lowspec";
    case PerformancePreset::Balanced: return "balanced";
    case PerformancePreset::FullQuality: return "fullquality";
    }
    return "balanced";
}
}

const char* ToString(const PerformancePreset preset) noexcept
{
    switch (preset)
    {
    case PerformancePreset::LowSpec: return "Low Spec";
    case PerformancePreset::Balanced: return "Balanced";
    case PerformancePreset::FullQuality: return "Full Quality";
    }
    return "Balanced";
}

PerformancePreferences PerformancePreferences::Defaults() noexcept
{
    return ForPreset(PerformancePreset::Balanced);
}

PerformancePreferences PerformancePreferences::ForPreset(
    const PerformancePreset preset) noexcept
{
    PerformancePreferences p;
    p.ActivePreset = preset;
    switch (preset)
    {
    case PerformancePreset::LowSpec:
        p.FpsForeground = 45.0F;
        p.FpsUnfocused = 15.0F;
        p.FpsMinimized = 5.0F;
        p.ThumbnailsPerFrame = 1;
        p.ImportsPolledPerFrame = 2;
        break;
    case PerformancePreset::Balanced:
        p.FpsForeground = 60.0F;
        p.FpsUnfocused = 30.0F;
        p.FpsMinimized = 5.0F;
        p.ThumbnailsPerFrame = 2;
        p.ImportsPolledPerFrame = 4;
        break;
    case PerformancePreset::FullQuality:
        p.FpsForeground = 0.0F; // unlimited
        p.FpsUnfocused = 60.0F;
        p.FpsMinimized = 15.0F;
        p.ThumbnailsPerFrame = 4;
        p.ImportsPolledPerFrame = 8;
        break;
    }
    return p;
}

void PerformancePreferences::ApplyPreset(const PerformancePreset preset) noexcept
{
    *this = ForPreset(preset);
}

bool ParsePerformancePreferences(
    const std::string_view json, PerformancePreferences& out)
{
    const auto parsed = project_json::Parse(json);
    if (!parsed || !parsed->IsObject())
    {
        return false;
    }
    const project_json::Value& obj = *parsed;
    PerformancePreferences p = PerformancePreferences::Defaults();

    if (obj.Contains("preset") && obj.at("preset").IsString())
    {
        if (const auto preset = ParsePreset(obj.at("preset").AsString()))
        {
            p.ActivePreset = *preset;
        }
    }
    auto readFloat = [&](const char* key, float& field) {
        if (obj.Contains(key) &&
            obj.at(key).GetType() == project_json::Value::Type::Number)
        {
            field = static_cast<float>(obj.at(key).AsNumber());
        }
    };
    auto readInt = [&](const char* key, int& field) {
        if (obj.Contains(key) &&
            obj.at(key).GetType() == project_json::Value::Type::Number)
        {
            field = static_cast<int>(std::lround(obj.at(key).AsNumber()));
        }
    };
    readFloat("fpsForeground", p.FpsForeground);
    readFloat("fpsUnfocused", p.FpsUnfocused);
    readFloat("fpsMinimized", p.FpsMinimized);
    readInt("thumbnailsPerFrame", p.ThumbnailsPerFrame);
    readInt("importsPolledPerFrame", p.ImportsPolledPerFrame);

    out = p;
    return true;
}

std::string StringifyPerformancePreferences(const PerformancePreferences& prefs)
{
    project_json::Value doc = project_json::Value::MakeObject();
    doc["preset"] = project_json::Value::MakeString(PresetToken(prefs.ActivePreset));
    doc["fpsForeground"] = project_json::Value::MakeNumber(
        static_cast<double>(prefs.FpsForeground));
    doc["fpsUnfocused"] = project_json::Value::MakeNumber(
        static_cast<double>(prefs.FpsUnfocused));
    doc["fpsMinimized"] = project_json::Value::MakeNumber(
        static_cast<double>(prefs.FpsMinimized));
    doc["thumbnailsPerFrame"] = project_json::Value::MakeNumber(prefs.ThumbnailsPerFrame);
    doc["importsPolledPerFrame"] = project_json::Value::MakeNumber(
        prefs.ImportsPolledPerFrame);
    return project_json::Stringify(doc);
}
}
