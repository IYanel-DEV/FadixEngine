#include "engine/Uuid.hpp"

#include <charconv>
#include <random>

namespace
{
constexpr char HexDigits[] = "0123456789abcdef";
}

namespace fadix
{
Uuid Uuid::Generate()
{
    thread_local std::mt19937_64 generator{std::random_device{}()};
    Storage bytes{};
    for (std::size_t offset = 0; offset < bytes.size(); offset += sizeof(std::uint64_t))
    {
        const std::uint64_t value = generator();
        for (std::size_t index = 0; index < sizeof(value); ++index)
        {
            bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
        }
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
    return Uuid{bytes};
}

std::optional<Uuid> Uuid::Parse(const std::string_view text) noexcept
{
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-')
    {
        return std::nullopt;
    }

    Storage bytes{};
    std::size_t source = 0;
    for (std::uint8_t& byte : bytes)
    {
        if (source == 8 || source == 13 || source == 18 || source == 23)
        {
            ++source;
        }
        unsigned int value = 0;
        const char* first = text.data() + source;
        const auto result = std::from_chars(first, first + 2, value, 16);
        if (result.ec != std::errc{} || result.ptr != first + 2)
        {
            return std::nullopt;
        }
        byte = static_cast<std::uint8_t>(value);
        source += 2;
    }
    return Uuid{bytes};
}

std::string Uuid::ToString() const
{
    std::string result(36, '-');
    std::size_t destination = 0;
    for (const std::uint8_t byte : m_Bytes)
    {
        if (destination == 8 || destination == 13 || destination == 18 || destination == 23)
        {
            ++destination;
        }
        result[destination++] = HexDigits[byte >> 4U];
        result[destination++] = HexDigits[byte & 0x0FU];
    }
    return result;
}
}

std::size_t std::hash<fadix::Uuid>::operator()(const fadix::Uuid& value) const noexcept
{
    std::size_t hash = 1469598103934665603ULL;
    for (const std::uint8_t byte : value.Bytes())
    {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}
