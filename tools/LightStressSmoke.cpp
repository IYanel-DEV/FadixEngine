// Forward+ light-culling smoke test.
//
// Exercises forward_plus::AssignLightsToTiles with no GPU: correctness on a few
// hand-placed lights plus a thousands-of-lights stress pass that asserts the
// tile assignment stays sane (no crash, no runaway allocation). This is the L0
// "many lights must not crash" guard and the L1 tile-cull correctness check.

#include "render/ForwardPlusLights.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using fadix::forward_plus::AssignLightsToTiles;
using fadix::forward_plus::CullView;
using fadix::forward_plus::GpuPointLight;
using fadix::forward_plus::TileAssignment;

namespace
{
int g_failures = 0;

void Check(const bool condition, const char* label)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition)
    {
        ++g_failures;
    }
}

// A 1280x720 perspective camera at the origin looking down -Z.
CullView MakeView()
{
    constexpr float fovY = glm::radians(60.0F);
    const float proj11 = 1.0F / std::tan(fovY * 0.5F);
    const float aspect = 1280.0F / 720.0F;
    CullView view;
    view.View = glm::mat4{1.0F}; // eye at origin, forward -Z
    view.Proj11 = proj11;
    view.Proj00 = proj11 / aspect;
    view.ScreenWidth = 1280;
    view.ScreenHeight = 720;
    return view;
}

GpuPointLight MakeLight(const glm::vec3 pos, const float range)
{
    GpuPointLight light;
    light.PositionRange = glm::vec4{pos, range};
    light.ColorIntensity = glm::vec4{1.0F, 1.0F, 1.0F, 1.0F};
    return light;
}

std::size_t TotalAssignments(const TileAssignment& tiles)
{
    std::size_t total = 0;
    for (const auto& header : tiles.Headers)
    {
        total += header.Count;
    }
    return total;
}

std::uint32_t TileAt(const TileAssignment& tiles, const int px, const int py)
{
    const int tx = px / fadix::forward_plus::kTileSize;
    const int ty = py / fadix::forward_plus::kTileSize;
    const std::size_t index = static_cast<std::size_t>(ty) * tiles.TilesX + tx;
    return tiles.Headers[index].Count;
}
} // namespace

int main()
{
    const CullView view = MakeView();

    // 1. Empty scene: valid grid, zero assignments, no crash.
    {
        const TileAssignment tiles = AssignLightsToTiles({}, view);
        Check(tiles.TilesX == 80 && tiles.TilesY == 45, "empty scene tile grid is 80x45");
        Check(TotalAssignments(tiles) == 0, "empty scene assigns no lights");
    }

    // 2. Single light dead ahead lands on the centre tile.
    {
        std::vector<GpuPointLight> lights = {MakeLight({0.0F, 0.0F, -10.0F}, 2.0F)};
        const TileAssignment tiles = AssignLightsToTiles(lights, view);
        Check(TileAt(tiles, 640, 360) == 1, "centre light hits the centre tile");
        Check(TotalAssignments(tiles) >= 1, "centre light is assigned");
    }

    // 3. Light entirely behind the camera is dropped.
    {
        std::vector<GpuPointLight> lights = {MakeLight({0.0F, 0.0F, 10.0F}, 1.0F)};
        const TileAssignment tiles = AssignLightsToTiles(lights, view);
        Check(TotalAssignments(tiles) == 0, "light behind camera is culled");
    }

    // 4. Light far off to the side never touches the screen.
    {
        std::vector<GpuPointLight> lights = {MakeLight({1000.0F, 0.0F, -10.0F}, 1.0F)};
        const TileAssignment tiles = AssignLightsToTiles(lights, view);
        Check(TotalAssignments(tiles) == 0, "off-screen light is culled");
    }

    // 5. Light straddling the near plane is conservatively put in every tile.
    {
        std::vector<GpuPointLight> lights = {MakeLight({0.0F, 0.0F, -1.0F}, 5.0F)};
        const TileAssignment tiles = AssignLightsToTiles(lights, view);
        const std::size_t tileCount = static_cast<std::size_t>(tiles.TilesX) * tiles.TilesY;
        Check(TotalAssignments(tiles) == tileCount, "near-plane light covers all tiles");
    }

    // 6. Stress: thousands of lights in front of the camera. Must not crash and
    //    must assign a plausible, bounded number of tile slots.
    {
        std::mt19937 rng(1234);
        std::uniform_real_distribution<float> xy(-40.0F, 40.0F);
        std::uniform_real_distribution<float> z(-80.0F, -5.0F);
        std::uniform_real_distribution<float> r(1.0F, 6.0F);
        std::vector<GpuPointLight> lights;
        lights.reserve(5000);
        for (int i = 0; i < 5000; ++i)
        {
            lights.push_back(MakeLight({xy(rng), xy(rng), z(rng)}, r(rng)));
        }
        const TileAssignment tiles = AssignLightsToTiles(lights, view);
        const std::size_t tileCount = static_cast<std::size_t>(tiles.TilesX) * tiles.TilesY;
        Check(tiles.Headers.size() == tileCount, "stress: header table covers every tile");
        Check(TotalAssignments(tiles) == tiles.Indices.size(),
            "stress: header counts match packed index list");
        Check(TotalAssignments(tiles) > 0, "stress: some lights are assigned");
        std::printf("       5000 lights -> %zu tile slots, %d overflow\n",
            TotalAssignments(tiles), tiles.OverflowCount);
    }

    if (g_failures == 0)
    {
        std::printf("\nfadix_light_stress_smoke: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("\nfadix_light_stress_smoke: %d CHECK(S) FAILED\n", g_failures);
    return 1;
}
