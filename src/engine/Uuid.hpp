#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace fadix
{
class Uuid final
{
public:
    using Storage = std::array<std::uint8_t, 16>;

    constexpr Uuid() noexcept = default;
    explicit constexpr Uuid(Storage bytes) noexcept : m_Bytes(bytes) {}

    [[nodiscard]] static Uuid Generate();
    [[nodiscard]] static std::optional<Uuid> Parse(std::string_view text) noexcept;
    [[nodiscard]] std::string ToString() const;
    [[nodiscard]] constexpr const Storage& Bytes() const noexcept { return m_Bytes; }
    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        for (const std::uint8_t byte : m_Bytes)
        {
            if (byte != 0)
            {
                return true;
            }
        }
        return false;
    }

    friend constexpr bool operator==(const Uuid&, const Uuid&) noexcept = default;

private:
    Storage m_Bytes{};
};
}

template <>
struct std::hash<fadix::Uuid>
{
    std::size_t operator()(const fadix::Uuid& value) const noexcept;
};
