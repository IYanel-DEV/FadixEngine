#include "render/ViewportGizmo.hpp"

#include "render/ViewportMath.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>

namespace fadix::viewport_gizmo
{
namespace
{
using viewport_math::ComposeMatrix;

constexpr glm::vec3 AxisColorX{0.90F, 0.24F, 0.24F};
constexpr glm::vec3 AxisColorY{0.32F, 0.80F, 0.32F};
constexpr glm::vec3 AxisColorZ{0.26F, 0.50F, 0.95F};
constexpr glm::vec3 HighlightColor{1.0F, 0.86F, 0.30F};
}

void BuildGizmoParts(
    const GizmoVisual& gizmo,
    const glm::mat4& view,
    const glm::mat4& projection,
    const float viewportHeightPixels,
    const GizmoMeshes& meshes,
    std::vector<GizmoPart>& out)
{
    namespace layout = gizmo_layout;
    out.clear();
    const float size = GizmoWorldSize(view, projection, gizmo.Position, viewportHeightPixels);
    if (size <= 0.0F)
    {
        return; // Anchor behind the camera.
    }
    const glm::quat orientation = gizmo.Orientation;
    const glm::vec3 position = gizmo.Position;

    constexpr std::array handles{GizmoHandle::AxisX, GizmoHandle::AxisY, GizmoHandle::AxisZ};
    constexpr std::array colors{AxisColorX, AxisColorY, AxisColorZ};
    const auto isHot = [&gizmo](const GizmoHandle handle) {
        return (gizmo.Active && *gizmo.Active == handle) ||
            (!gizmo.Active && gizmo.Hover && *gizmo.Hover == handle);
    };
    const auto axisRotation = [&](const int index) {
        // Rotate the +Y-aligned primitives onto the requested axis.
        if (index == 0)
        {
            return orientation * glm::angleAxis(-glm::half_pi<float>(), glm::vec3{0, 0, 1});
        }
        if (index == 2)
        {
            return orientation * glm::angleAxis(glm::half_pi<float>(), glm::vec3{1, 0, 0});
        }
        return orientation;
    };

    if (gizmo.Mode == GizmoMode::Rotate)
    {
        for (int index = 0; index < 3; ++index)
        {
            const GizmoHandle handle = handles[static_cast<std::size_t>(index)];
            // Torus circle lies in XY (around Z); rotate Z onto the axis.
            glm::quat ringRotation = orientation;
            if (index == 0)
            {
                ringRotation = orientation * glm::angleAxis(glm::half_pi<float>(), glm::vec3{0, 1, 0});
            }
            else if (index == 1)
            {
                ringRotation = orientation * glm::angleAxis(-glm::half_pi<float>(), glm::vec3{1, 0, 0});
            }
            out.push_back({handle,
                meshes.Torus,
                ComposeMatrix(position, ringRotation, glm::vec3{size * layout::RingRadius}),
                isHot(handle) ? HighlightColor : colors[static_cast<std::size_t>(index)],
                1.0F});
        }
        return;
    }

    for (int index = 0; index < 3; ++index)
    {
        const GizmoHandle handle = handles[static_cast<std::size_t>(index)];
        const glm::vec3 color =
            isHot(handle) ? HighlightColor : colors[static_cast<std::size_t>(index)];
        const glm::vec3 direction = orientation * GizmoAxisVector(static_cast<GizmoAxis>(index));
        const glm::quat rotation = axisRotation(index);
        const float shaftLength = (layout::ShaftEnd - layout::ShaftStart) * size;
        out.push_back({handle,
            meshes.Cylinder,
            ComposeMatrix(position + direction * layout::ShaftStart * size,
                rotation,
                glm::vec3{layout::ShaftRadius * 2.0F * size,
                    shaftLength,
                    layout::ShaftRadius * 2.0F * size}),
            color,
            1.0F});
        if (gizmo.Mode == GizmoMode::Translate)
        {
            out.push_back({handle,
                meshes.Cone,
                ComposeMatrix(position + direction * layout::ShaftEnd * size,
                    rotation,
                    glm::vec3{layout::ArrowRadius * 2.0F * size,
                        layout::ArrowLength * size,
                        layout::ArrowRadius * 2.0F * size}),
                color,
                1.0F});
        }
        else
        {
            out.push_back({handle,
                meshes.Cube,
                ComposeMatrix(position + direction * layout::ShaftEnd * size,
                    orientation,
                    glm::vec3{layout::ScaleCubeHalf * 2.0F * size}),
                color,
                1.0F});
        }
    }

    if (gizmo.Mode == GizmoMode::Translate)
    {
        constexpr std::array planeHandles{
            GizmoHandle::PlaneXY, GizmoHandle::PlaneXZ, GizmoHandle::PlaneYZ};
        constexpr std::array planeColors{AxisColorZ, AxisColorY, AxisColorX};
        for (std::size_t index = 0; index < planeHandles.size(); ++index)
        {
            const GizmoHandle handle = planeHandles[index];
            // Quad lives in XY; rotate onto the target plane.
            glm::quat planeRotation = orientation;
            if (handle == GizmoHandle::PlaneXZ)
            {
                planeRotation =
                    orientation * glm::angleAxis(glm::half_pi<float>(), glm::vec3{1, 0, 0});
            }
            else if (handle == GizmoHandle::PlaneYZ)
            {
                planeRotation =
                    orientation * glm::angleAxis(-glm::half_pi<float>(), glm::vec3{0, 1, 0});
            }
            const glm::vec3 planeOrigin = position +
                planeRotation *
                    glm::vec3{layout::PlaneOffset * size, layout::PlaneOffset * size, 0.0F};
            out.push_back({handle,
                meshes.Quad,
                ComposeMatrix(planeOrigin, planeRotation, glm::vec3{layout::PlaneSize * size}),
                isHot(handle) ? HighlightColor : planeColors[index],
                0.55F});
        }
    }
    else if (gizmo.Mode == GizmoMode::Scale)
    {
        out.push_back({GizmoHandle::Uniform,
            meshes.Cube,
            ComposeMatrix(position, orientation, glm::vec3{layout::UniformCubeHalf * 2.0F * size}),
            isHot(GizmoHandle::Uniform) ? HighlightColor : glm::vec3{0.85F, 0.85F, 0.88F},
            1.0F});
    }
}
}
