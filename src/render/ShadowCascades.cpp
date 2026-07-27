#include "render/ShadowCascades.hpp"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace fadix::shadow
{
CascadeSplits ComputeCascadeSplits(
    const int count, const float nearPlane, const float shadowDistance, const float lambda) noexcept
{
    CascadeSplits result;
    result.Count = std::clamp(count, 1, kMaxCascades);
    const float clampedNear = std::max(nearPlane, 1.0e-3F);
    const float far = std::max(shadowDistance, clampedNear + 1.0e-3F);
    const float ratio = far / clampedNear;
    const float blend = std::clamp(lambda, 0.0F, 1.0F);

    for (int i = 0; i < result.Count; ++i)
    {
        const float p = static_cast<float>(i + 1) / static_cast<float>(result.Count);
        const float logSplit = clampedNear * std::pow(ratio, p);
        const float uniformSplit = clampedNear + (far - clampedNear) * p;
        result.Far[static_cast<std::size_t>(i)] = blend * logSplit + (1.0F - blend) * uniformSplit;
    }
    // Pin the last split exactly to the far edge so no gap is left uncovered.
    result.Far[static_cast<std::size_t>(result.Count - 1)] = far;
    return result;
}

glm::vec3 SnapLightSpaceCenter(
    const glm::vec3& center,
    const glm::vec3& right,
    const glm::vec3& up,
    const glm::vec3& dir,
    const float worldPerTexel) noexcept
{
    if (worldPerTexel <= 0.0F)
    {
        return center;
    }
    const float cx = std::round(glm::dot(center, right) / worldPerTexel) * worldPerTexel;
    const float cy = std::round(glm::dot(center, up) / worldPerTexel) * worldPerTexel;
    const float cz = glm::dot(center, dir);
    return right * cx + up * cy + dir * cz;
}

CascadeFit FitCascadeSlice(
    const std::array<glm::vec3, 8>& corners,
    const float nearFraction,
    const float farFraction,
    const glm::vec3& lightDir,
    const int resolution,
    const float casterMargin) noexcept
{
    const glm::vec3 direction = glm::normalize(lightDir);

    // Slice corners: interpolate each near->far frustum edge by the split
    // fractions so the fit tracks exactly this depth band.
    std::array<glm::vec3, 8> slice{};
    for (int k = 0; k < 4; ++k)
    {
        const glm::vec3 nearCorner = corners[static_cast<std::size_t>(k)];
        const glm::vec3 farCorner = corners[static_cast<std::size_t>(k + 4)];
        slice[static_cast<std::size_t>(k)] = glm::mix(nearCorner, farCorner, nearFraction);
        slice[static_cast<std::size_t>(k + 4)] = glm::mix(nearCorner, farCorner, farFraction);
    }

    glm::vec3 center{0.0F};
    for (const glm::vec3& corner : slice)
    {
        center += corner;
    }
    center /= 8.0F;

    float radius = 1.0F;
    for (const glm::vec3& corner : slice)
    {
        radius = std::max(radius, glm::length(corner - center));
    }
    // Quantize so the texel size only changes in discrete steps, keeping the
    // snap grid stable across small camera rotations.
    radius = std::ceil(radius);

    glm::vec3 up{0.0F, 1.0F, 0.0F};
    if (std::abs(glm::dot(direction, up)) > 0.99F)
    {
        up = glm::vec3{0.0F, 0.0F, 1.0F};
    }
    const glm::vec3 right = glm::normalize(glm::cross(up, direction));
    const glm::vec3 realUp = glm::cross(direction, right);

    const float worldPerTexel = (2.0F * radius) / static_cast<float>(std::max(resolution, 1));
    center = SnapLightSpaceCenter(center, right, realUp, direction, worldPerTexel);

    const glm::vec3 eye = center - direction * (radius + casterMargin);
    const glm::mat4 lightView = glm::lookAt(eye, center, up);
    const glm::mat4 lightProj =
        glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0F, 2.0F * radius + casterMargin);

    CascadeFit fit;
    fit.ViewProjection = lightProj * lightView;
    fit.Center = center;
    fit.Radius = radius;
    return fit;
}

std::array<glm::vec3, 8> FrustumCornersWorld(
    const glm::mat4& projection, const glm::mat4& view) noexcept
{
    // RH_ZO clip space: z in [0,1] (0 = near, 1 = far). Order near0..3, far0..3
    // with matching xy so FitCascadeSlice pairs corner[k] with corner[k+4].
    static constexpr std::array<glm::vec3, 8> ndc{{
        {-1.0F, -1.0F, 0.0F}, {1.0F, -1.0F, 0.0F}, {-1.0F, 1.0F, 0.0F}, {1.0F, 1.0F, 0.0F},
        {-1.0F, -1.0F, 1.0F}, {1.0F, -1.0F, 1.0F}, {-1.0F, 1.0F, 1.0F}, {1.0F, 1.0F, 1.0F}}};
    const glm::mat4 inverse = glm::inverse(projection * view);
    std::array<glm::vec3, 8> corners{};
    for (std::size_t i = 0; i < corners.size(); ++i)
    {
        const glm::vec4 world = inverse * glm::vec4{ndc[i], 1.0F};
        corners[i] = glm::vec3{world} / world.w;
    }
    return corners;
}

CascadeSetup BuildCascades(
    const glm::mat4& baseProjection,
    const glm::mat4& view,
    const glm::vec3& lightDir,
    const int count,
    const int resolution,
    const float shadowDistance,
    const float lambda,
    const float casterMargin) noexcept
{
    CascadeSetup setup;
    setup.Count = std::clamp(count, 1, kMaxCascades);

    const std::array<glm::vec3, 8> corners = FrustumCornersWorld(baseProjection, view);
    // View-space depth (positive in front) of the near/far frustum planes, read
    // straight off the frustum so this is correct for perspective and ortho.
    const float zNear = -(view * glm::vec4{corners[0], 1.0F}).z;
    const float zFar = -(view * glm::vec4{corners[4], 1.0F}).z;
    const float clampedNear = std::max(zNear, 1.0e-3F);
    const float span = std::max(zFar - clampedNear, 1.0e-3F);
    const float coveredFar = std::clamp(shadowDistance, clampedNear + 1.0e-3F, zFar);

    const CascadeSplits splits =
        ComputeCascadeSplits(setup.Count, clampedNear, coveredFar, lambda);

    for (int i = 0; i < setup.Count; ++i)
    {
        const float sliceNear =
            (i == 0) ? clampedNear : splits.Far[static_cast<std::size_t>(i - 1)];
        const float sliceFar = splits.Far[static_cast<std::size_t>(i)];
        const float nearFraction = (sliceNear - clampedNear) / span;
        const float farFraction = (sliceFar - clampedNear) / span;
        const CascadeFit fit =
            FitCascadeSlice(corners, nearFraction, farFraction, lightDir, resolution, casterMargin);
        setup.LightSpace[static_cast<std::size_t>(i)] = fit.ViewProjection;
        setup.SplitFar[static_cast<std::size_t>(i)] = sliceFar;
        setup.WorldPerTexel[static_cast<std::size_t>(i)] =
            (2.0F * fit.Radius) / static_cast<float>(std::max(resolution, 1));
    }
    return setup;
}
}
