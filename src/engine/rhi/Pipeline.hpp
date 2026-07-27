#pragma once

#include "engine/rhi/Types.hpp"

namespace fadix::rhi
{
class Pipeline
{
public:
    virtual ~Pipeline() = default;
    [[nodiscard]] virtual const PipelineDesc& Description() const noexcept = 0;
};
}
