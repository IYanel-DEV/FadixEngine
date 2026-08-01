#pragma once

// Pure transform-gizmo geometry assembly extracted from ViewportRenderer.cpp.
// Given the gizmo state, camera, and the renderer's primitive mesh ranges, it
// produces the list of parts to draw. No GPU or renderer-state dependency.

#include "engine/render/GizmoTypes.hpp"
#include "engine/render/ViewportRenderer.hpp"
#include "render/ViewportGeometry.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <vector>

namespace fadix::viewport_gizmo
{
struct GizmoPart
{
    GizmoHandle Handle;
    const viewport_geometry::MeshRange* Mesh;
    glm::mat4 Model;
    glm::vec3 Color;
    float Alpha;
};

// The primitive mesh ranges the gizmo draws with. Pointers are owned elsewhere
// (the renderer's mesh library) and must outlive the produced parts.
struct GizmoMeshes
{
    const viewport_geometry::MeshRange* Torus;
    const viewport_geometry::MeshRange* Cylinder;
    const viewport_geometry::MeshRange* Cone;
    const viewport_geometry::MeshRange* Cube;
    const viewport_geometry::MeshRange* Quad;
};

// Fills `out` (cleared first) with the parts for the current gizmo mode/handle.
// Leaves it empty when the gizmo anchor is at or behind the camera.
void BuildGizmoParts(
    const GizmoVisual& gizmo,
    const glm::mat4& view,
    const glm::mat4& projection,
    float viewportHeightPixels,
    const GizmoMeshes& meshes,
    std::vector<GizmoPart>& out);
}
