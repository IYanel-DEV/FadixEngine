#pragma once

#include "engine/Result.hpp"
#include "engine/scene/SceneDocument.hpp"

namespace fadix
{
class IWorld;

class ISceneSerializer
{
public:
    virtual ~ISceneSerializer() = default;
    [[nodiscard]] virtual Result<void> Save(const SceneDocument& document, const IWorld& world) = 0;
    [[nodiscard]] virtual Result<void> Load(const SceneDocument& document, IWorld& world) = 0;
};
}
