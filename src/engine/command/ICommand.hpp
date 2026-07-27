#pragma once

#include <string_view>

namespace fadix
{
class ICommand
{
public:
    virtual ~ICommand() = default;
    virtual void Execute() = 0;
    virtual void Undo() = 0;
    [[nodiscard]] virtual std::string_view Name() const noexcept = 0;
};
}
