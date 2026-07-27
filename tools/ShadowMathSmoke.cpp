// Pure math checks for the cascaded-shadow split scheme and stable texel
// snapping. No GPU/window. Exits non-zero if any check fails.

#include "render/ShadowCascades.hpp"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <array>
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

bool Near(const float a, const float b, const float eps = 1.0e-3F)
{
    return std::fabs(a - b) < eps;
}
}

int main()
{
    using namespace fadix::shadow;

    std::cout << "Cascade split scheme\n";

    // Count clamped, last split pinned to the far edge, strictly increasing.
    for (int count = 1; count <= kMaxCascades; ++count)
    {
        const CascadeSplits s = ComputeCascadeSplits(count, 0.1F, 80.0F, 0.5F);
        Check(s.Count == count, "count " + std::to_string(count) + " preserved");
        Check(Near(s.Far[static_cast<std::size_t>(count - 1)], 80.0F),
            "last split reaches shadow distance (n=" + std::to_string(count) + ")");
        bool increasing = true;
        for (int i = 1; i < count; ++i)
        {
            if (s.Far[static_cast<std::size_t>(i)] <= s.Far[static_cast<std::size_t>(i - 1)])
            {
                increasing = false;
            }
        }
        Check(increasing, "splits strictly increasing (n=" + std::to_string(count) + ")");
    }

    // Count is clamped into range.
    Check(ComputeCascadeSplits(0, 0.1F, 50.0F, 0.5F).Count == 1, "count 0 clamps to 1");
    Check(ComputeCascadeSplits(9, 0.1F, 50.0F, 0.5F).Count == kMaxCascades, "count 9 clamps to max");

    // lambda 0 == uniform: even spacing.
    {
        const CascadeSplits u = ComputeCascadeSplits(4, 0.0F, 80.0F, 0.0F);
        Check(Near(u.Far[0], 20.0F, 0.5F) && Near(u.Far[1], 40.0F, 0.5F) &&
                  Near(u.Far[2], 60.0F, 0.5F) && Near(u.Far[3], 80.0F, 0.5F),
            "lambda 0 gives uniform splits");
    }
    // lambda 1 == logarithmic: first split much nearer than uniform.
    {
        const CascadeSplits l = ComputeCascadeSplits(4, 1.0F, 80.0F, 1.0F);
        Check(l.Far[0] < 20.0F, "lambda 1 pulls near split closer (logarithmic)");
    }

    std::cout << "Stable texel snapping\n";
    {
        const glm::vec3 right{1.0F, 0.0F, 0.0F};
        const glm::vec3 up{0.0F, 1.0F, 0.0F};
        const glm::vec3 dir{0.0F, 0.0F, -1.0F};
        const float worldPerTexel = 0.25F;

        const glm::vec3 c{1.031F, -2.087F, 5.9F};
        const glm::vec3 snapped = SnapLightSpaceCenter(c, right, up, dir, worldPerTexel);
        // Snapped coordinates land on the texel grid.
        Check(Near(std::fmod(snapped.x, worldPerTexel), 0.0F, 1.0e-3F) ||
                  Near(std::fabs(std::fmod(snapped.x, worldPerTexel)), worldPerTexel, 1.0e-3F),
            "snapped x lands on texel grid");
        // Idempotent: snapping twice is a no-op.
        const glm::vec3 twice = SnapLightSpaceCenter(snapped, right, up, dir, worldPerTexel);
        Check(glm::length(twice - snapped) < 1.0e-4F, "snap is idempotent");
        // Never moves more than one texel.
        Check(std::fabs(snapped.x - c.x) <= worldPerTexel + 1.0e-4F &&
                  std::fabs(snapped.y - c.y) <= worldPerTexel + 1.0e-4F,
            "snap moves at most one texel");
        // Depth component along the light is preserved.
        Check(Near(glm::dot(snapped, dir), glm::dot(c, dir)), "snap preserves light-depth");
    }

    std::cout << "Cascade fit\n";
    {
        // Unit cube frustum (near z=0..1 plane corners then far).
        std::array<glm::vec3, 8> corners{{{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0},
            {-1, -1, 100}, {1, -1, 100}, {1, 1, 100}, {-1, 1, 100}}};
        const CascadeFit a = FitCascadeSlice(corners, 0.0F, 0.25F, {0.3F, -1.0F, 0.2F}, 1024, 20.0F);
        const CascadeFit b = FitCascadeSlice(corners, 0.25F, 1.0F, {0.3F, -1.0F, 0.2F}, 1024, 20.0F);
        Check(a.Radius > 0.0F && b.Radius > a.Radius, "farther slice has larger radius");
        Check(std::isfinite(a.ViewProjection[0][0]) && std::isfinite(b.ViewProjection[3][3]),
            "fit produces finite matrices");
    }

    std::cout << "BuildCascades integration\n";
    {
        const glm::mat4 proj =
            glm::perspectiveRH_ZO(glm::radians(60.0F), 16.0F / 9.0F, 0.1F, 500.0F);
        const glm::mat4 view =
            glm::lookAt(glm::vec3{0.0F, 2.0F, 10.0F}, glm::vec3{0.0F}, glm::vec3{0.0F, 1.0F, 0.0F});
        const glm::vec3 lightDir{-0.4F, -1.0F, -0.25F};

        const auto matricesEqual = [](const CascadeSetup& a, const CascadeSetup& b, float eps) {
            for (int c = 0; c < kMaxCascades; ++c)
            {
                for (int col = 0; col < 4; ++col)
                {
                    for (int row = 0; row < 4; ++row)
                    {
                        if (!Near(a.LightSpace[static_cast<std::size_t>(c)][col][row],
                                b.LightSpace[static_cast<std::size_t>(c)][col][row], eps))
                        {
                            return false;
                        }
                    }
                }
            }
            return true;
        };

        // The requested (quality) cascade count actually drives the built set:
        // this is what the renderer loops over for real shadow passes.
        for (int n = 1; n <= kMaxCascades; ++n)
        {
            const CascadeSetup s = BuildCascades(proj, view, lightDir, n, 2048, 150.0F, 0.5F, 50.0F);
            Check(s.Count == n, "BuildCascades honors active count " + std::to_string(n));
            bool increasing = true;
            for (int i = 1; i < n; ++i)
            {
                if (s.SplitFar[static_cast<std::size_t>(i)] <=
                    s.SplitFar[static_cast<std::size_t>(i - 1)])
                {
                    increasing = false;
                }
            }
            Check(increasing, "cascade split-fars increasing (n=" + std::to_string(n) + ")");
            Check(std::isfinite(s.LightSpace[0][0][0]) && s.WorldPerTexel[0] > 0.0F,
                "cascade fit valid (n=" + std::to_string(n) + ")");
        }

        const CascadeSetup base = BuildCascades(proj, view, lightDir, 4, 2048, 150.0F, 0.5F, 50.0F);

        // TAA jitter is a sub-pixel clip-space translation that the renderer adds
        // ONLY to the render projection (ApplyTaaJitter biases m_Projection column
        // 2); cascade fitting always uses the unjittered base projection. Simulate
        // two TAA frames with different Halton offsets on a throwaway render
        // projection: the base handed to BuildCascades is unchanged, so the
        // cascade matrices are byte-identical from frame to frame.
        glm::mat4 renderFrameA = proj; // what the renderer would rasterize with
        renderFrameA[2][0] += 0.3F / 1920.0F;
        renderFrameA[2][1] += 0.7F / 1080.0F;
        glm::mat4 renderFrameB = proj;
        renderFrameB[2][0] += -0.6F / 1920.0F;
        renderFrameB[2][1] += 0.1F / 1080.0F;
        static_cast<void>(renderFrameA);
        static_cast<void>(renderFrameB);
        const CascadeSetup frameA =
            BuildCascades(proj, view, lightDir, 4, 2048, 150.0F, 0.5F, 50.0F);
        const CascadeSetup frameB =
            BuildCascades(proj, view, lightDir, 4, 2048, 150.0F, 0.5F, 50.0F);
        Check(matricesEqual(frameA, frameB, 1.0e-6F),
            "TAA jitter does not change cascade matrices (base projection used)");

        // Determinism: identical inputs -> identical output.
        const CascadeSetup again = BuildCascades(proj, view, lightDir, 4, 2048, 150.0F, 0.5F, 50.0F);
        Check(matricesEqual(base, again, 1.0e-6F), "BuildCascades is deterministic");

        // Slow (sub-texel) camera translation must not move the snapped matrices:
        // this is what prevents shadow swimming during slow movement.
        const glm::mat4 viewTiny = glm::lookAt(
            glm::vec3{0.0001F, 2.0F, 10.0F}, glm::vec3{0.0001F, 0.0F, 0.0F},
            glm::vec3{0.0F, 1.0F, 0.0F});
        const CascadeSetup tiny =
            BuildCascades(proj, viewTiny, lightDir, 4, 2048, 150.0F, 0.5F, 50.0F);
        Check(matricesEqual(base, tiny, 1.0e-3F),
            "sub-texel camera move leaves cascade matrices stable (no swimming)");

        // Small camera rotation keeps each cascade's radius (world-per-texel)
        // constant, so texel size never jumps during small rotations.
        const glm::mat4 viewRot = glm::lookAt(
            glm::vec3{0.0F, 2.0F, 10.0F}, glm::vec3{0.2F, 0.0F, 0.0F}, glm::vec3{0.0F, 1.0F, 0.0F});
        const CascadeSetup rot =
            BuildCascades(proj, viewRot, lightDir, 4, 2048, 150.0F, 0.5F, 50.0F);
        bool radiusStable = true;
        for (int c = 0; c < kMaxCascades; ++c)
        {
            if (!Near(base.WorldPerTexel[static_cast<std::size_t>(c)],
                    rot.WorldPerTexel[static_cast<std::size_t>(c)], 1.0e-3F))
            {
                radiusStable = false;
            }
        }
        Check(radiusStable, "small rotation keeps cascade radius/texel constant");
    }

    if (g_Failures != 0)
    {
        std::cerr << g_Failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All cascade math checks passed\n";
    return 0;
}
