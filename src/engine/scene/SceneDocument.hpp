#pragma once

#include "engine/Uuid.hpp"

#include <filesystem>
#include <string>

namespace fadix
{
struct SceneDocument
{
    Uuid Id;
    std::string Name{"Untitled Scene"};
    std::filesystem::path Path;
    bool Dirty{false};
};
}
