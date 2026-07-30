#include "render/ForwardPlusLights.hpp"

#include <algorithm>
#include <cmath>

namespace fadix::forward_plus
{
namespace
{
// Screen-tile rectangle a light touches (inclusive tile indices).
struct TileRect
{
    int MinX{0};
    int MinY{0};
    int MaxX{0};
    int MaxY{0};
};
} // namespace

TileAssignment AssignLightsToTiles(
    const std::vector<GpuPointLight>& lights, const CullView& view)
{
    TileAssignment result;
    if (view.ScreenWidth <= 0 || view.ScreenHeight <= 0)
    {
        return result;
    }
    result.TilesX = (view.ScreenWidth + kTileSize - 1) / kTileSize;
    result.TilesY = (view.ScreenHeight + kTileSize - 1) / kTileSize;
    const int tileCount = result.TilesX * result.TilesY;
    if (tileCount <= 0)
    {
        return result;
    }

    // Per-tile scratch lists, flattened into Headers/Indices at the end.
    std::vector<std::vector<std::uint32_t>> buckets(static_cast<std::size_t>(tileCount));

    const float halfW = 0.5F * static_cast<float>(view.ScreenWidth);
    const float halfH = 0.5F * static_cast<float>(view.ScreenHeight);
    const int lastTileX = result.TilesX - 1;
    const int lastTileY = result.TilesY - 1;

    const auto markRect = [&](const TileRect& rect, const std::uint32_t lightIndex) {
        for (int ty = rect.MinY; ty <= rect.MaxY; ++ty)
        {
            for (int tx = rect.MinX; tx <= rect.MaxX; ++tx)
            {
                std::vector<std::uint32_t>& bucket =
                    buckets[static_cast<std::size_t>(ty) * result.TilesX + tx];
                if (static_cast<int>(bucket.size()) < kMaxLightsPerTile)
                {
                    bucket.push_back(lightIndex);
                }
                else
                {
                    ++result.OverflowCount;
                }
            }
        }
    };

    const TileRect fullScreen{0, 0, lastTileX, lastTileY};

    for (std::size_t i = 0; i < lights.size(); ++i)
    {
        const glm::vec4 center{glm::vec3{lights[i].PositionRange}, 1.0F};
        const float range = lights[i].PositionRange.w;
        if (range <= 0.0F)
        {
            continue;
        }
        const glm::vec4 viewPos = view.View * center;
        // View forward is -Z (glm::lookAt convention); depth is positive ahead.
        const float depth = -viewPos.z;
        if (depth + range <= view.NearEps)
        {
            continue; // wholly behind the camera
        }
        const auto lightIndex = static_cast<std::uint32_t>(i);
        if (depth - range < view.NearEps)
        {
            // Straddles the near plane; projection blows up, so be conservative.
            markRect(fullScreen, lightIndex);
            continue;
        }

        // Screen-space bounding box of the sphere. ndc = focal * axis / depth.
        const float ndcCenterX = view.Proj00 * viewPos.x / depth;
        const float ndcCenterY = view.Proj11 * viewPos.y / depth;
        const float ndcRadiusX = view.Proj00 * range / depth;
        const float ndcRadiusY = view.Proj11 * range / depth;

        // NDC [-1,1] -> pixels. SDL GPU NDC is Y-up while the framebuffer row 0
        // is the top, so Y is flipped (matches SV_Position in the fragment).
        const float pxMin = (ndcCenterX - ndcRadiusX + 1.0F) * halfW;
        const float pxMax = (ndcCenterX + ndcRadiusX + 1.0F) * halfW;
        const float pyA = (1.0F - (ndcCenterY - ndcRadiusY)) * halfH;
        const float pyB = (1.0F - (ndcCenterY + ndcRadiusY)) * halfH;
        const float pyMin = std::min(pyA, pyB);
        const float pyMax = std::max(pyA, pyB);

        const float maxX = static_cast<float>(view.ScreenWidth - 1);
        const float maxY = static_cast<float>(view.ScreenHeight - 1);
        if (pxMax < 0.0F || pyMax < 0.0F || pxMin > maxX || pyMin > maxY)
        {
            continue; // fully off-screen
        }

        TileRect rect;
        rect.MinX = std::clamp(static_cast<int>(std::floor(std::max(pxMin, 0.0F))) / kTileSize, 0, lastTileX);
        rect.MaxX = std::clamp(static_cast<int>(std::floor(std::min(pxMax, maxX))) / kTileSize, 0, lastTileX);
        rect.MinY = std::clamp(static_cast<int>(std::floor(std::max(pyMin, 0.0F))) / kTileSize, 0, lastTileY);
        rect.MaxY = std::clamp(static_cast<int>(std::floor(std::min(pyMax, maxY))) / kTileSize, 0, lastTileY);
        markRect(rect, lightIndex);
    }

    // Flatten buckets into the offset/count table and packed index list.
    result.Headers.resize(static_cast<std::size_t>(tileCount));
    std::size_t total = 0;
    for (const std::vector<std::uint32_t>& bucket : buckets)
    {
        total += bucket.size();
    }
    result.Indices.reserve(total);
    for (int t = 0; t < tileCount; ++t)
    {
        const std::vector<std::uint32_t>& bucket = buckets[static_cast<std::size_t>(t)];
        result.Headers[static_cast<std::size_t>(t)] = TileHeader{
            static_cast<std::uint32_t>(result.Indices.size()),
            static_cast<std::uint32_t>(bucket.size())};
        result.Indices.insert(result.Indices.end(), bucket.begin(), bucket.end());
    }
    return result;
}
} // namespace fadix::forward_plus
