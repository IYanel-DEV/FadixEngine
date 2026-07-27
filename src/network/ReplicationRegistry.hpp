#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fadix
{
enum class ReplicationCondition : std::uint8_t
{
    Always,
    OwnerOnly,
    SkipOwner,
    InitialOnly
};

enum class ReplicationReliability : std::uint8_t
{
    UnreliableSequenced,
    ReliableOrdered
};

struct ReplicatedField
{
    std::string Name;
    ReplicationCondition Condition{ReplicationCondition::Always};
    float QuantizationStep{0.001F};
};

struct ReplicatedComponentSchema
{
    std::string Name;
    ReplicationReliability Reliability{ReplicationReliability::UnreliableSequenced};
    float UpdatesPerSecond{20.0F};
    std::vector<ReplicatedField> Fields;
    std::uint64_t StableId{0};
};

class ReplicationRegistry final
{
public:
    ReplicationRegistry();

    void Register(ReplicatedComponentSchema schema);
    [[nodiscard]] const std::vector<ReplicatedComponentSchema>& Schemas() const noexcept;
    [[nodiscard]] std::uint64_t ProtocolHash() const noexcept;

private:
    static std::uint64_t Hash(std::string_view text, std::uint64_t seed);
    void RebuildProtocolHash();

    std::vector<ReplicatedComponentSchema> m_Schemas;
    std::uint64_t m_ProtocolHash{0};
};
}
