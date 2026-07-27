#pragma once

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>

namespace fadix
{
enum class GizmoMode
{
    Select,
    Translate,
    Rotate,
    Scale
};

enum class GizmoAxis
{
    X,
    Y,
    Z
};

// A concrete interactive part of the transform gizmo. Axis handles exist in
// every mode; plane handles only for translate; Uniform only for scale.
enum class GizmoHandle
{
    AxisX,
    AxisY,
    AxisZ,
    PlaneXY,
    PlaneXZ,
    PlaneYZ,
    Uniform
};

[[nodiscard]] constexpr glm::vec3 GizmoAxisVector(const GizmoAxis axis) noexcept
{
    switch (axis)
    {
    case GizmoAxis::X: return {1.0F, 0.0F, 0.0F};
    case GizmoAxis::Y: return {0.0F, 1.0F, 0.0F};
    case GizmoAxis::Z: return {0.0F, 0.0F, 1.0F};
    }
    return {0.0F, 0.0F, 0.0F};
}

[[nodiscard]] constexpr bool GizmoHandleIsAxis(const GizmoHandle handle) noexcept
{
    return handle == GizmoHandle::AxisX || handle == GizmoHandle::AxisY ||
        handle == GizmoHandle::AxisZ;
}

// Axis a handle operates on. For plane handles this is the plane normal.
[[nodiscard]] constexpr GizmoAxis GizmoHandleAxis(const GizmoHandle handle) noexcept
{
    switch (handle)
    {
    case GizmoHandle::AxisX: return GizmoAxis::X;
    case GizmoHandle::AxisY: return GizmoAxis::Y;
    case GizmoHandle::AxisZ: return GizmoAxis::Z;
    case GizmoHandle::PlaneYZ: return GizmoAxis::X;
    case GizmoHandle::PlaneXZ: return GizmoAxis::Y;
    case GizmoHandle::PlaneXY: return GizmoAxis::Z;
    case GizmoHandle::Uniform: return GizmoAxis::X;
    }
    return GizmoAxis::X;
}

// Shared gizmo layout, in multiples of the per-frame gizmo world size.
// The renderer and the editor's ray hit-testing must both use these so the
// hover/drag zones line up with the drawn geometry.
namespace gizmo_layout
{
inline constexpr float ShaftStart = 0.18F;
inline constexpr float ShaftEnd = 1.0F;
inline constexpr float ShaftRadius = 0.018F;
inline constexpr float ArrowLength = 0.22F;
inline constexpr float ArrowRadius = 0.055F;
inline constexpr float PlaneOffset = 0.38F;
inline constexpr float PlaneSize = 0.17F;
inline constexpr float RingRadius = 1.0F;
inline constexpr float RingTubeRadius = 0.016F;
inline constexpr float ScaleCubeHalf = 0.06F;
inline constexpr float UniformCubeHalf = 0.08F;
// Extra grab slack, in multiples of gizmo size, added on top of the visual
// radius so thin handles stay easy to hit without stealing neighbors.
inline constexpr float PickSlack = 0.06F;
}

// Half extents (unit entity scale) of the editor-only camera model, and the
// pick radius of the editor-only sun model. Pickable bounds must match these.
inline constexpr glm::vec3 CameraGizmoHalfExtents{0.30F, 0.36F, 0.70F};
inline constexpr float SunGizmoRadius = 0.55F;

// Target on-screen length from the gizmo center to an axis tip, in pixels.
inline constexpr float GizmoPixelSize = 90.0F;

// Projection-aware world size for the gizmo: `GizmoPixelSize` pixels at the
// gizmo anchor for both perspective and orthographic projections, at any
// viewport height (physical pixels, so high-DPI targets stay consistent).
// Returns 0 when the anchor is at or behind the camera plane, which callers
// must treat as "gizmo hidden". Renderer and hit-testing must both use this so
// hover/drag zones line up with the drawn geometry at any zoom level.
[[nodiscard]] inline float GizmoWorldSize(
    const glm::mat4& view,
    const glm::mat4& projection,
    const glm::vec3& gizmoPosition,
    const float viewportHeightPixels) noexcept
{
    // projection[1][1] is 1/tan(fov/2) for perspective, 2/visibleHeight for ortho.
    const float projectionScale = projection[1][1];
    if (projectionScale <= 0.0F || viewportHeightPixels <= 0.0F)
    {
        return 0.0F;
    }
    const bool orthographic = std::abs(projection[3][3] - 1.0F) < 1.0e-4F;
    const glm::vec4 clip = projection * view * glm::vec4{gizmoPosition, 1.0F};
    const float depth = orthographic ? 1.0F : clip.w;
    if (depth <= 1.0e-4F)
    {
        return 0.0F;
    }
    const float worldUnitsPerPixel = 2.0F * depth / (projectionScale * viewportHeightPixels);
    float size = GizmoPixelSize * worldUnitsPerPixel;
    if (!orthographic)
    {
        // Keep the geometry (tips reach ~1.3x size from the anchor) from
        // extending through the camera/near plane at very close range.
        size = std::min(size, depth * 0.5F);
    }
    return size;
}
}
