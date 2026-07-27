#pragma once

#include <cstdint>

struct SDL_GPUTexture;

namespace fadix::editor
{
/// Offscreen color target description for viewport / material preview sampling.
struct SceneViewSource
{
    SDL_GPUTexture* Texture{nullptr};
    std::uint32_t Width{0};
    std::uint32_t Height{0};
};
}
