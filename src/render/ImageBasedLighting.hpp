#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>

namespace fadix::ibl
{
// Bumping this forces cached IBL data to regenerate when the convolution changes.
inline constexpr std::uint32_t kIblCacheVersion = 1;

// Order-2 spherical-harmonic representation of diffuse irradiance (9 RGB bands).
// Compact enough to live in a uniform buffer, so diffuse IBL needs no texture.
struct Sh9
{
    std::array<glm::vec3, 9> Bands{};
};

// Projects an equirectangular HDR image (row-major float RGB(A), `channels`>=3,
// +Y up, theta down from the pole) onto SH irradiance bands. This is the diffuse
// convolution and is meant to run once at import, not per frame.
[[nodiscard]] Sh9 ProjectEquirectToSh(
    const float* pixels, int width, int height, int channels) noexcept;

// Reconstructs cosine-convolved irradiance for a surface normal from SH bands
// (Ramamoorthi & Hanrahan 2001). Returns radiance-scale irradiance; the shader
// multiplies by albedo/PI for the final diffuse term.
[[nodiscard]] glm::vec3 EvaluateShIrradiance(const Sh9& sh, const glm::vec3& normal) noexcept;

// Split-sum environment BRDF integration (Karis). Returns (scale, bias) for the
// F0 term, i.e. the value stored in a BRDF LUT texel at (NdotV, roughness).
[[nodiscard]] glm::vec2 IntegrateBrdf(float nDotV, float roughness, int sampleCount) noexcept;
}
