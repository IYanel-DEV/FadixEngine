#pragma once

#include "engine/Uuid.hpp"

#include <cstdint>
#include <optional>

namespace fadix
{
struct PickRequest
{
    std::uint32_t X{0};
    std::uint32_t Y{0};
};

struct PickResult
{
    Uuid Entity;
    float Depth{1.0F};
};

class Picking
{
public:
    virtual ~Picking() = default;
    virtual void Request(PickRequest request) = 0;
    [[nodiscard]] virtual std::optional<PickResult> Poll() = 0;
};
}
