#include "assets/MaterialAssetJson.hpp"

#include "project/ProjectJson.hpp"

#include <fstream>
#include <sstream>

namespace fadix
{
namespace
{
using project_json::Value;

Value Vec4Json(const glm::vec4& value)
{
    Value array = Value::MakeArray();
    array.Push(Value::MakeNumber(value.x));
    array.Push(Value::MakeNumber(value.y));
    array.Push(Value::MakeNumber(value.z));
    array.Push(Value::MakeNumber(value.w));
    return array;
}

Value Vec3Json(const glm::vec3& value)
{
    Value array = Value::MakeArray();
    array.Push(Value::MakeNumber(value.x));
    array.Push(Value::MakeNumber(value.y));
    array.Push(Value::MakeNumber(value.z));
    return array;
}

Value Vec2Json(const glm::vec2& value)
{
    Value array = Value::MakeArray();
    array.Push(Value::MakeNumber(value.x));
    array.Push(Value::MakeNumber(value.y));
    return array;
}

glm::vec4 ReadVec4(const Value& value, const glm::vec4& fallback)
{
    if (!value.IsArray() || value.Array().size() < 4U)
    {
        return fallback;
    }
    const auto& array = value.Array();
    return {
        static_cast<float>(array[0].AsNumber()),
        static_cast<float>(array[1].AsNumber()),
        static_cast<float>(array[2].AsNumber()),
        static_cast<float>(array[3].AsNumber())};
}

glm::vec3 ReadVec3(const Value& value, const glm::vec3& fallback)
{
    if (!value.IsArray() || value.Array().size() < 3U)
    {
        return fallback;
    }
    const auto& array = value.Array();
    return {
        static_cast<float>(array[0].AsNumber()),
        static_cast<float>(array[1].AsNumber()),
        static_cast<float>(array[2].AsNumber())};
}

glm::vec2 ReadVec2(const Value& value, const glm::vec2& fallback)
{
    if (!value.IsArray() || value.Array().size() < 2U)
    {
        return fallback;
    }
    const auto& array = value.Array();
    return {static_cast<float>(array[0].AsNumber()), static_cast<float>(array[1].AsNumber())};
}

AssetHandle ReadHandleField(const Value& object, const char* key)
{
    if (!object.Contains(key))
    {
        return InvalidAssetHandle;
    }
    return Uuid::Parse(object.at(key).AsString()).value_or(InvalidAssetHandle);
}

AlphaMode ReadAlphaMode(const Value& object)
{
    if (!object.Contains("AlphaMode"))
    {
        return AlphaMode::Opaque;
    }
    const std::string mode = object.at("AlphaMode").AsString();
    if (mode == "Mask")
    {
        return AlphaMode::Mask;
    }
    if (mode == "Blend")
    {
        return AlphaMode::Blend;
    }
    return AlphaMode::Opaque;
}
} // namespace

MaterialAsset DefaultMaterialAsset(const AssetHandle handle, std::string name)
{
    MaterialAsset asset;
    asset.Handle = handle;
    asset.Name = std::move(name);
    return asset;
}

Result<MaterialAsset> LoadMaterialAsset(const std::filesystem::path& path, const AssetHandle handle)
{
    std::ifstream in{path};
    if (!in)
    {
        return Result<MaterialAsset>::Error("Could not open material file: " + path.string());
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    const auto json = project_json::Parse(buffer.str());
    if (!json || !json->IsObject())
    {
        return Result<MaterialAsset>::Error("Material file is not valid JSON: " + path.string());
    }

    MaterialAsset asset = DefaultMaterialAsset(handle, path.stem().string());
    const Value& object = *json;
    if (object.Contains("Version") &&
        static_cast<std::uint32_t>(object.at("Version").AsNumber()) > MaterialFileVersion)
    {
        return Result<MaterialAsset>::Error("Unsupported material version in " + path.string());
    }
    if (object.Contains("Name"))
    {
        asset.Name = object.at("Name").AsString();
    }
    if (object.Contains("BaseColor"))
    {
        asset.BaseColor = ReadVec4(object.at("BaseColor"), asset.BaseColor);
    }
    asset.BaseColorTexture = ReadHandleField(object, "BaseColorTexture");
    asset.NormalTexture = ReadHandleField(object, "NormalTexture");
    if (object.Contains("MetallicFactor"))
    {
        asset.Metallic = static_cast<float>(object.at("MetallicFactor").AsNumber());
    }
    else if (object.Contains("Metallic"))
    {
        asset.Metallic = static_cast<float>(object.at("Metallic").AsNumber());
    }
    if (object.Contains("RoughnessFactor"))
    {
        asset.Roughness = static_cast<float>(object.at("RoughnessFactor").AsNumber());
    }
    else if (object.Contains("Roughness"))
    {
        asset.Roughness = static_cast<float>(object.at("Roughness").AsNumber());
    }
    asset.MetallicRoughnessTexture = ReadHandleField(object, "MetallicRoughnessTexture");
    if (object.Contains("EmissiveFactor"))
    {
        asset.EmissiveColor = ReadVec3(object.at("EmissiveFactor"), asset.EmissiveColor);
    }
    else if (object.Contains("EmissiveColor"))
    {
        asset.EmissiveColor = ReadVec3(object.at("EmissiveColor"), asset.EmissiveColor);
    }
    if (object.Contains("EmissiveIntensity"))
    {
        asset.EmissiveIntensity = static_cast<float>(object.at("EmissiveIntensity").AsNumber());
    }
    asset.EmissiveTexture = ReadHandleField(object, "EmissiveTexture");
    // Occlusion + normal scale (added later; absent -> neutral defaults so old
    // material files keep rendering identically).
    asset.OcclusionTexture = ReadHandleField(object, "OcclusionTexture");
    if (object.Contains("OcclusionStrength"))
    {
        asset.OcclusionStrength = static_cast<float>(object.at("OcclusionStrength").AsNumber());
    }
    if (object.Contains("NormalScale"))
    {
        asset.NormalScale = static_cast<float>(object.at("NormalScale").AsNumber());
    }
    asset.AlphaMode = ReadAlphaMode(object);
    if (object.Contains("AlphaCutoff"))
    {
        asset.AlphaCutoff = static_cast<float>(object.at("AlphaCutoff").AsNumber());
    }
    if (object.Contains("DoubleSided"))
    {
        asset.DoubleSided = object.at("DoubleSided").AsBool();
    }
    if (object.Contains("UVScale"))
    {
        asset.UVScale = ReadVec2(object.at("UVScale"), asset.UVScale);
    }
    if (object.Contains("UVOffset"))
    {
        asset.UVOffset = ReadVec2(object.at("UVOffset"), asset.UVOffset);
    }

    asset.Metallic = std::clamp(asset.Metallic, 0.0F, 1.0F);
    asset.Roughness = std::clamp(asset.Roughness, 0.0F, 1.0F);
    asset.AlphaCutoff = std::clamp(asset.AlphaCutoff, 0.0F, 1.0F);
    asset.EmissiveIntensity = std::max(asset.EmissiveIntensity, 0.0F);
    asset.OcclusionStrength = std::clamp(asset.OcclusionStrength, 0.0F, 1.0F);
    asset.NormalScale = std::clamp(asset.NormalScale, 0.0F, 8.0F);
    return Result<MaterialAsset>::Ok(std::move(asset));
}

Result<void> SaveMaterialAsset(const std::filesystem::path& path, const MaterialAsset& asset)
{
    Value object = Value::MakeObject();
    object["Version"] = Value::MakeNumber(MaterialFileVersion);
    object["Name"] = Value::MakeString(asset.Name);
    object["BaseColor"] = Vec4Json(asset.BaseColor);
    if (asset.BaseColorTexture.IsValid())
    {
        object["BaseColorTexture"] = Value::MakeString(asset.BaseColorTexture.ToString());
    }
    if (asset.NormalTexture.IsValid())
    {
        object["NormalTexture"] = Value::MakeString(asset.NormalTexture.ToString());
    }
    object["MetallicFactor"] = Value::MakeNumber(asset.Metallic);
    object["RoughnessFactor"] = Value::MakeNumber(asset.Roughness);
    if (asset.MetallicRoughnessTexture.IsValid())
    {
        object["MetallicRoughnessTexture"] =
            Value::MakeString(asset.MetallicRoughnessTexture.ToString());
    }
    object["EmissiveFactor"] = Vec3Json(asset.EmissiveColor);
    object["EmissiveIntensity"] = Value::MakeNumber(asset.EmissiveIntensity);
    if (asset.EmissiveTexture.IsValid())
    {
        object["EmissiveTexture"] = Value::MakeString(asset.EmissiveTexture.ToString());
    }
    if (asset.OcclusionTexture.IsValid())
    {
        object["OcclusionTexture"] = Value::MakeString(asset.OcclusionTexture.ToString());
    }
    object["OcclusionStrength"] = Value::MakeNumber(asset.OcclusionStrength);
    object["NormalScale"] = Value::MakeNumber(asset.NormalScale);
    object["AlphaMode"] = Value::MakeString(
        asset.AlphaMode == AlphaMode::Mask ? "Mask"
            : asset.AlphaMode == AlphaMode::Blend ? "Blend"
                                                  : "Opaque");
    object["AlphaCutoff"] = Value::MakeNumber(asset.AlphaCutoff);
    object["DoubleSided"] = Value::MakeBool(asset.DoubleSided);
    object["UVScale"] = Vec2Json(asset.UVScale);
    object["UVOffset"] = Vec2Json(asset.UVOffset);

    std::ofstream out{path, std::ios::trunc};
    if (!out)
    {
        return Result<void>::Error("Could not open material file for writing: " + path.string());
    }
    out << project_json::Stringify(object);
    return Result<void>::Ok();
}

} // namespace fadix
