#pragma once

#include <cstddef>
#include <span>

namespace fadix::rhi
{
class Buffer
{
public:
    virtual ~Buffer() = default;
    [[nodiscard]] virtual std::size_t Size() const noexcept = 0;
    virtual void Upload(std::span<const std::byte> data, std::size_t offset = 0) = 0;
};
}
