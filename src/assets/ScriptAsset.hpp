#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fadix
{
enum class ScriptLanguage
{
    Lua,
    Cpp
};

// One script file on disk. SourceCode is loaded on demand and holds the edited
// text while the FadixScript editor has the file open.
struct ScriptAsset
{
    std::filesystem::path SourcePath; // e.g. "Scripts/player.lua"
    std::string Name;                 // display name, the file stem ("player")
    ScriptLanguage Language{ScriptLanguage::Lua};
    std::vector<char> SourceCode; // full source text, loaded on demand
    bool Loaded{false};           // SourceCode has been read from disk
    bool Dirty{false};            // unsaved edits in the editor
};

[[nodiscard]] ScriptLanguage LanguageForExtension(const std::filesystem::path& path);
[[nodiscard]] const char* ExtensionForLanguage(ScriptLanguage language);
}
