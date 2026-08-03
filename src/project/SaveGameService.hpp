#pragma once

#include "engine/Result.hpp"

#include <filesystem>
#include <string_view>

namespace fadix
{
class IWorld;

class SaveGameService final
{
public:
    explicit SaveGameService(std::filesystem::path saveDirectory);

    [[nodiscard]] static bool IsValidSlot(std::string_view slot) noexcept;
    [[nodiscard]] Result<std::filesystem::path> Save(
        std::string_view slot, const IWorld& world) const;
    [[nodiscard]] Result<void> Load(std::string_view slot, IWorld& world) const;

private:
    [[nodiscard]] std::filesystem::path SlotPath(std::string_view slot) const;

    std::filesystem::path m_SaveDirectory;
};
}
