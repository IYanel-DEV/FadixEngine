#include "editor/imgui/GraphicsPreferences.hpp"

#include "engine/camera/EditorMode.hpp"
#include "project/ProjectJson.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace fadix::editor
{
namespace
{
[[nodiscard]] const char* ToAntiAliasToken(const AntiAliasMode mode) noexcept
{
    switch (mode)
    {
    case AntiAliasMode::Auto: return "auto";
    case AntiAliasMode::Off: return "off";
    case AntiAliasMode::Fxaa: return "fxaa";
    case AntiAliasMode::Taa: return "taa";
    }
    return "auto";
}

[[nodiscard]] std::optional<AntiAliasMode> ParseAntiAliasToken(const std::string_view text) noexcept
{
    if (text == "auto")
    {
        return AntiAliasMode::Auto;
    }
    if (text == "off")
    {
        return AntiAliasMode::Off;
    }
    if (text == "fxaa")
    {
        return AntiAliasMode::Fxaa;
    }
    if (text == "taa")
    {
        return AntiAliasMode::Taa;
    }
    return std::nullopt;
}

[[nodiscard]] int ReadCap(const project_json::Value& object, const char* key) noexcept
{
    if (!object.Contains(key))
    {
        return 0;
    }
    const auto& value = object.at(key);
    if (value.GetType() == project_json::Value::Type::Number)
    {
        return static_cast<int>(std::lround(value.AsNumber()));
    }
    return 0;
}
}

GraphicsPreferences GraphicsPreferences::Defaults() noexcept
{
    return GraphicsPreferences{};
}

RenderQualitySettings GraphicsPreferences::ApplyTo(const RenderQualityPreset preset) const noexcept
{
    RenderQualitySettings settings = MakeQualitySettings(preset);
    if (ResolutionScaleOverride > 0.0F)
    {
        settings.ResolutionScale = ResolutionScaleOverride;
    }
    if (ShadowCascadeCap > 0)
    {
        settings.ShadowCascadeCount = std::min(settings.ShadowCascadeCount, ShadowCascadeCap);
    }
    if (ShadowResolutionCap > 0)
    {
        settings.ShadowResolutionCap = std::min(settings.ShadowResolutionCap, ShadowResolutionCap);
    }
    if (SpotShadowBudgetCap > 0)
    {
        settings.SpotShadowBudget = std::min(settings.SpotShadowBudget, SpotShadowBudgetCap);
    }
    if (PointShadowBudgetCap > 0)
    {
        settings.PointShadowBudget = std::min(settings.PointShadowBudget, PointShadowBudgetCap);
    }
    return settings;
}

bool ParseGraphicsPreferences(const std::string_view json, GraphicsPreferences& out)
{
    const auto parsed = project_json::Parse(json);
    if (!parsed || !parsed->IsObject())
    {
        return false;
    }
    GraphicsPreferences prefs = GraphicsPreferences::Defaults();
    const project_json::Value& object = *parsed;

    if (object.Contains("sceneQuality") && object.at("sceneQuality").IsString())
    {
        if (const auto preset = ParseQualityPreset(object.at("sceneQuality").AsString()))
        {
            prefs.SceneQuality = *preset;
        }
    }
    if (object.Contains("gameQuality") && object.at("gameQuality").IsString())
    {
        if (const auto preset = ParseQualityPreset(object.at("gameQuality").AsString()))
        {
            prefs.GameQuality = *preset;
        }
    }
    if (object.Contains("resolutionScaleOverride") &&
        object.at("resolutionScaleOverride").GetType() == project_json::Value::Type::Number)
    {
        prefs.ResolutionScaleOverride = static_cast<float>(object.at("resolutionScaleOverride").AsNumber());
    }
    if (object.Contains("aaOverride") && object.at("aaOverride").IsString())
    {
        if (const auto mode = ParseAntiAliasToken(object.at("aaOverride").AsString()))
        {
            prefs.AaOverride = *mode;
        }
    }
    prefs.ShadowCascadeCap = ReadCap(object, "shadowCascadeCap");
    prefs.ShadowResolutionCap = ReadCap(object, "shadowResolutionCap");
    prefs.SpotShadowBudgetCap = ReadCap(object, "spotShadowBudgetCap");
    prefs.PointShadowBudgetCap = ReadCap(object, "pointShadowBudgetCap");
    if (object.Contains("viewportProjection") && object.at("viewportProjection").IsString())
    {
        const std::string_view tok = object.at("viewportProjection").AsString();
        prefs.ProjectionMode = (tok == "2d") ? ViewportProjectionMode::Ortho2D
                                             : ViewportProjectionMode::Perspective;
    }

    out = prefs;
    return true;
}

std::string StringifyGraphicsPreferences(const GraphicsPreferences& prefs)
{
    project_json::Value document = project_json::Value::MakeObject();
    document["sceneQuality"] = project_json::Value::MakeString(ToString(prefs.SceneQuality));
    document["gameQuality"] = project_json::Value::MakeString(ToString(prefs.GameQuality));
    document["resolutionScaleOverride"] =
        project_json::Value::MakeNumber(static_cast<double>(prefs.ResolutionScaleOverride));
    document["aaOverride"] = project_json::Value::MakeString(ToAntiAliasToken(prefs.AaOverride));
    document["shadowCascadeCap"] = project_json::Value::MakeNumber(prefs.ShadowCascadeCap);
    document["shadowResolutionCap"] = project_json::Value::MakeNumber(prefs.ShadowResolutionCap);
    document["spotShadowBudgetCap"] = project_json::Value::MakeNumber(prefs.SpotShadowBudgetCap);
    document["pointShadowBudgetCap"] = project_json::Value::MakeNumber(prefs.PointShadowBudgetCap);
    document["viewportProjection"] = project_json::Value::MakeString(
        prefs.ProjectionMode == ViewportProjectionMode::Ortho2D ? "2d" : "3d");
    return project_json::Stringify(document);
}
}
