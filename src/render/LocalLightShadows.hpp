#pragma once

#include <string>
#include <vector>

namespace fadix::lightshadow
{
// One local light considered for a shadow-map slot this frame. Key is the stable
// UUID string used as the final deterministic tie-break.
struct ShadowCandidate
{
    std::string Key;
    bool Enabled{true};
    bool CastShadows{false};
    bool Visible{true};
    float DistanceSq{0.0F}; // squared distance to the camera
    float Intensity{1.0F};
    float Range{1.0F};
};

// Importance heuristic: brighter, longer-range, closer lights matter more.
// Exposed so the renderer and the smoke agree on ordering.
[[nodiscard]] float ShadowImportance(const ShadowCandidate& candidate) noexcept;

// Deterministically selects up to `budget` shadow-casting lights. Only
// enabled + CastShadows + Visible candidates are eligible. Ordering is by
// descending importance, then ascending distance, then ascending UUID key, so
// the result never depends on input order or container iteration order. Returns
// indices into `candidates` in selected (priority) order.
[[nodiscard]] std::vector<int> SelectShadowLights(
    const std::vector<ShadowCandidate>& candidates, int budget);
}
