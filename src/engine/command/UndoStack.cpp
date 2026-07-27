#include "engine/command/UndoStack.hpp"

#include "engine/command/ICommand.hpp"

namespace fadix
{
void UndoStack::Push(std::unique_ptr<ICommand> command)
{
    if (!command)
    {
        return;
    }
    m_Commands.erase(m_Commands.begin() + static_cast<std::ptrdiff_t>(m_Cursor), m_Commands.end());
    command->Execute();
    m_Commands.push_back(std::move(command));
    m_Cursor = m_Commands.size();
}

void UndoStack::Undo()
{
    if (!CanUndo())
    {
        return;
    }
    m_Commands[--m_Cursor]->Undo();
}

void UndoStack::Redo()
{
    if (!CanRedo())
    {
        return;
    }
    m_Commands[m_Cursor++]->Execute();
}

bool UndoStack::CanUndo() const noexcept
{
    return m_Cursor > 0;
}

bool UndoStack::CanRedo() const noexcept
{
    return m_Cursor < m_Commands.size();
}

void UndoStack::Clear() noexcept
{
    m_Commands.clear();
    m_Cursor = 0;
}
}
