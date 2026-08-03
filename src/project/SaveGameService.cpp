#include "project/SaveGameService.hpp"

#include "editor/scene/SceneSerializer.hpp"
#include "engine/Uuid.hpp"
#include "engine/scene/IWorld.hpp"
#include "engine/scene/SceneDocument.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace fadix
{
SaveGameService::SaveGameService(std::filesystem::path saveDirectory)
    : m_SaveDirectory(std::move(saveDirectory))
{
}

bool SaveGameService::IsValidSlot(const std::string_view slot) noexcept
{
    if (slot.empty() || slot.size() > 64)
    {
        return false;
    }
    return std::all_of(slot.begin(), slot.end(), [](const unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c == '-';
    });
}

std::filesystem::path SaveGameService::SlotPath(const std::string_view slot) const
{
    return m_SaveDirectory / (std::string{slot} + ".fadixsave");
}

Result<std::filesystem::path> SaveGameService::Save(
    const std::string_view slot, const IWorld& world) const
{
    if (!IsValidSlot(slot))
    {
        return Result<std::filesystem::path>::Error(
            "Save slot must use 1-64 letters, numbers, underscores, or hyphens");
    }
    if (m_SaveDirectory.empty())
    {
        return Result<std::filesystem::path>::Error("Save directory is not configured");
    }
    // ponytail: this is deliberately a component/world snapshot. Script VM locals
    // need an explicit opt-in serialization contract before they can be persisted.
    const std::filesystem::path path = SlotPath(slot);
    SceneDocument document{Uuid::Generate(), std::string{slot}, path, true};
    SceneService scenes;
    if (const Result<void> saved = scenes.Save(document, world); !saved)
    {
        return Result<std::filesystem::path>::Error(saved.ErrorMessage());
    }
    return Result<std::filesystem::path>::Ok(path);
}

Result<void> SaveGameService::Load(const std::string_view slot, IWorld& world) const
{
    if (!IsValidSlot(slot))
    {
        return Result<void>::Error(
            "Save slot must use 1-64 letters, numbers, underscores, or hyphens");
    }
    const std::filesystem::path path = SlotPath(slot);
    SceneDocument document;
    SceneService scenes;
    return scenes.Load(document, world, path);
}
}
