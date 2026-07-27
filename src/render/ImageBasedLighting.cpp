#include "render/ImageBasedLighting.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace fadix::ibl
{
namespace
{
// Real SH basis Y_l^m evaluated for a unit direction (l up to 2, 9 bands).
std::array<float, 9> ShBasis(const glm::vec3& d) noexcept
{
    return {
        0.282095F,               // Y00
        0.488603F * d.y,         // Y1-1
        0.488603F * d.z,         // Y10
        0.488603F * d.x,         // Y11
        1.092548F * d.x * d.y,   // Y2-2
        1.092548F * d.y * d.z,   // Y2-1
        0.315392F * (3.0F * d.z * d.z - 1.0F), // Y20
        1.092548F * d.x * d.z,   // Y21
        0.546274F * (d.x * d.x - d.y * d.y)};  // Y22
}

glm::vec3 EquirectDir(const int x, const int y, const int width, const int height) noexcept
{
    const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(width);
    const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(height);
    const float phi = u * glm::two_pi<float>();
    const float theta = v * glm::pi<float>();
    const float st = std::sin(theta);
    return {st * std::cos(phi), std::cos(theta), st * std::sin(phi)};
}

float RadicalInverseVdC(std::uint32_t bits) noexcept
{
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return static_cast<float>(bits) * 2.3283064365386963e-10F;
}
}

Sh9 ProjectEquirectToSh(
    const float* pixels, const int width, const int height, const int channels) noexcept
{
    Sh9 result;
    if (pixels == nullptr || width <= 0 || height <= 0 || channels < 3)
    {
        return result;
    }
    float weightSum = 0.0F;
    for (int y = 0; y < height; ++y)
    {
        // Solid-angle weight: texels near the poles cover less of the sphere.
        const float theta = (static_cast<float>(y) + 0.5F) / static_cast<float>(height) * glm::pi<float>();
        const float sinTheta = std::sin(theta);
        for (int x = 0; x < width; ++x)
        {
            const glm::vec3 dir = EquirectDir(x, y, width, height);
            const std::size_t idx =
                (static_cast<std::size_t>(y) * width + x) * static_cast<std::size_t>(channels);
            const glm::vec3 radiance{pixels[idx], pixels[idx + 1], pixels[idx + 2]};
            const std::array<float, 9> basis = ShBasis(dir);
            for (int i = 0; i < 9; ++i)
            {
                result.Bands[static_cast<std::size_t>(i)] += radiance * (basis[static_cast<std::size_t>(i)] * sinTheta);
            }
            weightSum += sinTheta;
        }
    }
    // Normalize so the integral is over the full 4*pi sphere.
    const float norm = weightSum > 0.0F ? (4.0F * glm::pi<float>()) / weightSum : 0.0F;
    for (glm::vec3& band : result.Bands)
    {
        band *= norm;
    }
    return result;
}

glm::vec3 EvaluateShIrradiance(const Sh9& sh, const glm::vec3& normal) noexcept
{
    const glm::vec3 n = glm::length(normal) > 1.0e-6F ? glm::normalize(normal) : glm::vec3{0, 1, 0};
    // Cosine-lobe convolution constants A_l.
    constexpr float a0 = 3.141593F;   // pi
    constexpr float a1 = 2.094395F;   // 2pi/3
    constexpr float a2 = 0.785398F;   // pi/4
    const std::array<float, 9> basis = ShBasis(n);
    glm::vec3 e = sh.Bands[0] * (a0 * basis[0]);
    for (int i = 1; i < 4; ++i)
    {
        e += sh.Bands[static_cast<std::size_t>(i)] * (a1 * basis[static_cast<std::size_t>(i)]);
    }
    for (int i = 4; i < 9; ++i)
    {
        e += sh.Bands[static_cast<std::size_t>(i)] * (a2 * basis[static_cast<std::size_t>(i)]);
    }
    return glm::max(e, glm::vec3{0.0F});
}

glm::vec2 IntegrateBrdf(const float nDotV, const float roughness, const int sampleCount) noexcept
{
    const float clampedNdotV = std::clamp(nDotV, 1.0e-4F, 1.0F);
    const glm::vec3 v{std::sqrt(1.0F - clampedNdotV * clampedNdotV), 0.0F, clampedNdotV};
    const float a = std::max(roughness, 1.0e-3F);
    const float a2 = a * a;
    const int samples = std::max(sampleCount, 1);

    float scale = 0.0F;
    float bias = 0.0F;
    for (int i = 0; i < samples; ++i)
    {
        const float u = static_cast<float>(i) / static_cast<float>(samples);
        const float vdc = RadicalInverseVdC(static_cast<std::uint32_t>(i));
        // GGX importance-sampled half vector.
        const float phi = glm::two_pi<float>() * u;
        const float cosTheta = std::sqrt((1.0F - vdc) / (1.0F + (a2 - 1.0F) * vdc));
        const float sinTheta = std::sqrt(std::max(0.0F, 1.0F - cosTheta * cosTheta));
        const glm::vec3 h{sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
        const glm::vec3 l = 2.0F * glm::dot(v, h) * h - v;
        const float nDotL = std::max(l.z, 0.0F);
        if (nDotL <= 0.0F)
        {
            continue;
        }
        const float nDotH = std::max(h.z, 0.0F);
        const float vDotH = std::max(glm::dot(v, h), 0.0F);
        // Smith geometry (IBL k) + visibility weighting.
        const float k = a2 / 2.0F;
        const float gv = clampedNdotV / (clampedNdotV * (1.0F - k) + k);
        const float gl = nDotL / (nDotL * (1.0F - k) + k);
        const float g = gv * gl;
        const float gVis = (g * vDotH) / std::max(nDotH * clampedNdotV, 1.0e-5F);
        const float fc = std::pow(1.0F - vDotH, 5.0F);
        scale += (1.0F - fc) * gVis;
        bias += fc * gVis;
    }
    return {scale / static_cast<float>(samples), bias / static_cast<float>(samples)};
}
}
