// Pure checks for local-light shadow budget selection: eligibility filtering,
// budget cap, deterministic ordering, and stable UUID tie-break. Plus the
// quality budget tiers. No GPU. Exits non-zero on failure.

#include "engine/render/RenderQuality.hpp"
#include "render/LocalLightShadows.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace
{
int g_Failures = 0;

void Check(const bool condition, const std::string& label)
{
    if (condition)
    {
        std::cout << "  ok   " << label << '\n';
    }
    else
    {
        std::cerr << "  FAIL " << label << '\n';
        ++g_Failures;
    }
}

fadix::lightshadow::ShadowCandidate Make(
    const std::string& key, const bool cast, const float dist2, const float intensity,
    const float range, const bool enabled = true, const bool visible = true)
{
    fadix::lightshadow::ShadowCandidate c;
    c.Key = key;
    c.Enabled = enabled;
    c.CastShadows = cast;
    c.Visible = visible;
    c.DistanceSq = dist2;
    c.Intensity = intensity;
    c.Range = range;
    return c;
}
}

int main()
{
    using namespace fadix;
    using namespace fadix::lightshadow;

    std::cout << "Shadow selection\n";
    {
        // Only enabled + CastShadows + Visible are eligible.
        std::vector<ShadowCandidate> lights = {
            Make("a", true, 4.0F, 10.0F, 10.0F),
            Make("b", false, 1.0F, 100.0F, 20.0F),          // no CastShadows
            Make("c", true, 1.0F, 5.0F, 5.0F, false, true), // disabled
            Make("d", true, 1.0F, 5.0F, 5.0F, true, false), // invisible
            Make("e", true, 9.0F, 50.0F, 30.0F)};
        const std::vector<int> got = SelectShadowLights(lights, 4);
        Check(got.size() == 2, "ineligible lights filtered out");
        // e is brighter+longer-range despite being farther -> outranks a.
        Check(got.size() == 2 && lights[got[0]].Key == "e" && lights[got[1]].Key == "a",
            "importance orders brighter/longer-range first");
    }
    {
        // Budget caps the count; result is the top-N by importance.
        std::vector<ShadowCandidate> lights;
        for (int i = 0; i < 8; ++i)
        {
            lights.push_back(Make(std::to_string(i), true, static_cast<float>(i), 10.0F, 10.0F));
        }
        Check(SelectShadowLights(lights, 3).size() == 3, "budget caps selection to 3");
        Check(SelectShadowLights(lights, 0).empty(), "zero budget selects nothing");
        // Nearest (smallest distance) wins when intensity/range are equal.
        const std::vector<int> got = SelectShadowLights(lights, 1);
        Check(got.size() == 1 && lights[got[0]].Key == "0", "nearest wins on equal importance");
    }
    {
        // Identical everything except UUID -> deterministic key tie-break, and
        // independent of input order.
        std::vector<ShadowCandidate> forward = {
            Make("zzz", true, 2.0F, 10.0F, 10.0F), Make("aaa", true, 2.0F, 10.0F, 10.0F)};
        std::vector<ShadowCandidate> reversed = {
            Make("aaa", true, 2.0F, 10.0F, 10.0F), Make("zzz", true, 2.0F, 10.0F, 10.0F)};
        const std::vector<int> a = SelectShadowLights(forward, 1);
        const std::vector<int> b = SelectShadowLights(reversed, 1);
        Check(a.size() == 1 && forward[a[0]].Key == "aaa", "tie-break picks smaller UUID");
        Check(b.size() == 1 && reversed[b[0]].Key == "aaa", "selection is order-independent");
    }

    std::cout << "Quality shadow budgets\n";
    const RenderQualitySettings low = MakeQualitySettings(RenderQualityPreset::Low);
    const RenderQualitySettings med = MakeQualitySettings(RenderQualityPreset::Medium);
    const RenderQualitySettings high = MakeQualitySettings(RenderQualityPreset::High);
    const RenderQualitySettings epic = MakeQualitySettings(RenderQualityPreset::Epic);
    Check(low.SpotShadowBudget == 0 && low.PointShadowBudget == 0, "Low: no local shadows");
    Check(med.SpotShadowBudget == 1 && med.PointShadowBudget == 0, "Medium: 1 spot");
    Check(high.SpotShadowBudget == 4 && high.PointShadowBudget == 1, "High: 4 spot + 1 point");
    Check(epic.SpotShadowBudget >= high.SpotShadowBudget &&
              epic.PointShadowBudget >= high.PointShadowBudget,
        "Epic: higher budget");

    if (g_Failures != 0)
    {
        std::cerr << g_Failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All local-light shadow checks passed\n";
    return 0;
}
