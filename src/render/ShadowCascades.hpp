#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>

namespace fadix::shadow
{
inline constexpr int kMaxCascades = 4;

// Far view-space distances of each cascade under the practical split scheme.
// Count is clamped to [1, kMaxCascades]; Far[Count-1] == shadowDistance exactly.
struct CascadeSplits
{
    std::array<float, kMaxCascades> Far{{0.0F, 0.0F, 0.0F, 0.0F}};
    int Count{1};
};

// Practical split scheme (Zhang et al. / "PSSM"): blends a logarithmic split
// (good near-field detail) with a uniform split by `lambda` in [0,1]. lambda 0
// = uniform, 1 = logarithmic. Distances are strictly increasing; the last one
// is pinned to shadowDistance so the final cascade always reaches the far edge.
[[nodiscard]] CascadeSplits ComputeCascadeSplits(
    int count, float nearPlane, float shadowDistance, float lambda) noexcept;

// Snaps a light-space center so the shadow map only translates in whole-texel
// steps along the light's right/up axes. This is what stops the shadow from
// swimming as the camera moves (idempotent: Snap(Snap(x)) == Snap(x)).
[[nodiscard]] glm::vec3 SnapLightSpaceCenter(
    const glm::vec3& center,
    const glm::vec3& right,
    const glm::vec3& up,
    const glm::vec3& dir,
    float worldPerTexel) noexcept;

// Stable per-cascade light-space fit. Bounds the frustum slice between
// nearFraction and farFraction (0..1 along each near->far edge of `corners`,
// ordered near0..3 then far0..3) with a rotation-stable sphere, quantizes the
// radius, and texel-snaps the center. Returns a light view-projection plus the
// sphere it was built from. This is the single-cascade stabilized fit extended
// to operate on an arbitrary slice.
struct CascadeFit
{
    glm::mat4 ViewProjection{1.0F};
    glm::vec3 Center{0.0F};
    float Radius{1.0F};
};

[[nodiscard]] CascadeFit FitCascadeSlice(
    const std::array<glm::vec3, 8>& corners,
    float nearFraction,
    float farFraction,
    const glm::vec3& lightDir,
    int resolution,
    float casterMargin) noexcept;

// The eight world-space corners of a camera frustum, ordered near0..3 then
// far0..3 (matching FitCascadeSlice). Pass the UNJITTERED base projection: a
// TAA sub-pixel jitter must never reach cascade fitting.
[[nodiscard]] std::array<glm::vec3, 8> FrustumCornersWorld(
    const glm::mat4& projection, const glm::mat4& view) noexcept;

// Fully-built directional cascade set: one light view-projection per active
// cascade, the view-space far split distances used to select a cascade in the
// shader, and the world-per-texel of each cascade (for depth-bias scaling /
// diagnostics). Inactive entries stay identity / 0.
struct CascadeSetup
{
    std::array<glm::mat4, kMaxCascades> LightSpace{
        {glm::mat4{1.0F}, glm::mat4{1.0F}, glm::mat4{1.0F}, glm::mat4{1.0F}}};
    std::array<float, kMaxCascades> SplitFar{{0.0F, 0.0F, 0.0F, 0.0F}};
    std::array<float, kMaxCascades> WorldPerTexel{{0.0F, 0.0F, 0.0F, 0.0F}};
    int Count{1};
};

// Builds every active cascade from the camera's base (unjittered) projection.
// This is the single source of truth shared by the renderer and the smoke
// tests: it derives the camera near/far from the frustum itself (works for
// perspective and orthographic), pins the last split to shadowDistance, and
// fits each depth slice with the rotation-stable, texel-snapped fitter above.
// Because the fitter quantizes radius and snaps the center, a sub-texel change
// to the projection (TAA jitter) or a sub-texel camera translation produces the
// exact same matrices.
[[nodiscard]] CascadeSetup BuildCascades(
    const glm::mat4& baseProjection,
    const glm::mat4& view,
    const glm::vec3& lightDir,
    int count,
    int resolution,
    float shadowDistance,
    float lambda,
    float casterMargin) noexcept;
}
