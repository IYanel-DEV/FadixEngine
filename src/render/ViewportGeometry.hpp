#pragma once

// Procedural primitive mesh generation for the viewport renderer's built-in
// mesh library (cube, sphere, cone, cylinder, torus, quad, plane, capsule).
// Extracted from ViewportRenderer.cpp -- these are pure, self-contained CPU
// geometry builders with no dependency on the renderer or the GPU.

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <vector>

namespace fadix::viewport_geometry
{
// Interleaved vertex layout shared by every viewport mesh. Must stay 96 bytes
// so it matches the vertex-buffer stride expected by viewport.hlsl.
struct Vertex
{
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec4 Tangent;
    glm::vec2 UV;
    glm::vec4 Color;
    glm::vec4 JointIndices{0.0F};
    glm::vec4 JointWeights{0.0F};
};
static_assert(sizeof(Vertex) == 96);

// A contiguous slice of the shared index buffer identifying one primitive mesh.
struct MeshRange
{
    std::uint32_t FirstIndex{0};
    std::uint32_t IndexCount{0};
};

void AppendCube(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices);
void AppendCylinder(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, int segments);
void AppendCone(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, int segments);
void AppendSphere(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, int slices, int stacks);
void AppendTorus(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, float tubeRadius, int majorSegments, int minorSegments);
void AppendQuad(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices);
void AppendPlanePrimitive(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices);
// XY-plane unit quad centered at origin, front face only (for sprite rendering).
// UV (0,0) = top-left, (1,1) = bottom-right in image space with Y-up world.
void AppendSpriteQuad(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices);
void AppendCapsule(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, int slices, int stacks);
} // namespace fadix::viewport_geometry
