#pragma once

#include "engine/rhi/Types.hpp"

namespace fadix::rhi
{
class Texture
{
public:
    virtual ~Texture() = default;
    [[nodiscard]] virtual const TextureDesc& Description() const noexcept = 0;
};
}
