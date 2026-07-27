#pragma once

#include "engine/rhi/Types.hpp"

namespace fadix::rhi
{
class Texture;

class RenderTarget
{
public:
    virtual ~RenderTarget() = default;
    [[nodiscard]] virtual Texture* ColorTexture() const noexcept = 0;
    [[nodiscard]] virtual Texture* ColorTexture1() const noexcept { return nullptr; }
    [[nodiscard]] virtual Texture* DepthTexture() const noexcept = 0;
    [[nodiscard]] virtual Extent2D Extent() const noexcept = 0;
};
}
