#pragma once

#include "assets/ScriptAsset.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fadix
{
// In-memory registry of the scripts under a project's Scripts/ folder. Owns the
// ScriptAsset entries and is the single writer to disk for script sources.
class ScriptDatabase
{
public:
    ScriptDatabase();
    ~ScriptDatabase();

    // Point the database at a project root and recursively scan its scripts.
    void ScanFolder(const std::filesystem::path& rootFolder);
    void Refresh();

    [[nodiscard]] std::optional<std::reference_wrapper<ScriptAsset>> Get(const std::string& name);

    // Create a new script file with a starter template and register it. Returns
    // nullptr if the name is empty or already exists.
    ScriptAsset* CreateNew(const std::string& name,
        ScriptLanguage language,
        const std::filesystem::path& destinationFolder = {});
    bool Delete(const std::string& name);
    bool Rename(const std::string& oldName, const std::string& newName);

    // Read the file into ScriptAsset::SourceCode (sets Loaded).
    bool LoadSource(ScriptAsset& asset);
    bool Save(const std::string& name);
    bool SaveAll();

    [[nodiscard]] const std::vector<std::unique_ptr<ScriptAsset>>& All() const { return m_Scripts; }
    [[nodiscard]] const std::filesystem::path& ScriptsFolder() const { return m_Folder; }

private:
    void Reindex();
    [[nodiscard]] ScriptAsset* Find(const std::string& name);

    std::filesystem::path m_Folder;
    std::vector<std::unique_ptr<ScriptAsset>> m_Scripts;
    std::unordered_map<std::string, std::size_t> m_NameIndex;
};
}
