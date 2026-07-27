#pragma once

#include "engine/Result.hpp"
#include "engine/rhi/Types.hpp"

namespace fadix::rhi
{
class RenderTarget;

class Swapchain
{
public:
    virtual ~Swapchain() = default;
    [[nodiscard]] virtual Result<void> Resize(Extent2D extent) = 0;
    [[nodiscard]] virtual RenderTarget* AcquireNextTarget() = 0;
    [[nodiscard]] virtual Result<void> Present() = 0;
};
}
