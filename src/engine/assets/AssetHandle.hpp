#pragma once

#include "engine/Uuid.hpp"

namespace fadix
{
using AssetHandle = Uuid;
inline constexpr AssetHandle InvalidAssetHandle{};
}
