#pragma once

#include "engine/Uuid.hpp"
#include "engine/camera/CameraComponent.hpp"
#include "engine/physics/IPhysicsWorld.hpp"
#include "engine/reflection/PropertyRegistry.hpp"
#include "engine/assets/AssetHandle.hpp"
#include "engine/render/RenderQuality.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace fadix
{
struct UuidComponent
{
    Uuid Id;
};

struct NameComponent
{
    std::string Name{"Entity"};
};

struct TransformComponent
{
    glm::vec3 Position{0.0F};
    glm::quat Rotation{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 Scale{1.0F};
};

struct RelationshipComponent
{
    Uuid Parent;
};

enum class MeshKind : std::uint8_t
{
    Cube,
    Sphere,
    Plane,
    Cylinder,
    Capsule
};

inline constexpr unsigned MeshKindCount = 5;

// Older or corrupted scene data may carry values outside the enum; always fall
// back to Cube so meshes keep rendering.
[[nodiscard]] inline MeshKind SanitizeMeshKind(const unsigned value) noexcept
{
    return value < MeshKindCount ? static_cast<MeshKind>(value) : MeshKind::Cube;
}

[[nodiscard]] inline const char* MeshKindName(const MeshKind kind) noexcept
{
    switch (kind)
    {
    case MeshKind::Sphere: return "Sphere";
    case MeshKind::Plane: return "Plane";
    case MeshKind::Cylinder: return "Cylinder";
    case MeshKind::Capsule: return "Capsule";
    case MeshKind::Cube: break;
    }
    return "Cube";
}

struct MeshComponent
{
    MeshKind Kind{MeshKind::Cube};
    glm::vec3 BaseColor{0.62F, 0.65F, 0.70F};
    float Metallic{0.05F};
    float Roughness{0.55F};
    AssetHandle Material{};
    AssetHandle ImportedMesh{}; // when valid, overrides Kind with an imported glTF mesh
};

enum class CelestialLightType : std::uint8_t
{
    None,
    Sun,
    Moon
};

struct DirectionalLightComponent
{
    glm::vec3 Color{1.0F, 0.96F, 0.88F};
    float Intensity{3.0F};
    bool CastShadows{true};
    float ShadowBias{0.005F};
    float ShadowStrength{0.7F};
    int ShadowMapResolution{2048};
    float ShadowDistance{150.0F};
    float CascadeSplitLambda{0.5F};
    float ShadowSoftness{1.0F};
    CelestialLightType CelestialBody{CelestialLightType::None};
    bool LinkOppositeSun{false};
};

[[nodiscard]] inline DirectionalLightComponent MakeSunLight() noexcept
{
    DirectionalLightComponent light;
    light.CelestialBody = CelestialLightType::Sun;
    return light;
}

[[nodiscard]] inline DirectionalLightComponent MakeMoonLight() noexcept
{
    DirectionalLightComponent light;
    light.Color = {0.62F, 0.72F, 1.0F};
    light.Intensity = 0.35F;
    light.ShadowStrength = 0.45F;
    light.ShadowMapResolution = 1024;
    light.CelestialBody = CelestialLightType::Moon;
    light.LinkOppositeSun = true;
    return light;
}

[[nodiscard]] inline CelestialLightType SanitizeCelestialLightType(const unsigned value) noexcept
{
    return value <= static_cast<unsigned>(CelestialLightType::Moon)
        ? static_cast<CelestialLightType>(value)
        : CelestialLightType::None;
}

struct PointLightComponent
{
    glm::vec3 Color{1.0F};
    float Intensity{10.0F};
    float Range{10.0F};
    float FalloffExponent{2.0F};
    bool Enabled{true};
    bool CastShadows{false};
    float ShadowBias{0.005F};
    int ShadowResolution{512};
    float ShadowStrength{0.7F};
    float Softness{1.0F};
};

struct SpotLightComponent
{
    glm::vec3 Color{1.0F};
    float Intensity{15.0F};
    float Range{15.0F};
    float InnerConeDegrees{20.0F};
    float OuterConeDegrees{35.0F};
    float FalloffExponent{2.0F};
    bool Enabled{true};
    bool CastShadows{false};
    float ShadowBias{0.005F};
    int ShadowResolution{512};
    float ShadowStrength{0.7F};
    float Softness{1.0F};
};

struct EnvironmentComponent
{
    glm::vec3 SkyZenithColor{0.13F, 0.27F, 0.52F};
    glm::vec3 SkyHorizonColor{0.63F, 0.71F, 0.82F};
    glm::vec3 GroundColor{0.20F, 0.19F, 0.18F};
    glm::vec3 AmbientColor{0.45F, 0.55F, 0.70F};
    float AmbientIntensity{1.0F};
    float Exposure{1.0F};

    bool FogEnabled{false};
    glm::vec3 FogColor{0.60F, 0.66F, 0.75F};
    float FogDensity{0.01F};
    float FogStart{10.0F};
    float FogEnd{250.0F};

    bool Primary{false};
    int Priority{0};

    // Post-processing (viewport). Exposure above is reused for the tonemap pass.
    bool BloomEnabled{true};
    float BloomThreshold{1.0F};
    float BloomIntensity{0.3F};
    int BloomPasses{5};

    bool TonemapEnabled{true};

    bool ColorGradingEnabled{false};
    glm::vec3 Lift{0.0F, 0.0F, 0.0F};
    glm::vec3 Gamma{1.0F, 1.0F, 1.0F};
    glm::vec3 Gain{1.0F, 1.0F, 1.0F};

    bool FxaaEnabled{true};
    float FxaaStrength{1.0F};

    AntiAliasMode AntiAlias{AntiAliasMode::Auto};
    float TaaHistoryWeight{0.9F};
    float TaaSharpness{0.0F};

    float ExposureCompensationEV{0.0F};
    float WhiteBalanceTemperature{6500.0F};
    float WhiteBalanceTint{0.0F};
    float Contrast{1.0F};
    float Saturation{1.0F};
    float HighlightCompression{0.0F};

    AssetHandle EnvironmentMap{};
    float EnvironmentRotation{0.0F};
    float EnvironmentIntensity{1.0F};
    bool UseEnvironmentAsSky{false};

    bool AoEnabled{true};
    float AoRadius{0.5F};
    float AoIntensity{1.0F};
    float AoPower{2.0F};
    float AoBias{0.025F};
};

struct VisibilityComponent
{
    bool VisibleInEditor{true};
    bool VisibleInGame{true};
    bool Selectable{true};
};

// Shared clamping used by the Inspector, the serializer, and the smoke tests
// so an invalid value can never reach the renderer from any path.
[[nodiscard]] inline float SanitizedFloat(
    const float value, const float minimum, const float maximum, const float fallback) noexcept
{
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

[[nodiscard]] inline glm::vec3 SanitizedColor(const glm::vec3 color, const glm::vec3 fallback) noexcept
{
    return {SanitizedFloat(color.x, 0.0F, 1.0F, fallback.x),
        SanitizedFloat(color.y, 0.0F, 1.0F, fallback.y),
        SanitizedFloat(color.z, 0.0F, 1.0F, fallback.z)};
}

[[nodiscard]] inline int SanitizedShadowResolution(const int requested) noexcept
{
    if (requested >= 4096)
    {
        return 4096;
    }
    if (requested >= 2048)
    {
        return 2048;
    }
    return 1024;
}

inline void Sanitize(DirectionalLightComponent& light) noexcept
{
    const DirectionalLightComponent defaults;
    light.Color = SanitizedColor(light.Color, defaults.Color);
    light.Intensity = SanitizedFloat(light.Intensity, 0.0F, 100000.0F, defaults.Intensity);
    light.ShadowBias = SanitizedFloat(light.ShadowBias, 0.0F, 0.1F, defaults.ShadowBias);
    light.ShadowStrength = SanitizedFloat(light.ShadowStrength, 0.0F, 1.0F, defaults.ShadowStrength);
    light.ShadowMapResolution = SanitizedShadowResolution(light.ShadowMapResolution);
    light.ShadowDistance = SanitizedFloat(light.ShadowDistance, 5.0F, 2000.0F, defaults.ShadowDistance);
    light.CascadeSplitLambda =
        SanitizedFloat(light.CascadeSplitLambda, 0.0F, 1.0F, defaults.CascadeSplitLambda);
    light.ShadowSoftness = SanitizedFloat(light.ShadowSoftness, 0.0F, 4.0F, defaults.ShadowSoftness);
    light.CelestialBody =
        SanitizeCelestialLightType(static_cast<unsigned>(light.CelestialBody));
    if (light.CelestialBody != CelestialLightType::Moon)
    {
        light.LinkOppositeSun = false;
    }
}

inline void Sanitize(PointLightComponent& light) noexcept
{
    const PointLightComponent defaults;
    light.Color = SanitizedColor(light.Color, defaults.Color);
    light.Intensity = SanitizedFloat(light.Intensity, 0.0F, 100000.0F, defaults.Intensity);
    light.Range = SanitizedFloat(light.Range, 0.01F, 10000.0F, defaults.Range);
    light.FalloffExponent = SanitizedFloat(light.FalloffExponent, 0.1F, 16.0F, defaults.FalloffExponent);
    light.ShadowBias = SanitizedFloat(light.ShadowBias, 0.0F, 1.0F, defaults.ShadowBias);
    light.ShadowResolution = SanitizedShadowResolution(light.ShadowResolution);
    light.ShadowStrength = SanitizedFloat(light.ShadowStrength, 0.0F, 1.0F, defaults.ShadowStrength);
    light.Softness = SanitizedFloat(light.Softness, 0.0F, 8.0F, defaults.Softness);
}

inline void Sanitize(SpotLightComponent& light) noexcept
{
    const SpotLightComponent defaults;
    light.Color = SanitizedColor(light.Color, defaults.Color);
    light.Intensity = SanitizedFloat(light.Intensity, 0.0F, 100000.0F, defaults.Intensity);
    light.Range = SanitizedFloat(light.Range, 0.01F, 10000.0F, defaults.Range);
    light.FalloffExponent = SanitizedFloat(light.FalloffExponent, 0.1F, 16.0F, defaults.FalloffExponent);
    light.OuterConeDegrees =
        SanitizedFloat(light.OuterConeDegrees, 0.2F, 89.9F, defaults.OuterConeDegrees);
    light.InnerConeDegrees = SanitizedFloat(
        light.InnerConeDegrees, 0.0F, light.OuterConeDegrees - 0.1F,
        std::min(defaults.InnerConeDegrees, light.OuterConeDegrees - 0.1F));
    light.ShadowBias = SanitizedFloat(light.ShadowBias, 0.0F, 1.0F, defaults.ShadowBias);
    light.ShadowResolution = SanitizedShadowResolution(light.ShadowResolution);
    light.ShadowStrength = SanitizedFloat(light.ShadowStrength, 0.0F, 1.0F, defaults.ShadowStrength);
    light.Softness = SanitizedFloat(light.Softness, 0.0F, 8.0F, defaults.Softness);
}

inline void Sanitize(EnvironmentComponent& environment) noexcept
{
    const EnvironmentComponent defaults;
    environment.SkyZenithColor = SanitizedColor(environment.SkyZenithColor, defaults.SkyZenithColor);
    environment.SkyHorizonColor =
        SanitizedColor(environment.SkyHorizonColor, defaults.SkyHorizonColor);
    environment.GroundColor = SanitizedColor(environment.GroundColor, defaults.GroundColor);
    environment.AmbientColor = SanitizedColor(environment.AmbientColor, defaults.AmbientColor);
    environment.AmbientIntensity =
        SanitizedFloat(environment.AmbientIntensity, 0.0F, 16.0F, defaults.AmbientIntensity);
    environment.Exposure = SanitizedFloat(environment.Exposure, 0.05F, 16.0F, defaults.Exposure);
    environment.FogColor = SanitizedColor(environment.FogColor, defaults.FogColor);
    environment.FogDensity = SanitizedFloat(environment.FogDensity, 0.0F, 1.0F, defaults.FogDensity);
    environment.FogStart = SanitizedFloat(environment.FogStart, 0.0F, 100000.0F, defaults.FogStart);
    environment.FogEnd = SanitizedFloat(
        environment.FogEnd, environment.FogStart + 0.1F, 200000.0F, environment.FogStart + 240.0F);
    environment.Priority = std::clamp(environment.Priority, -1000000, 1000000);
    environment.BloomThreshold = SanitizedFloat(environment.BloomThreshold, 0.0F, 10.0F, defaults.BloomThreshold);
    environment.BloomIntensity = SanitizedFloat(environment.BloomIntensity, 0.0F, 4.0F, defaults.BloomIntensity);
    environment.BloomPasses = std::clamp(environment.BloomPasses, 1, 8);
    environment.Lift = {
        SanitizedFloat(environment.Lift.x, -1.0F, 1.0F, defaults.Lift.x),
        SanitizedFloat(environment.Lift.y, -1.0F, 1.0F, defaults.Lift.y),
        SanitizedFloat(environment.Lift.z, -1.0F, 1.0F, defaults.Lift.z)};
    environment.Gamma = {
        SanitizedFloat(environment.Gamma.x, 0.1F, 3.0F, defaults.Gamma.x),
        SanitizedFloat(environment.Gamma.y, 0.1F, 3.0F, defaults.Gamma.y),
        SanitizedFloat(environment.Gamma.z, 0.1F, 3.0F, defaults.Gamma.z)};
    environment.Gain = {
        SanitizedFloat(environment.Gain.x, 0.0F, 3.0F, defaults.Gain.x),
        SanitizedFloat(environment.Gain.y, 0.0F, 3.0F, defaults.Gain.y),
        SanitizedFloat(environment.Gain.z, 0.0F, 3.0F, defaults.Gain.z)};
    environment.FxaaStrength = SanitizedFloat(environment.FxaaStrength, 0.0F, 2.0F, defaults.FxaaStrength);
    environment.AntiAlias = SanitizeAntiAliasMode(static_cast<unsigned>(environment.AntiAlias));
    environment.TaaHistoryWeight =
        SanitizedFloat(environment.TaaHistoryWeight, 0.7F, 0.99F, defaults.TaaHistoryWeight);
    environment.TaaSharpness = SanitizedFloat(environment.TaaSharpness, 0.0F, 1.0F, defaults.TaaSharpness);
    environment.ExposureCompensationEV =
        SanitizedFloat(environment.ExposureCompensationEV, -8.0F, 8.0F, defaults.ExposureCompensationEV);
    environment.WhiteBalanceTemperature = SanitizedFloat(
        environment.WhiteBalanceTemperature, 1500.0F, 15000.0F, defaults.WhiteBalanceTemperature);
    environment.WhiteBalanceTint =
        SanitizedFloat(environment.WhiteBalanceTint, -1.0F, 1.0F, defaults.WhiteBalanceTint);
    environment.Contrast = SanitizedFloat(environment.Contrast, 0.0F, 2.0F, defaults.Contrast);
    environment.Saturation = SanitizedFloat(environment.Saturation, 0.0F, 2.0F, defaults.Saturation);
    environment.HighlightCompression =
        SanitizedFloat(environment.HighlightCompression, 0.0F, 1.0F, defaults.HighlightCompression);
    environment.EnvironmentRotation =
        SanitizedFloat(environment.EnvironmentRotation, -360.0F, 360.0F, defaults.EnvironmentRotation);
    environment.EnvironmentIntensity =
        SanitizedFloat(environment.EnvironmentIntensity, 0.0F, 16.0F, defaults.EnvironmentIntensity);
    environment.AoRadius = SanitizedFloat(environment.AoRadius, 0.05F, 5.0F, defaults.AoRadius);
    environment.AoIntensity = SanitizedFloat(environment.AoIntensity, 0.0F, 4.0F, defaults.AoIntensity);
    environment.AoPower = SanitizedFloat(environment.AoPower, 0.1F, 8.0F, defaults.AoPower);
    environment.AoBias = SanitizedFloat(environment.AoBias, 0.0F, 0.5F, defaults.AoBias);
}

enum class NetworkAuthority : std::uint8_t
{
    Server,
    Owner,
    Shared
};

struct NetworkIdentityComponent
{
    std::uint64_t NetworkId{0};
    NetworkAuthority Authority{NetworkAuthority::Server};
    bool Replicate{true};
};

struct JoltBodyComponent
{
    PhysicsBodyHandle Handle{InvalidPhysicsBody};
    glm::vec3 HalfExtent{0.5F};
    ColliderShape3D Shape{ColliderShape3D::Auto};
    float Mass{1.0F};
    float Friction{0.8F};
    bool AutoFit{true};
    bool Dynamic{true};
};

inline void SanitizeJoltBody(JoltBodyComponent& body) noexcept
{
    body.HalfExtent.x = SanitizedFloat(body.HalfExtent.x, 0.001F, 100000.0F, 0.5F);
    body.HalfExtent.y = SanitizedFloat(body.HalfExtent.y, 0.001F, 100000.0F, 0.5F);
    body.HalfExtent.z = SanitizedFloat(body.HalfExtent.z, 0.001F, 100000.0F, 0.5F);
    body.Mass = SanitizedFloat(body.Mass, 0.001F, 1000000.0F, 1.0F);
    body.Friction = SanitizedFloat(body.Friction, 0.0F, 2.0F, 0.8F);
}

// A gameplay character is not a loose rigid body. Jolt sweeps this upright
// capsule through the world, which provides stable slopes, stairs and grounding
// without allowing impacts to tip the player over.
struct CharacterControllerComponent
{
    float Radius{0.5F};
    float Height{2.0F};
    float StepHeight{0.35F};
    float MaxSlopeDegrees{45.0F};
    float MoveSpeed{5.0F};
    float JumpSpeed{5.0F};
    float Gravity{-9.81F};
    float AirControl{0.35F};
    float PushStrength{100.0F};

    // Runtime commands/state. These are cloned for Play but never serialized.
    glm::vec2 MoveInput{0.0F};
    bool JumpRequested{false};
    bool Grounded{false};
};

inline void SanitizeCharacterController(CharacterControllerComponent& character) noexcept
{
    character.Radius = SanitizedFloat(character.Radius, 0.05F, 10.0F, 0.5F);
    character.Height = SanitizedFloat(character.Height,
        character.Radius * 2.0F + 0.02F, 20.0F, 2.0F);
    character.StepHeight = SanitizedFloat(character.StepHeight, 0.0F, character.Height, 0.35F);
    character.MaxSlopeDegrees =
        SanitizedFloat(character.MaxSlopeDegrees, 0.0F, 89.0F, 45.0F);
    character.MoveSpeed = SanitizedFloat(character.MoveSpeed, 0.0F, 100.0F, 5.0F);
    character.JumpSpeed = SanitizedFloat(character.JumpSpeed, 0.0F, 100.0F, 5.0F);
    character.Gravity = SanitizedFloat(character.Gravity, -100.0F, 0.0F, -9.81F);
    character.AirControl = SanitizedFloat(character.AirControl, 0.0F, 1.0F, 0.35F);
    character.PushStrength =
        SanitizedFloat(character.PushStrength, 0.0F, 100000.0F, 100.0F);
}

[[nodiscard]] inline const char* ColliderShape3DName(const ColliderShape3D shape) noexcept
{
    switch (shape)
    {
    case ColliderShape3D::Box: return "Box";
    case ColliderShape3D::Sphere: return "Sphere";
    case ColliderShape3D::Capsule: return "Capsule";
    case ColliderShape3D::Cylinder: return "Cylinder";
    case ColliderShape3D::Plane: return "Plane";
    case ColliderShape3D::Mesh: return "Mesh";
    case ColliderShape3D::Auto: break;
    }
    return "Auto";
}

[[nodiscard]] inline ColliderShape3D SanitizeColliderShape3D(const unsigned value) noexcept
{
    return value <= static_cast<unsigned>(ColliderShape3D::Mesh)
        ? static_cast<ColliderShape3D>(value)
        : ColliderShape3D::Auto;
}

[[nodiscard]] inline ColliderShape3D ResolveColliderShape3D(
    const JoltBodyComponent& body, const MeshComponent* mesh) noexcept
{
    if (body.Shape != ColliderShape3D::Auto)
    {
        return body.Shape;
    }
    if (mesh == nullptr)
    {
        return ColliderShape3D::Box;
    }
    if (mesh->ImportedMesh.IsValid())
    {
        return ColliderShape3D::Mesh;
    }
    switch (mesh->Kind)
    {
    case MeshKind::Sphere: return ColliderShape3D::Sphere;
    case MeshKind::Capsule: return ColliderShape3D::Capsule;
    case MeshKind::Cylinder: return ColliderShape3D::Cylinder;
    case MeshKind::Plane: return ColliderShape3D::Plane;
    case MeshKind::Cube: break;
    }
    return ColliderShape3D::Box;
}

[[nodiscard]] inline glm::vec3 DefaultColliderHalfExtent(const MeshKind kind) noexcept
{
    return kind == MeshKind::Capsule ? glm::vec3{0.5F, 1.0F, 0.5F}
        : kind == MeshKind::Plane ? glm::vec3{0.5F, 0.02F, 0.5F}
                                  : glm::vec3{0.5F};
}

struct Box2DBodyComponent
{
    PhysicsBodyHandle Handle{InvalidPhysicsBody};
    glm::vec2 HalfExtent{0.5F};
    bool Dynamic{true};
};

struct ScriptComponent
{
    std::vector<std::string> ScriptNames; // scripts attached to this entity, by name
    bool Enabled{true};
    Uuid Target{}; // hierarchy entity ref for scripts; invalid = unset
};

enum class EmitterShape : std::uint8_t
{
    Sphere,
    Cone,
    Box
};

struct ParticleEmitterComponent
{
    bool Enabled{true};
    int MaxParticles{200};
    float EmitRate{20.0F};
    float LifetimeMin{1.0F};
    float LifetimeMax{3.0F};
    float SpeedMin{1.0F};
    float SpeedMax{3.0F};
    EmitterShape Shape{EmitterShape::Cone};
    float ShapeRadius{0.5F};
    float ConeAngle{25.0F};
    glm::vec3 Direction{0.0F, 1.0F, 0.0F};
    glm::vec3 Gravity{0.0F, -9.81F, 0.0F};
    glm::vec4 ColorStart{1.0F, 1.0F, 1.0F, 1.0F};
    glm::vec4 ColorEnd{1.0F, 1.0F, 1.0F, 0.0F};
    float SizeStart{0.1F};
    float SizeEnd{0.0F};
    bool PlayOnStart{true};
    bool Loop{true};
};

struct AudioSourceComponent
{
    AssetHandle Sound{};
    bool PlayOnStart{true};
    bool Loop{false};
    float Volume{1.0F};
    float MinDistance{1.0F};
    float MaxDistance{50.0F};
    bool Spatial{false};
    int ActiveTrack{-1}; // runtime only; not serialized
};

struct AudioListenerComponent
{
    bool Active{true};
};

struct UICanvasComponent
{
    AssetHandle UIAsset{};
    AssetHandle StyleAsset{};
    bool RenderInEditor{false};
    bool RenderInGame{true};
    int Order{0};
    float Scale{1.0F};
};

struct TerrainLayer
{
    AssetHandle Texture{};
    float Tiling{10.0F};
    float MinHeight{0.0F};
    float MaxHeight{1.0F};
    float MinSlope{0.0F};
    float MaxSlope{1.0F};
};

struct TerrainComponent
{
    AssetHandle Heightmap{};
    float Width{100.0F};
    float Depth{100.0F};
    float HeightScale{30.0F};
    int ResolutionX{129};
    int ResolutionZ{129};
    TerrainLayer Layers[4]{};
    int LayerCount{1};
    bool CastShadows{true};
};

struct SkeletonComponent
{
    bool HasSkeleton{false};
    int JointCount{0};
    std::vector<glm::mat4> JointMatrices;
};

struct AnimatorComponent
{
    std::string ClipName;
    float Speed{1.0F};
    bool Loop{true};
    bool Playing{true};
    int ClipIndex{-1};
    float CurrentTime{0.0F};
};

inline void RegisterRuntimeComponentProperties(PropertyRegistry& registry)
{
    registry.RegisterComponent<NameComponent>("Name");
    registry.AddProperty<NameComponent>("name");
    registry.RegisterComponent<TransformComponent>("Transform");
    registry.AddProperty<TransformComponent>("position");
    registry.AddProperty<TransformComponent>("rotation");
    registry.AddProperty<TransformComponent>("scale");
    registry.RegisterComponent<MeshComponent>("Mesh");
    registry.AddProperty<MeshComponent>("kind");
    registry.AddProperty<MeshComponent>("baseColor");
    registry.AddProperty<MeshComponent>("metallic");
    registry.AddProperty<MeshComponent>("roughness");
    registry.AddProperty<MeshComponent>("material");
    registry.RegisterComponent<DirectionalLightComponent>("DirectionalLight");
    registry.AddProperty<DirectionalLightComponent>("color");
    registry.AddProperty<DirectionalLightComponent>("intensity");
    registry.AddProperty<DirectionalLightComponent>("castShadows");
    registry.AddProperty<DirectionalLightComponent>("shadowBias");
    registry.AddProperty<DirectionalLightComponent>("shadowStrength");
    registry.AddProperty<DirectionalLightComponent>("shadowMapResolution");
    registry.AddProperty<DirectionalLightComponent>("celestialBody");
    registry.AddProperty<DirectionalLightComponent>("linkOppositeSun");
    registry.RegisterComponent<PointLightComponent>("PointLight");
    registry.AddProperty<PointLightComponent>("color");
    registry.AddProperty<PointLightComponent>("intensity");
    registry.AddProperty<PointLightComponent>("range");
    registry.AddProperty<PointLightComponent>("falloffExponent");
    registry.AddProperty<PointLightComponent>("enabled");
    registry.RegisterComponent<SpotLightComponent>("SpotLight");
    registry.AddProperty<SpotLightComponent>("color");
    registry.AddProperty<SpotLightComponent>("intensity");
    registry.AddProperty<SpotLightComponent>("range");
    registry.AddProperty<SpotLightComponent>("innerConeDegrees");
    registry.AddProperty<SpotLightComponent>("outerConeDegrees");
    registry.AddProperty<SpotLightComponent>("falloffExponent");
    registry.AddProperty<SpotLightComponent>("enabled");
    registry.RegisterComponent<EnvironmentComponent>("Environment");
    registry.AddProperty<EnvironmentComponent>("skyZenithColor");
    registry.AddProperty<EnvironmentComponent>("skyHorizonColor");
    registry.AddProperty<EnvironmentComponent>("groundColor");
    registry.AddProperty<EnvironmentComponent>("ambientColor");
    registry.AddProperty<EnvironmentComponent>("ambientIntensity");
    registry.AddProperty<EnvironmentComponent>("exposure");
    registry.AddProperty<EnvironmentComponent>("fogEnabled");
    registry.AddProperty<EnvironmentComponent>("fogColor");
    registry.AddProperty<EnvironmentComponent>("fogDensity");
    registry.AddProperty<EnvironmentComponent>("fogStart");
    registry.AddProperty<EnvironmentComponent>("fogEnd");
    registry.AddProperty<EnvironmentComponent>("primary");
    registry.AddProperty<EnvironmentComponent>("priority");
    registry.AddProperty<EnvironmentComponent>("bloomEnabled");
    registry.AddProperty<EnvironmentComponent>("bloomThreshold");
    registry.AddProperty<EnvironmentComponent>("bloomIntensity");
    registry.AddProperty<EnvironmentComponent>("bloomPasses");
    registry.AddProperty<EnvironmentComponent>("tonemapEnabled");
    registry.AddProperty<EnvironmentComponent>("colorGradingEnabled");
    registry.AddProperty<EnvironmentComponent>("lift");
    registry.AddProperty<EnvironmentComponent>("gamma");
    registry.AddProperty<EnvironmentComponent>("gain");
    registry.AddProperty<EnvironmentComponent>("fxaaEnabled");
    registry.AddProperty<EnvironmentComponent>("fxaaStrength");
    registry.RegisterComponent<VisibilityComponent>("Visibility");
    registry.AddProperty<VisibilityComponent>("visibleInEditor");
    registry.AddProperty<VisibilityComponent>("visibleInGame");
    registry.AddProperty<VisibilityComponent>("selectable");
    registry.RegisterComponent<CameraComponent>("Camera");
    registry.AddProperty<CameraComponent>("fieldOfView");
    registry.AddProperty<CameraComponent>("nearPlane");
    registry.AddProperty<CameraComponent>("farPlane");
    registry.AddProperty<CameraComponent>("primary");
    registry.RegisterComponent<NetworkIdentityComponent>("NetworkIdentity");
    registry.AddProperty<NetworkIdentityComponent>("networkId");
    registry.AddProperty<NetworkIdentityComponent>("authority");
    registry.AddProperty<NetworkIdentityComponent>("replicate");
    registry.RegisterComponent<JoltBodyComponent>("JoltBody");
    registry.AddProperty<JoltBodyComponent>("halfExtent");
    registry.AddProperty<JoltBodyComponent>("shape");
    registry.AddProperty<JoltBodyComponent>("mass");
    registry.AddProperty<JoltBodyComponent>("friction");
    registry.AddProperty<JoltBodyComponent>("autoFit");
    registry.AddProperty<JoltBodyComponent>("dynamic");
    registry.RegisterComponent<CharacterControllerComponent>("CharacterController");
    registry.AddProperty<CharacterControllerComponent>("radius");
    registry.AddProperty<CharacterControllerComponent>("height");
    registry.AddProperty<CharacterControllerComponent>("stepHeight");
    registry.AddProperty<CharacterControllerComponent>("maxSlopeDegrees");
    registry.AddProperty<CharacterControllerComponent>("moveSpeed");
    registry.AddProperty<CharacterControllerComponent>("jumpSpeed");
    registry.AddProperty<CharacterControllerComponent>("gravity");
    registry.AddProperty<CharacterControllerComponent>("airControl");
    registry.AddProperty<CharacterControllerComponent>("pushStrength");
    registry.RegisterComponent<Box2DBodyComponent>("Box2DBody");
    registry.AddProperty<Box2DBodyComponent>("halfExtent");
    registry.AddProperty<Box2DBodyComponent>("dynamic");
    registry.RegisterComponent<ScriptComponent>("Script");
    registry.AddProperty<ScriptComponent>("scriptNames");
    registry.AddProperty<ScriptComponent>("enabled");
    registry.AddProperty<ScriptComponent>("target");
    registry.RegisterComponent<ParticleEmitterComponent>("ParticleEmitter");
    registry.AddProperty<ParticleEmitterComponent>("enabled");
    registry.AddProperty<ParticleEmitterComponent>("maxParticles");
    registry.AddProperty<ParticleEmitterComponent>("emitRate");
    registry.AddProperty<ParticleEmitterComponent>("lifetimeMin");
    registry.AddProperty<ParticleEmitterComponent>("lifetimeMax");
    registry.AddProperty<ParticleEmitterComponent>("speedMin");
    registry.AddProperty<ParticleEmitterComponent>("speedMax");
    registry.AddProperty<ParticleEmitterComponent>("shape");
    registry.AddProperty<ParticleEmitterComponent>("shapeRadius");
    registry.AddProperty<ParticleEmitterComponent>("coneAngle");
    registry.AddProperty<ParticleEmitterComponent>("direction");
    registry.AddProperty<ParticleEmitterComponent>("gravity");
    registry.AddProperty<ParticleEmitterComponent>("colorStart");
    registry.AddProperty<ParticleEmitterComponent>("colorEnd");
    registry.AddProperty<ParticleEmitterComponent>("sizeStart");
    registry.AddProperty<ParticleEmitterComponent>("sizeEnd");
    registry.AddProperty<ParticleEmitterComponent>("playOnStart");
    registry.AddProperty<ParticleEmitterComponent>("loop");
    registry.RegisterComponent<AudioSourceComponent>("AudioSource");
    registry.AddProperty<AudioSourceComponent>("sound");
    registry.AddProperty<AudioSourceComponent>("playOnStart");
    registry.AddProperty<AudioSourceComponent>("loop");
    registry.AddProperty<AudioSourceComponent>("volume");
    registry.AddProperty<AudioSourceComponent>("minDistance");
    registry.AddProperty<AudioSourceComponent>("maxDistance");
    registry.AddProperty<AudioSourceComponent>("spatial");
    registry.RegisterComponent<AudioListenerComponent>("AudioListener");
    registry.AddProperty<AudioListenerComponent>("active");
    registry.RegisterComponent<UICanvasComponent>("UICanvas");
    registry.AddProperty<UICanvasComponent>("uiAsset");
    registry.AddProperty<UICanvasComponent>("styleAsset");
    registry.AddProperty<UICanvasComponent>("renderInEditor");
    registry.AddProperty<UICanvasComponent>("renderInGame");
    registry.AddProperty<UICanvasComponent>("order");
    registry.AddProperty<UICanvasComponent>("scale");
    registry.RegisterComponent<TerrainComponent>("Terrain");
    registry.AddProperty<TerrainComponent>("heightmap");
    registry.AddProperty<TerrainComponent>("width");
    registry.AddProperty<TerrainComponent>("depth");
    registry.AddProperty<TerrainComponent>("heightScale");
    registry.AddProperty<TerrainComponent>("resolutionX");
    registry.AddProperty<TerrainComponent>("resolutionZ");
    registry.AddProperty<TerrainComponent>("layerCount");
    registry.AddProperty<TerrainComponent>("castShadows");
    registry.RegisterComponent<SkeletonComponent>("Skeleton");
    registry.AddProperty<SkeletonComponent>("jointCount");
    registry.RegisterComponent<AnimatorComponent>("Animator");
    registry.AddProperty<AnimatorComponent>("clipName");
    registry.AddProperty<AnimatorComponent>("speed");
    registry.AddProperty<AnimatorComponent>("loop");
    registry.AddProperty<AnimatorComponent>("playing");
}
}

