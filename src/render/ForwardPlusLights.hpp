#pragma once

// Forward+ (tiled forward) light assignment for the viewport renderer.
//
// The classic path packs at most 8 point lights into a uniform buffer and every
// fragment loops all of them. Forward+ instead uploads every active point light
// to a GPU storage buffer and, on the CPU, assigns each light to the screen
// tiles its bounding sphere covers. The lit fragment then loops only the lights
// in its own tile. This scales to thousands of lights instead of eight.
//
// AssignLightsToTiles is pure (no GPU, no renderer state) so it is unit-tested
// by fadix_light_stress_smoke.

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <vector>

namespace fadix::forward_plus
{
// Screen tile edge in pixels. 16x16 is the usual Forward+ default.
constexpr int kTileSize = 16;
// Per-tile light cap. A tile that overlaps more lights than this drops the
// extras (counted as overflow) rather than growing unbounded.
// ponytail: fixed cap, no per-tile overflow spill list; raise or go clustered if dense scenes need it.
constexpr int kMaxLightsPerTile = 256;
// Soft CPU sanity cap on total uploaded point lights. Not a hardware limit,
// just a guard so a runaway scene cannot allocate gigabytes.
constexpr std::uint32_t kMaxPointLights = 16384;

// One point light as stored in the GPU storage buffer. Byte-identical to the
// GpuPointLight struct in viewport.hlsl (three float4 = 48 bytes).
struct GpuPointLight
{
    glm::vec4 PositionRange{};  // xyz world position, w range
    glm::vec4 ColorIntensity{}; // rgb color, a intensity
    glm::vec4 Params{};         // x falloff exponent
};

// Per-tile (offset,count) header. Byte-identical to StructuredBuffer<uint2> in
// the shader: x = first index into Indices, y = number of lights in this tile.
struct TileHeader
{
    std::uint32_t Offset{0};
    std::uint32_t Count{0};
};

// Camera parameters needed to project a light sphere onto the tile grid. Kept
// as plain scalars (not a whole projection matrix) so tests can build one
// trivially. Proj00/Proj11 are projection[0][0]/[1][1] (the focal terms).
struct CullView
{
    glm::mat4 View{1.0F};
    float Proj00{1.0F};
    float Proj11{1.0F};
    int ScreenWidth{0};
    int ScreenHeight{0};
    // Points whose sphere sits entirely behind this eye distance are dropped.
    float NearEps{0.05F};
};

struct TileAssignment
{
    int TilesX{0};
    int TilesY{0};
    std::vector<TileHeader> Headers; // TilesX*TilesY entries
    std::vector<std::uint32_t> Indices; // packed light indices, per-tile runs
    int OverflowCount{0};            // lights dropped by the per-tile cap
};

// Assigns each light to the tiles its screen-space bounding box covers.
// Lights fully behind the camera are skipped; lights straddling the near plane
// are conservatively assigned to every tile (projection is singular there).
[[nodiscard]] TileAssignment AssignLightsToTiles(
    const std::vector<GpuPointLight>& lights, const CullView& view);
} // namespace fadix::forward_plus
