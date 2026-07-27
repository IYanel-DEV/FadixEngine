#pragma once

#include "engine/command/ICommand.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace fadix
{
class UndoStack final
{
public:
    void Push(std::unique_ptr<ICommand> command);
    void Undo();
    void Redo();
    [[nodiscard]] bool CanUndo() const noexcept;
    [[nodiscard]] bool CanRedo() const noexcept;
    void Clear() noexcept;

private:
    std::vector<std::unique_ptr<ICommand>> m_Commands;
    std::size_t m_Cursor{0};
};
}
