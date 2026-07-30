#pragma once

// GPU constant-buffer layouts for the viewport renderer. Every struct here is a
// CPU mirror of a cbuffer in viewport.hlsl / shadow_depth.hlsl and MUST stay
// byte-compatible with it (hence the static_asserts). Extracted from
// ViewportRenderer.cpp to keep the shader<->C++ contract in one documented spot.

#include "engine/animation/Skeleton.hpp"  // kMaxSkinJoints
#include "engine/rhi/Pipeline.hpp"        // rhi::PipelineDesc, rhi::Format
#include "render/ShadowCascades.hpp"      // shadow::kMaxCascades

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>

namespace fadix::viewport_uniforms
{
constexpr int MaxPointLights = 8;
constexpr int MaxSpotLights = 8;

struct VertexUniform
{
    glm::mat4 ViewProjection{1.0F};
    glm::mat4 PrevViewProjection{1.0F};
    glm::mat4 Model{1.0F};
    glm::mat4 PrevModel{1.0F};
    glm::vec4 SkinParams{0.0F};
    glm::mat4 NormalMatrix{1.0F};
};

struct BoneUniform
{
    glm::mat4 Bones[kMaxSkinJoints]{};
};

[[nodiscard]] inline glm::mat4 NormalMatrixFor(const glm::mat4& model)
{
    return glm::mat4{glm::inverseTranspose(glm::mat3{model})};
}

struct ShadowVertexUniform
{
    glm::mat4 LightSpaceMatrix{1.0F};
    glm::mat4 Model{1.0F};
    glm::vec4 SkinParams{0.0F}; // x = skinning enabled (matches shadow_depth.hlsl)
};

struct ShadowFragmentUniform
{
    glm::vec4 UvParams{1.0F, 1.0F, 0.0F, 0.0F};
    glm::vec4 AlphaParams{0.0F, 0.5F, 0.0F, 0.0F}; // x = mask enabled
};

// Per-frame punctual light data shared by every lit draw. Must match the
// LightSet layout inside FragmentUniforms in viewport.hlsl exactly.
struct LightSet
{
    // x = point light count, y = spot light count.
    glm::vec4 Counts{0.0F};
    glm::vec4 PointPositionRange[MaxPointLights]{};   // xyz position, w range
    glm::vec4 PointColorIntensity[MaxPointLights]{};  // rgb color, a intensity
    glm::vec4 PointParams[MaxPointLights]{};          // x falloff exponent
    glm::vec4 SpotPositionRange[MaxSpotLights]{};     // xyz position, w range
    glm::vec4 SpotDirectionFalloff[MaxSpotLights]{};  // xyz direction, w falloff
    glm::vec4 SpotColorIntensity[MaxSpotLights]{};    // rgb color, a intensity
    glm::vec4 SpotCone[MaxSpotLights]{};              // x cos(inner), y cos(outer)
};

static_assert(sizeof(LightSet) ==
    sizeof(glm::vec4) * (1 + 3 * MaxPointLights + 4 * MaxSpotLights));

struct FragmentUniform
{
    glm::vec4 BaseColor{1.0F};
    glm::vec4 Material{0.0F, 0.65F, 0.0F, 0.0F};
    glm::vec4 EmissiveColorIntensity{0.0F, 0.0F, 0.0F, 0.0F};
    glm::vec4 UvParams{1.0f, 1.0f, 0.0f, 0.0f};
    glm::vec4 AlphaParams{0.0f, 0.5f, 0.0f, 0.0f};
    glm::vec4 LightDirection{-0.4F, -1.0F, -0.25F, 0.0F};
    glm::vec4 LightColor{1.0F, 0.96F, 0.88F, 3.0F};
    glm::vec4 CameraPosition{0.0F};
    glm::vec4 AmbientSky{0.45F, 0.55F, 0.70F, 0.55F};
    glm::vec4 AmbientGround{0.28F, 0.25F, 0.22F, 0.0F};
    glm::vec4 EnvParams{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec4 FogColorDensity{0.60F, 0.66F, 0.75F, 0.0F};
    glm::vec4 FogRange{10.0F, 250.0F, 0.0F, 0.0F};
    LightSet Lights;
    glm::vec4 ShadowParams{0.0F};   // x filter radius (texels), y bias, z strength, w enabled
    glm::vec4 CascadeSplits{0.0F};  // view-space far distance of cascade 0..3
    glm::vec4 CascadeTexel{0.0F};   // UV texel size (1/resolution) of cascade 0..3
    glm::vec4 CascadeCount{0.0F};   // x active cascade count
    glm::vec4 CameraForward{0.0F};  // xyz unit camera view direction
    std::array<glm::mat4, shadow::kMaxCascades> CascadeLightSpace{
        {glm::mat4{1.0F}, glm::mat4{1.0F}, glm::mat4{1.0F}, glm::mat4{1.0F}}};
    glm::vec4 DebugParams{0.0F};  // x mode, y cascade count (diagnostic)
    glm::vec4 DebugSplits{0.0F};  // x split2, y split3, z shadow distance, w unused
};

static_assert(sizeof(FragmentUniform) ==
    sizeof(glm::vec4) * 13 + sizeof(LightSet) + sizeof(glm::vec4) * 5 +
        sizeof(glm::mat4) * shadow::kMaxCascades + sizeof(glm::vec4) * 2,
    "FragmentUniform must stay tightly packed to match viewport.hlsl");

struct SkyUniform
{
    glm::mat4 InverseViewProjection{1.0F};
    glm::mat4 PrevInverseViewProjection{1.0F};
    glm::mat4 ViewProjection{1.0F};
    glm::mat4 PrevViewProjection{1.0F};
    glm::vec4 SunDirection{-0.4F, -1.0F, -0.25F, 1.0F};
    glm::vec4 SunColor{1.0F, 0.92F, 0.78F, 1.0F};
    glm::vec4 MoonDirection{0.4F, 1.0F, 0.25F, 1.0F};
    glm::vec4 MoonColor{0.62F, 0.72F, 1.0F, 0.35F};
    glm::vec4 Params{0.0F}; // x ortho flag, y exposure
    glm::vec4 ZenithColor{0.13F, 0.27F, 0.52F, 0.0F};
    glm::vec4 HorizonColor{0.63F, 0.71F, 0.82F, 0.0F};
    glm::vec4 GroundColor{0.20F, 0.19F, 0.18F, 0.0F};
};

inline void ApplySceneMrt(rhi::PipelineDesc& description)
{
    description.ColorFormat = rhi::Format::R16G16B16A16Float;
    description.ColorFormat1 = rhi::Format::R16G16Float;
}

inline void ApplyLdrTarget(rhi::PipelineDesc& description)
{
    description.ColorFormat = rhi::Format::R8G8B8A8Unorm;
    description.ColorFormat1 = rhi::Format::Unknown;
}
} // namespace fadix::viewport_uniforms
