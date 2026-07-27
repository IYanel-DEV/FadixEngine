#include "editor/scene/EntityTextIO.hpp"

#include "engine/scene/IWorld.hpp"
#include "runtime/Components.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fadix
{
namespace
{
template <typename Component>
bool Has(const entt::registry& registry, const entt::entity entity)
{
    return registry.all_of<Component>(entity);
}
}

Result<void> AtomicReplaceFile(
    const std::filesystem::path& temporary, const std::filesystem::path& destination)
{
#ifdef _WIN32
    if (MoveFileExW(temporary.wstring().c_str(), destination.wstring().c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
    {
        std::error_code cleanup;
        std::filesystem::remove(temporary, cleanup);
        return Result<void>::Error("Could not replace scene file: " + destination.string());
    }
    return Result<void>::Ok();
#else
    // POSIX rename() atomically replaces an existing destination on the same
    // filesystem; the original survives until the new file is fully in place.
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error)
    {
        std::error_code cleanup;
        std::filesystem::remove(temporary, cleanup);
        return Result<void>::Error("Could not replace scene: " + error.message());
    }
    return Result<void>::Ok();
#endif
}

void CopyEntityComponents(
    IWorld& destination,
    entt::entity destinationEntity,
    const IWorld& source,
    entt::entity sourceEntity)
{
    const entt::registry& from = source.Registry();
    entt::registry& to = destination.Registry();
    if (const auto* value = from.try_get<NameComponent>(sourceEntity))
    {
        to.emplace<NameComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<TransformComponent>(sourceEntity))
    {
        to.emplace<TransformComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<RelationshipComponent>(sourceEntity))
    {
        to.emplace<RelationshipComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<MeshComponent>(sourceEntity))
    {
        to.emplace<MeshComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<DirectionalLightComponent>(sourceEntity))
    {
        to.emplace<DirectionalLightComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<PointLightComponent>(sourceEntity))
    {
        to.emplace<PointLightComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<SpotLightComponent>(sourceEntity))
    {
        to.emplace<SpotLightComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<EnvironmentComponent>(sourceEntity))
    {
        to.emplace<EnvironmentComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<VisibilityComponent>(sourceEntity))
    {
        to.emplace<VisibilityComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<CameraComponent>(sourceEntity))
    {
        to.emplace<CameraComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<NetworkIdentityComponent>(sourceEntity))
    {
        to.emplace<NetworkIdentityComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<JoltBodyComponent>(sourceEntity))
    {
        to.emplace<JoltBodyComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<CharacterControllerComponent>(sourceEntity))
    {
        to.emplace<CharacterControllerComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<Box2DBodyComponent>(sourceEntity))
    {
        to.emplace<Box2DBodyComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<ScriptComponent>(sourceEntity))
    {
        to.emplace<ScriptComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<ParticleEmitterComponent>(sourceEntity))
    {
        to.emplace<ParticleEmitterComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<AudioSourceComponent>(sourceEntity))
    {
        AudioSourceComponent copy = *value;
        copy.ActiveTrack = -1;
        to.emplace<AudioSourceComponent>(destinationEntity, copy);
    }
    if (const auto* value = from.try_get<AudioListenerComponent>(sourceEntity))
    {
        to.emplace<AudioListenerComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<UICanvasComponent>(sourceEntity))
    {
        to.emplace<UICanvasComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<TerrainComponent>(sourceEntity))
    {
        to.emplace<TerrainComponent>(destinationEntity, *value);
    }
    if (const auto* value = from.try_get<AnimatorComponent>(sourceEntity))
    {
        AnimatorComponent copy = *value;
        copy.ClipIndex = -1;
        copy.CurrentTime = 0.0F;
        to.emplace<AnimatorComponent>(destinationEntity, std::move(copy));
    }
}

void CopyWorldInto(const IWorld& source, IWorld& destination)
{
    const entt::registry& from = source.Registry();
    for (const auto [entity, uuid] : from.view<const UuidComponent>().each())
    {
        const entt::entity created = destination.Create(uuid.Id);
        CopyEntityComponents(destination, created, source, entity);
    }
}

void WriteEntityLine(
    std::ostream& out,
    const IWorld& world,
    entt::entity entity,
    std::optional<Uuid> parentOverride)
{
    const entt::registry& registry = world.Registry();
    const UuidComponent* uuid = registry.try_get<UuidComponent>(entity);
    if (!uuid)
    {
        return;
    }
    const NameComponent* name = registry.try_get<NameComponent>(entity);
    const TransformComponent* transform = registry.try_get<TransformComponent>(entity);
    out << "ENTITY " << uuid->Id.ToString() << ' ' << std::quoted(name ? name->Name : "Entity") << ' '
        << Has<TransformComponent>(registry, entity) << ' ';
    if (transform)
    {
        out << transform->Position.x << ' ' << transform->Position.y << ' ' << transform->Position.z << ' '
            << transform->Rotation.w << ' ' << transform->Rotation.x << ' ' << transform->Rotation.y << ' '
            << transform->Rotation.z << ' ' << transform->Scale.x << ' ' << transform->Scale.y << ' '
            << transform->Scale.z << ' ';
    }
    std::string parentToken = "-";
    if (parentOverride.has_value())
    {
        parentToken = parentOverride->IsValid() ? parentOverride->ToString() : "-";
    }
    else if (const RelationshipComponent* relationship =
                 world.Registry().try_get<RelationshipComponent>(entity))
    {
        parentToken = relationship->Parent.ToString();
    }
    out << parentToken << ' ';
    if (const MeshComponent* mesh = registry.try_get<MeshComponent>(entity))
    {
        out << "M " << static_cast<unsigned>(mesh->Kind) << ' '
            << mesh->BaseColor.x << ' ' << mesh->BaseColor.y << ' ' << mesh->BaseColor.z << ' '
            << mesh->Metallic << ' ' << mesh->Roughness << ' '
            << (mesh->Material.IsValid() ? mesh->Material.ToString() : "-") << ' '
            << (mesh->ImportedMesh.IsValid() ? mesh->ImportedMesh.ToString() : "-") << ' ';
    }
    if (const DirectionalLightComponent* light = registry.try_get<DirectionalLightComponent>(entity))
    {
        out << "L " << light->Color.x << ' ' << light->Color.y << ' ' << light->Color.z << ' '
            << light->Intensity << ' ' << light->CastShadows << ' ' << light->ShadowBias << ' '
            << light->ShadowStrength << ' ' << light->ShadowMapResolution << ' '
            << light->ShadowDistance << ' ' << light->CascadeSplitLambda << ' '
            << light->ShadowSoftness << ' '
            << "LC " << static_cast<unsigned>(light->CelestialBody) << ' '
            << light->LinkOppositeSun << ' ';
    }
    if (const PointLightComponent* light = registry.try_get<PointLightComponent>(entity))
    {
        out << "P " << light->Color.x << ' ' << light->Color.y << ' ' << light->Color.z << ' '
            << light->Intensity << ' ' << light->Range << ' ' << light->FalloffExponent << ' '
            << light->Enabled << ' '
            << light->CastShadows << ' ' << light->ShadowBias << ' ' << light->ShadowResolution
            << ' ' << light->ShadowStrength << ' ' << light->Softness << ' ';
    }
    if (const SpotLightComponent* light = registry.try_get<SpotLightComponent>(entity))
    {
        out << "S " << light->Color.x << ' ' << light->Color.y << ' ' << light->Color.z << ' '
            << light->Intensity << ' ' << light->Range << ' ' << light->InnerConeDegrees << ' '
            << light->OuterConeDegrees << ' ' << light->FalloffExponent << ' '
            << light->Enabled << ' '
            << light->CastShadows << ' ' << light->ShadowBias << ' ' << light->ShadowResolution
            << ' ' << light->ShadowStrength << ' ' << light->Softness << ' ';
    }
    if (const EnvironmentComponent* env = registry.try_get<EnvironmentComponent>(entity))
    {
        out << "E " << env->SkyZenithColor.x << ' ' << env->SkyZenithColor.y << ' '
            << env->SkyZenithColor.z << ' ' << env->SkyHorizonColor.x << ' '
            << env->SkyHorizonColor.y << ' ' << env->SkyHorizonColor.z << ' '
            << env->GroundColor.x << ' ' << env->GroundColor.y << ' ' << env->GroundColor.z << ' '
            << env->AmbientColor.x << ' ' << env->AmbientColor.y << ' ' << env->AmbientColor.z << ' '
            << env->AmbientIntensity << ' ' << env->Exposure << ' ' << env->FogEnabled << ' '
            << env->FogColor.x << ' ' << env->FogColor.y << ' ' << env->FogColor.z << ' '
            << env->FogDensity << ' ' << env->FogStart << ' ' << env->FogEnd << ' '
            << env->Primary << ' ' << env->Priority << ' '
            << env->BloomEnabled << ' ' << env->BloomThreshold << ' '
            << env->BloomIntensity << ' ' << env->BloomPasses << ' '
            << env->TonemapEnabled << ' ' << env->ColorGradingEnabled << ' '
            << env->Lift.x << ' ' << env->Lift.y << ' ' << env->Lift.z << ' '
            << env->Gamma.x << ' ' << env->Gamma.y << ' ' << env->Gamma.z << ' '
            << env->Gain.x << ' ' << env->Gain.y << ' ' << env->Gain.z << ' '
            << env->FxaaEnabled << ' ' << env->FxaaStrength << ' '
            << static_cast<unsigned>(env->AntiAlias) << ' ' << env->TaaHistoryWeight << ' '
            << env->TaaSharpness << ' '
            << env->ExposureCompensationEV << ' ' << env->WhiteBalanceTemperature << ' '
            << env->WhiteBalanceTint << ' ' << env->Contrast << ' '
            << env->Saturation << ' ' << env->HighlightCompression << ' '
            << (env->EnvironmentMap.IsValid() ? env->EnvironmentMap.ToString() : "-") << ' '
            << env->EnvironmentRotation << ' ' << env->EnvironmentIntensity << ' '
            << env->UseEnvironmentAsSky << ' '
            << env->AoEnabled << ' ' << env->AoRadius << ' ' << env->AoIntensity << ' '
            << env->AoPower << ' ' << env->AoBias << ' ';
    }
    if (const VisibilityComponent* visibility = registry.try_get<VisibilityComponent>(entity))
    {
        out << "V " << visibility->VisibleInEditor << ' ' << visibility->VisibleInGame << ' '
            << visibility->Selectable << ' ';
    }
    if (const CameraComponent* camera = registry.try_get<CameraComponent>(entity))
    {
        out << "C " << camera->FieldOfView << ' ' << camera->NearPlane << ' ' << camera->FarPlane << ' '
            << camera->Primary << ' ';
    }
    if (const NetworkIdentityComponent* network = registry.try_get<NetworkIdentityComponent>(entity))
    {
        out << "N " << network->NetworkId << ' ' << static_cast<unsigned>(network->Authority) << ' '
            << network->Replicate << ' ';
    }
    if (const JoltBodyComponent* body = registry.try_get<JoltBodyComponent>(entity))
    {
        out << "J " << body->HalfExtent.x << ' ' << body->HalfExtent.y << ' ' << body->HalfExtent.z << ' '
            << body->Dynamic << ' '
            << "K " << static_cast<unsigned>(body->Shape) << ' ' << body->AutoFit << ' '
            << "JW " << body->Mass << ' '
            << "JF " << body->Friction << ' ';
    }
    if (const CharacterControllerComponent* character =
            registry.try_get<CharacterControllerComponent>(entity))
    {
        out << "CC " << character->Radius << ' ' << character->Height << ' '
            << character->StepHeight << ' ' << character->MaxSlopeDegrees << ' '
            << character->MoveSpeed << ' ' << character->JumpSpeed << ' '
            << character->Gravity << ' ' << character->AirControl << ' '
            << character->PushStrength << ' ';
    }
    if (const Box2DBodyComponent* body = registry.try_get<Box2DBodyComponent>(entity))
    {
        out << "B " << body->HalfExtent.x << ' ' << body->HalfExtent.y << ' ' << body->Dynamic << ' ';
    }
    if (const ScriptComponent* script = registry.try_get<ScriptComponent>(entity))
    {
        out << "Y " << script->ScriptNames.size() << ' ';
        for (const std::string& scriptName : script->ScriptNames)
        {
            out << std::quoted(scriptName) << ' ';
        }
        out << script->Enabled << ' '
            << (script->Target.IsValid() ? script->Target.ToString() : "-") << ' ';
    }
    if (const ParticleEmitterComponent* emitter = registry.try_get<ParticleEmitterComponent>(entity))
    {
        out << "PE " << emitter->Enabled << ' ' << emitter->MaxParticles << ' '
            << emitter->EmitRate << ' ' << emitter->LifetimeMin << ' ' << emitter->LifetimeMax << ' '
            << emitter->SpeedMin << ' ' << emitter->SpeedMax << ' '
            << static_cast<unsigned>(emitter->Shape) << ' ' << emitter->ShapeRadius << ' '
            << emitter->ConeAngle << ' ' << emitter->Direction.x << ' ' << emitter->Direction.y << ' '
            << emitter->Direction.z << ' ' << emitter->Gravity.x << ' ' << emitter->Gravity.y << ' '
            << emitter->Gravity.z << ' ' << emitter->ColorStart.x << ' ' << emitter->ColorStart.y << ' '
            << emitter->ColorStart.z << ' ' << emitter->ColorStart.w << ' ' << emitter->ColorEnd.x << ' '
            << emitter->ColorEnd.y << ' ' << emitter->ColorEnd.z << ' ' << emitter->ColorEnd.w << ' '
            << emitter->SizeStart << ' ' << emitter->SizeEnd << ' ' << emitter->PlayOnStart << ' '
            << emitter->Loop << ' ';
    }
    if (const AudioSourceComponent* audio = registry.try_get<AudioSourceComponent>(entity))
    {
        out << "AS " << (audio->Sound.IsValid() ? audio->Sound.ToString() : "-") << ' '
            << audio->PlayOnStart << ' ' << audio->Loop << ' ' << audio->Volume << ' '
            << audio->MinDistance << ' ' << audio->MaxDistance << ' ' << audio->Spatial << ' ';
    }
    if (const AudioListenerComponent* listener = registry.try_get<AudioListenerComponent>(entity))
    {
        out << "AL " << listener->Active << ' ';
    }
    if (const UICanvasComponent* ui = registry.try_get<UICanvasComponent>(entity))
    {
        out << "UC " << (ui->UIAsset.IsValid() ? ui->UIAsset.ToString() : "-") << ' '
            << (ui->StyleAsset.IsValid() ? ui->StyleAsset.ToString() : "-") << ' '
            << ui->RenderInEditor << ' ' << ui->RenderInGame << ' ' << ui->Order << ' '
            << ui->Scale << ' ';
    }
    if (const TerrainComponent* terrain = registry.try_get<TerrainComponent>(entity))
    {
        const int layerCount = std::clamp(terrain->LayerCount, 1, 4);
        out << "TR " << (terrain->Heightmap.IsValid() ? terrain->Heightmap.ToString() : "-") << ' '
            << terrain->Width << ' ' << terrain->Depth << ' ' << terrain->HeightScale << ' '
            << terrain->ResolutionX << ' ' << terrain->ResolutionZ << ' ' << layerCount << ' '
            << terrain->CastShadows << ' ';
        for (int i = 0; i < layerCount; ++i)
        {
            const TerrainLayer& layer = terrain->Layers[i];
            out << (layer.Texture.IsValid() ? layer.Texture.ToString() : "-") << ' ' << layer.Tiling
                << ' ' << layer.MinHeight << ' ' << layer.MaxHeight << ' ' << layer.MinSlope << ' '
                << layer.MaxSlope << ' ';
        }
    }
    if (const AnimatorComponent* animator = registry.try_get<AnimatorComponent>(entity))
    {
        out << "AN " << std::quoted(animator->ClipName) << ' ' << animator->Speed << ' '
            << animator->Loop << ' ' << animator->Playing << ' ';
    }
    out << "END\n";
}

Result<void> ParseEntityLine(std::string_view line, IWorld& world)
{
    std::istringstream row{std::string{line}};
    std::string marker;
    std::string uuidText;
    std::string name;
    bool hasTransform = false;
    if (!(row >> marker >> uuidText >> std::quoted(name) >> hasTransform) || marker != "ENTITY")
    {
        return Result<void>::Error("Malformed entity in scene");
    }
    const auto uuid = Uuid::Parse(uuidText);
    if (!uuid)
    {
        return Result<void>::Error("Invalid entity UUID in scene");
    }
    entt::entity entity{};
    try
    {
        entity = world.Create(*uuid);
    }
    catch (const std::exception& exception)
    {
        return Result<void>::Error(exception.what());
    }
    entt::registry& registry = world.Registry();
    registry.emplace<NameComponent>(entity, std::move(name));
    if (hasTransform)
    {
        TransformComponent value;
        row >> value.Position.x >> value.Position.y >> value.Position.z
            >> value.Rotation.w >> value.Rotation.x >> value.Rotation.y >> value.Rotation.z
            >> value.Scale.x >> value.Scale.y >> value.Scale.z;
        registry.emplace<TransformComponent>(entity, value);
    }
    std::string parent;
    row >> parent;
    if (parent != "-")
    {
        const auto parentId = Uuid::Parse(parent);
        if (!parentId)
        {
            return Result<void>::Error("Invalid parent UUID in scene");
        }
        registry.emplace<RelationshipComponent>(entity, *parentId);
    }
    while (row >> marker && marker != "END")
    {
        if (marker == "M")
        {
            MeshComponent value;
            unsigned kind = 0;
            row >> kind >> value.BaseColor.x >> value.BaseColor.y >> value.BaseColor.z
                >> value.Metallic >> value.Roughness;
            const std::streampos pos = row.tellg();
            std::string token;
            if (row >> token)
            {
                // "-" is an explicit empty handle (consumed). A following component
                // marker fails Uuid::Parse — seek back so older scenes without these
                // tokens still load.
                if (token != "-")
                {
                    if (const auto materialId = Uuid::Parse(token))
                    {
                        value.Material = *materialId;
                    }
                    else
                    {
                        row.clear();
                        row.seekg(pos);
                    }
                }
            }
            const std::streampos importedPos = row.tellg();
            std::string importedToken;
            if (row >> importedToken)
            {
                if (importedToken != "-")
                {
                    if (const auto importedId = Uuid::Parse(importedToken))
                    {
                        value.ImportedMesh = *importedId;
                    }
                    else
                    {
                        row.clear();
                        row.seekg(importedPos);
                    }
                }
            }
            // Unknown kinds from newer/older builds degrade to Cube.
            value.Kind = SanitizeMeshKind(kind);
            value.BaseColor = SanitizedColor(value.BaseColor, MeshComponent{}.BaseColor);
            value.Metallic = SanitizedFloat(value.Metallic, 0.0F, 1.0F, 0.05F);
            value.Roughness = SanitizedFloat(value.Roughness, 0.0F, 1.0F, 0.55F);
            registry.emplace<MeshComponent>(entity, value);
        }
        else if (marker == "L")
        {
            DirectionalLightComponent value;
            row >> value.Color.x >> value.Color.y >> value.Color.z >> value.Intensity;
            // Shadow fields are optional so older scenes still load.
            const auto shadowPos = row.tellg();
            int castShadows = value.CastShadows ? 1 : 0;
            if (row >> castShadows >> value.ShadowBias >> value.ShadowStrength >>
                value.ShadowMapResolution)
            {
                value.CastShadows = castShadows != 0;
                // Optional trailing cascade tokens (added later). Missing -> defaults.
                DirectionalLightComponent extended = value;
                if (row >> extended.ShadowDistance >> extended.CascadeSplitLambda >>
                    extended.ShadowSoftness)
                {
                    value = extended;
                }
            }
            else
            {
                row.clear();
                row.seekg(shadowPos);
            }
            Sanitize(value);
            registry.emplace<DirectionalLightComponent>(entity, value);
        }
        else if (marker == "LC")
        {
            unsigned body = 0;
            bool linked = false;
            row >> body >> linked;
            if (DirectionalLightComponent* value =
                    registry.try_get<DirectionalLightComponent>(entity))
            {
                value->CelestialBody = SanitizeCelestialLightType(body);
                value->LinkOppositeSun = linked;
            }
        }
        else if (marker == "P")
        {
            PointLightComponent value;
            row >> value.Color.x >> value.Color.y >> value.Color.z >> value.Intensity
                >> value.Range >> value.FalloffExponent >> value.Enabled;
            // Optional trailing shadow tokens (added later); absent or followed
            // by another marker -> restore the stream and keep defaults.
            const auto pointShadowPos = row.tellg();
            PointLightComponent shadowed = value;
            if (row >> shadowed.CastShadows >> shadowed.ShadowBias >> shadowed.ShadowResolution >>
                shadowed.ShadowStrength >> shadowed.Softness)
            {
                value = shadowed;
            }
            else
            {
                row.clear();
                row.seekg(pointShadowPos);
            }
            Sanitize(value);
            registry.emplace<PointLightComponent>(entity, value);
        }
        else if (marker == "S")
        {
            SpotLightComponent value;
            row >> value.Color.x >> value.Color.y >> value.Color.z >> value.Intensity
                >> value.Range >> value.InnerConeDegrees >> value.OuterConeDegrees
                >> value.FalloffExponent >> value.Enabled;
            const auto spotShadowPos = row.tellg();
            SpotLightComponent shadowed = value;
            if (row >> shadowed.CastShadows >> shadowed.ShadowBias >> shadowed.ShadowResolution >>
                shadowed.ShadowStrength >> shadowed.Softness)
            {
                value = shadowed;
            }
            else
            {
                row.clear();
                row.seekg(spotShadowPos);
            }
            Sanitize(value);
            registry.emplace<SpotLightComponent>(entity, value);
        }
        else if (marker == "E")
        {
            EnvironmentComponent value;
            row >> value.SkyZenithColor.x >> value.SkyZenithColor.y >> value.SkyZenithColor.z
                >> value.SkyHorizonColor.x >> value.SkyHorizonColor.y >> value.SkyHorizonColor.z
                >> value.GroundColor.x >> value.GroundColor.y >> value.GroundColor.z
                >> value.AmbientColor.x >> value.AmbientColor.y >> value.AmbientColor.z
                >> value.AmbientIntensity >> value.Exposure >> value.FogEnabled
                >> value.FogColor.x >> value.FogColor.y >> value.FogColor.z
                >> value.FogDensity >> value.FogStart >> value.FogEnd
                >> value.Primary >> value.Priority;
            EnvironmentComponent probe = value;
            if (row >> probe.BloomEnabled >> probe.BloomThreshold >> probe.BloomIntensity
                    >> probe.BloomPasses >> probe.TonemapEnabled >> probe.ColorGradingEnabled
                    >> probe.Lift.x >> probe.Lift.y >> probe.Lift.z
                    >> probe.Gamma.x >> probe.Gamma.y >> probe.Gamma.z
                    >> probe.Gain.x >> probe.Gain.y >> probe.Gain.z
                    >> probe.FxaaEnabled >> probe.FxaaStrength)
            {
                value = probe;
                EnvironmentComponent aaProbe = value;
                unsigned antiAliasRaw = 0;
                if (row >> antiAliasRaw >> aaProbe.TaaHistoryWeight >> aaProbe.TaaSharpness)
                {
                    aaProbe.AntiAlias = SanitizeAntiAliasMode(antiAliasRaw);
                    value = aaProbe;
                }
                else
                {
                    value.AntiAlias =
                        value.FxaaEnabled ? AntiAliasMode::Auto : AntiAliasMode::Off;
                    row.clear();
                }
                // Optional trailing HDR-grade tokens (added later). A scene saved
                // before these existed simply keeps the neutral defaults.
                EnvironmentComponent extended = value;
                if (row >> extended.ExposureCompensationEV >> extended.WhiteBalanceTemperature
                        >> extended.WhiteBalanceTint >> extended.Contrast
                        >> extended.Saturation >> extended.HighlightCompression)
                {
                    value = extended;
                    // Optional trailing IBL tokens (added later still): env map
                    // handle ("-" = none), rotation, intensity, use-as-sky.
                    EnvironmentComponent ibl = value;
                    std::string envMapToken;
                    if (row >> envMapToken >> ibl.EnvironmentRotation >> ibl.EnvironmentIntensity >>
                        ibl.UseEnvironmentAsSky)
                    {
                        if (envMapToken != "-")
                        {
                            if (const auto id = Uuid::Parse(envMapToken))
                            {
                                ibl.EnvironmentMap = *id;
                            }
                        }
                        value = ibl;
                        // Optional trailing AO tokens (added later still).
                        EnvironmentComponent ao = value;
                        if (row >> ao.AoEnabled >> ao.AoRadius >> ao.AoIntensity >> ao.AoPower >>
                            ao.AoBias)
                        {
                            value = ao;
                        }
                    }
                }
            }
            row.clear(); // reset failbit so an older row (no post tokens) still parses cleanly
            Sanitize(value);
            registry.emplace<EnvironmentComponent>(entity, value);
        }
        else if (marker == "V")
        {
            VisibilityComponent value;
            row >> value.VisibleInEditor >> value.VisibleInGame >> value.Selectable;
            registry.emplace<VisibilityComponent>(entity, value);
        }
        else if (marker == "C")
        {
            CameraComponent value;
            row >> value.FieldOfView >> value.NearPlane >> value.FarPlane >> value.Primary;
            registry.emplace<CameraComponent>(entity, value);
        }
        else if (marker == "N")
        {
            NetworkIdentityComponent value;
            unsigned authority = 0;
            row >> value.NetworkId >> authority >> value.Replicate;
            value.Authority = static_cast<NetworkAuthority>(authority);
            registry.emplace<NetworkIdentityComponent>(entity, value);
        }
        else if (marker == "J")
        {
            JoltBodyComponent value;
            row >> value.HalfExtent.x >> value.HalfExtent.y >> value.HalfExtent.z >> value.Dynamic;
            SanitizeJoltBody(value);
            registry.emplace<JoltBodyComponent>(entity, value);
        }
        else if (marker == "K")
        {
            unsigned shape = 0;
            bool autoFit = true;
            row >> shape >> autoFit;
            if (JoltBodyComponent* value = registry.try_get<JoltBodyComponent>(entity))
            {
                value->Shape = SanitizeColliderShape3D(shape);
                value->AutoFit = autoFit;
            }
        }
        else if (marker == "JW")
        {
            float mass = 1.0F;
            row >> mass;
            if (JoltBodyComponent* value = registry.try_get<JoltBodyComponent>(entity))
            {
                value->Mass = mass;
                SanitizeJoltBody(*value);
            }
        }
        else if (marker == "JF")
        {
            float friction = 0.8F;
            row >> friction;
            if (JoltBodyComponent* value = registry.try_get<JoltBodyComponent>(entity))
            {
                value->Friction = friction;
                SanitizeJoltBody(*value);
            }
        }
        else if (marker == "CC")
        {
            CharacterControllerComponent value;
            row >> value.Radius >> value.Height >> value.StepHeight >> value.MaxSlopeDegrees
                >> value.MoveSpeed >> value.JumpSpeed >> value.Gravity >> value.AirControl
                >> value.PushStrength;
            SanitizeCharacterController(value);
            registry.remove<JoltBodyComponent>(entity);
            registry.emplace<CharacterControllerComponent>(entity, value);
        }
        else if (marker == "B")
        {
            Box2DBodyComponent value;
            row >> value.HalfExtent.x >> value.HalfExtent.y >> value.Dynamic;
            registry.emplace<Box2DBodyComponent>(entity, value);
        }
        else if (marker == "Y")
        {
            ScriptComponent value;
            std::size_t count = 0;
            row >> count;
            std::string scriptName;
            // The stream check stops a truncated or corrupt count from
            // looping past the data actually present on the line.
            for (std::size_t i = 0; i < count && (row >> std::quoted(scriptName)); ++i)
            {
                value.ScriptNames.push_back(std::move(scriptName));
            }
            row >> value.Enabled;
            const std::streampos targetPos = row.tellg();
            std::string targetText;
            if (row >> targetText)
            {
                if (targetText == "-")
                {
                    // Explicitly empty target in the current format.
                }
                else if (const auto parsed = Uuid::Parse(targetText))
                {
                    value.Target = *parsed;
                }
                else
                {
                    // Older scenes ended Script at Enabled. Do not consume the
                    // following component marker as if it were a target UUID.
                    row.clear();
                    row.seekg(targetPos);
                }
            }
            registry.emplace<ScriptComponent>(entity, std::move(value));
        }
        else if (marker == "PE")
        {
            ParticleEmitterComponent value;
            unsigned shape = 0;
            row >> value.Enabled >> value.MaxParticles >> value.EmitRate >> value.LifetimeMin >>
                value.LifetimeMax >> value.SpeedMin >> value.SpeedMax >> shape >> value.ShapeRadius >>
                value.ConeAngle >> value.Direction.x >> value.Direction.y >> value.Direction.z >>
                value.Gravity.x >> value.Gravity.y >> value.Gravity.z >> value.ColorStart.x >>
                value.ColorStart.y >> value.ColorStart.z >> value.ColorStart.w >> value.ColorEnd.x >>
                value.ColorEnd.y >> value.ColorEnd.z >> value.ColorEnd.w >> value.SizeStart >>
                value.SizeEnd >> value.PlayOnStart >> value.Loop;
            value.Shape = shape <= static_cast<unsigned>(EmitterShape::Box)
                ? static_cast<EmitterShape>(shape)
                : EmitterShape::Cone;
            registry.emplace<ParticleEmitterComponent>(entity, value);
        }
        else if (marker == "AS")
        {
            AudioSourceComponent value;
            std::string soundText;
            row >> soundText >> value.PlayOnStart >> value.Loop >> value.Volume >> value.MinDistance >>
                value.MaxDistance >> value.Spatial;
            if (soundText != "-")
            {
                if (const auto parsed = Uuid::Parse(soundText))
                {
                    value.Sound = *parsed;
                }
            }
            value.ActiveTrack = -1;
            registry.emplace<AudioSourceComponent>(entity, value);
        }
        else if (marker == "AL")
        {
            AudioListenerComponent value;
            row >> value.Active;
            registry.emplace<AudioListenerComponent>(entity, value);
        }
        else if (marker == "UC")
        {
            UICanvasComponent value;
            std::string uiText;
            std::string styleText;
            row >> uiText >> styleText >> value.RenderInEditor >> value.RenderInGame >> value.Order >>
                value.Scale;
            if (uiText != "-")
            {
                if (const auto parsed = Uuid::Parse(uiText))
                {
                    value.UIAsset = *parsed;
                }
            }
            if (styleText != "-")
            {
                if (const auto parsed = Uuid::Parse(styleText))
                {
                    value.StyleAsset = *parsed;
                }
            }
            registry.emplace<UICanvasComponent>(entity, value);
        }
        else if (marker == "TR")
        {
            TerrainComponent value;
            std::string heightmapText;
            row >> heightmapText >> value.Width >> value.Depth >> value.HeightScale >>
                value.ResolutionX >> value.ResolutionZ >> value.LayerCount >> value.CastShadows;
            value.LayerCount = std::clamp(value.LayerCount, 1, 4);
            if (heightmapText != "-")
            {
                if (const auto parsed = Uuid::Parse(heightmapText))
                {
                    value.Heightmap = *parsed;
                }
            }
            for (int i = 0; i < value.LayerCount; ++i)
            {
                std::string textureText;
                row >> textureText >> value.Layers[i].Tiling >> value.Layers[i].MinHeight >>
                    value.Layers[i].MaxHeight >> value.Layers[i].MinSlope >> value.Layers[i].MaxSlope;
                if (textureText != "-")
                {
                    if (const auto parsed = Uuid::Parse(textureText))
                    {
                        value.Layers[i].Texture = *parsed;
                    }
                }
            }
            registry.emplace<TerrainComponent>(entity, value);
        }
        else if (marker == "AN")
        {
            AnimatorComponent value;
            row >> std::quoted(value.ClipName) >> value.Speed >> value.Loop >> value.Playing;
            value.ClipIndex = -1;
            value.CurrentTime = 0.0F;
            registry.emplace<AnimatorComponent>(entity, value);
        }
        else
        {
            // Unknown component record from a newer build: its argument
            // count is unknowable, so skip the remainder of this entity
            // line rather than corrupting the rest of the scene.
            break;
        }
    }
    if (!row && marker != "END")
    {
        return Result<void>::Error("Truncated entity in scene");
    }
    return Result<void>::Ok();
}
}
