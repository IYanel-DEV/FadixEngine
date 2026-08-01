#pragma once

#include <cstdint>

namespace fadix::editor
{
/// Offscreen color target description for viewport / material preview sampling.
struct SceneViewSource
{
    void* Texture{nullptr};
    std::uint32_t Width{0};
    std::uint32_t Height{0};
};
}
