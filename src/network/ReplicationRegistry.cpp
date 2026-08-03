#include "network/ReplicationRegistry.hpp"

#include <algorithm>
#include <stdexcept>

namespace fadix
{
ReplicationRegistry::ReplicationRegistry()
{
    // Schema, not sockets: this keeps wire IDs stable; packets remain somebody else's problem.
    Register({"Transform", ReplicationReliability::UnreliableSequenced, 30.0F,
        {{"position", ReplicationCondition::Always, 0.001F},
         {"rotation", ReplicationCondition::Always, 0.0001F},
         {"scale", ReplicationCondition::InitialOnly, 0.001F}}});
}
void ReplicationRegistry::Register(ReplicatedComponentSchema schema)
{
    const auto duplicate = std::ranges::find_if(m_Schemas,
        [&schema](const ReplicatedComponentSchema& value) { return value.Name == schema.Name; });
    if (duplicate != m_Schemas.end())
    {
        throw std::invalid_argument("Duplicate replicated component: " + schema.Name);
    }

    schema.StableId = Hash(schema.Name, 14695981039346656037ULL);
    m_Schemas.push_back(std::move(schema));
    // Registration order must not fork the protocol. Multiplayer has enough ways to do that already.
    std::ranges::sort(m_Schemas, {}, &ReplicatedComponentSchema::StableId);
    RebuildProtocolHash();
}

const std::vector<ReplicatedComponentSchema>& ReplicationRegistry::Schemas() const noexcept
{
    return m_Schemas;
}

std::uint64_t ReplicationRegistry::ProtocolHash() const noexcept
{
    return m_ProtocolHash;
}

std::uint64_t ReplicationRegistry::Hash(std::string_view text, std::uint64_t seed)
{
    std::uint64_t hash = seed;
    for (const char character : text)
    {
        hash ^= static_cast<std::uint8_t>(character);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void ReplicationRegistry::RebuildProtocolHash()
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const ReplicatedComponentSchema& schema : m_Schemas)
    {
        hash = Hash(schema.Name, hash);
        for (const ReplicatedField& field : schema.Fields)
        {
            hash = Hash(field.Name, hash);
        }
    }
    m_ProtocolHash = hash;
}
}
