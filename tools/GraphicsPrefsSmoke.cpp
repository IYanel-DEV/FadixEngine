#include "editor/imgui/GraphicsPreferences.hpp"

#include <cmath>
#include <iostream>
#include <string>

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
}

int main()
{
    using namespace fadix;
    using namespace fadix::editor;

    std::cout << "Graphics preferences\n";

    GraphicsPreferences prefs = GraphicsPreferences::Defaults();
    Check(prefs.SceneQuality == RenderQualityPreset::Low, "default scene low");
    Check(prefs.GameQuality == RenderQualityPreset::High, "default game high");

    prefs.ResolutionScaleOverride = 0.5F;
    prefs.ShadowCascadeCap = 1;
    const RenderQualitySettings quality = prefs.ApplyTo(RenderQualityPreset::Epic);
    Check(quality.ResolutionScale <= 0.5F + 1.0e-4F, "scale override applied");
    Check(quality.ShadowCascadeCount <= 1, "cascade cap applied");

    const std::string serialized = StringifyGraphicsPreferences(prefs);
    GraphicsPreferences loaded;
    Check(ParseGraphicsPreferences(serialized, loaded), "parse ok");
    Check(std::fabs(loaded.ResolutionScaleOverride - 0.5F) < 1.0e-4F, "round-trip scale");

    if (g_Failures != 0)
    {
        std::cerr << g_Failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All graphics-preferences checks passed\n";
    return 0;
}
