#pragma once

#include "engine/rhi/Types.hpp"

namespace fadix::rhi
{
class Shader
{
public:
    virtual ~Shader() = default;
    [[nodiscard]] virtual const ShaderDesc& Description() const noexcept = 0;
};
}
