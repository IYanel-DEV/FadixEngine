// Pure math checks for the image-based-lighting convolution cores: SH diffuse
// irradiance projection and split-sum BRDF integration. No GPU. Exits non-zero
// on failure.

#include "render/ImageBasedLighting.hpp"

#include <glm/geometric.hpp>

#include <cmath>
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

bool Near(const float a, const float b, const float eps)
{
    return std::fabs(a - b) < eps;
}
}

int main()
{
    using namespace fadix::ibl;

    std::cout << "SH diffuse irradiance\n";
    // Constant white environment: irradiance must be uniform and ~ the input
    // radiance for every normal (a constant field has only the DC band).
    {
        const int w = 32;
        const int h = 16;
        std::vector<float> img(static_cast<std::size_t>(w) * h * 3, 0.0F);
        for (float& value : img)
        {
            value = 0.5F;
        }
        const Sh9 sh = ProjectEquirectToSh(img.data(), w, h, 3);
        const glm::vec3 up = EvaluateShIrradiance(sh, {0, 1, 0});
        const glm::vec3 side = EvaluateShIrradiance(sh, {1, 0, 0});
        const glm::vec3 down = EvaluateShIrradiance(sh, {0, -1, 0});
        Check(up.x > 0.0F, "constant env gives positive irradiance");
        Check(Near(up.x, side.x, 0.05F) && Near(up.x, down.x, 0.05F),
            "constant env irradiance is directionally uniform");
        // Cosine-convolved irradiance of a constant radiance L is pi*L.
        Check(Near(up.x, 3.14159F * 0.5F, 0.1F), "constant env irradiance ~= pi*L");
    }
    // A bright top hemisphere should light an up-facing normal more than a
    // down-facing one.
    {
        const int w = 32;
        const int h = 16;
        std::vector<float> img(static_cast<std::size_t>(w) * h * 3, 0.0F);
        for (int y = 0; y < h; ++y)
        {
            const float value = y < h / 2 ? 1.0F : 0.0F; // top rows bright
            for (int x = 0; x < w; ++x)
            {
                const std::size_t idx = (static_cast<std::size_t>(y) * w + x) * 3;
                img[idx] = img[idx + 1] = img[idx + 2] = value;
            }
        }
        const Sh9 sh = ProjectEquirectToSh(img.data(), w, h, 3);
        const float top = EvaluateShIrradiance(sh, {0, 1, 0}).x;
        const float bottom = EvaluateShIrradiance(sh, {0, -1, 0}).x;
        Check(top > bottom, "up normal brighter under a bright top hemisphere");
    }

    std::cout << "Split-sum BRDF\n";
    {
        const glm::vec2 smooth = IntegrateBrdf(1.0F, 0.02F, 256);
        Check(smooth.x > 0.9F && smooth.x <= 1.01F, "smooth grazing scale ~1");
        Check(smooth.y >= 0.0F && smooth.y < 0.1F, "smooth bias ~0");
        for (float r = 0.1F; r <= 1.0F; r += 0.3F)
        {
            const glm::vec2 b = IntegrateBrdf(0.5F, r, 128);
            Check(b.x >= 0.0F && b.x <= 1.0F && b.y >= 0.0F && b.y <= 1.0F,
                "BRDF LUT in [0,1] (r=" + std::to_string(r) + ")");
        }
    }

    if (g_Failures != 0)
    {
        std::cerr << g_Failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All IBL math checks passed\n";
    return 0;
}
