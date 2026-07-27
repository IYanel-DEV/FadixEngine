#include "render/LocalLightShadows.hpp"

#include <algorithm>

namespace fadix::lightshadow
{
float ShadowImportance(const ShadowCandidate& candidate) noexcept
{
    // Radiant reach over distance. +1 avoids a singularity at the camera and
    // keeps the ordering finite for co-located lights.
    return (candidate.Intensity * candidate.Range) / (candidate.DistanceSq + 1.0F);
}

std::vector<int> SelectShadowLights(
    const std::vector<ShadowCandidate>& candidates, const int budget)
{
    std::vector<int> eligible;
    eligible.reserve(candidates.size());
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i)
    {
        const ShadowCandidate& c = candidates[static_cast<std::size_t>(i)];
        if (c.Enabled && c.CastShadows && c.Visible)
        {
            eligible.push_back(i);
        }
    }

    std::sort(eligible.begin(), eligible.end(), [&candidates](const int a, const int b) {
        const ShadowCandidate& ca = candidates[static_cast<std::size_t>(a)];
        const ShadowCandidate& cb = candidates[static_cast<std::size_t>(b)];
        const float ia = ShadowImportance(ca);
        const float ib = ShadowImportance(cb);
        if (ia != ib)
        {
            return ia > ib; // more important first
        }
        if (ca.DistanceSq != cb.DistanceSq)
        {
            return ca.DistanceSq < cb.DistanceSq; // nearest first
        }
        return ca.Key < cb.Key; // stable UUID tie-break
    });

    const int cap = std::max(budget, 0);
    if (static_cast<int>(eligible.size()) > cap)
    {
        eligible.resize(static_cast<std::size_t>(cap));
    }
    return eligible;
}
}
