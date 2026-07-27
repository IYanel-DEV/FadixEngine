#include "assets/ScriptDatabase.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <sstream>

namespace fadix
{
namespace
{
std::string TodayIso()
{
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    char buffer[16] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &parts);
    return buffer;
}

// Turn a script name into a valid C++ class identifier for the native template.
std::string ClassNameFrom(const std::string& name)
{
    std::string result;
    bool capitalize = true;
    for (const char c : name)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0)
        {
            result += capitalize ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
            capitalize = false;
        }
        else
        {
            capitalize = true; // collapse separators, capitalize the next word
        }
    }
    if (result.empty() || std::isalpha(static_cast<unsigned char>(result.front())) == 0)
    {
        result.insert(result.begin(), 'S'); // must start with a letter
    }
    return result;
}

std::string StarterTemplate(const std::string& name, ScriptLanguage language)
{
    const std::string date = TodayIso();
    std::ostringstream out;
    if (language == ScriptLanguage::Lua)
    {
        out << "-- FadixScript: " << name << "\n";
        out << "-- Created: " << date << "\n\n";
        out << "function OnStart(entity)\n";
        out << "    -- Called once when the script first runs\n";
        out << "end\n\n";
        out << "function OnUpdate(entity, deltaTime)\n";
        out << "    -- Called every frame while playing\n";
        out << "end\n\n";
        out << "function OnDestroy(entity)\n";
        out << "    -- Called when the entity is destroyed\n";
        out << "end\n";
    }
    else
    {
        const std::string cls = ClassNameFrom(name);
        out << "// FadixScript: " << name << "\n";
        out << "// Created: " << date << "\n";
        out << "#include <scripting/NativeScript.hpp>\n\n";
        out << "class " << cls << " : public fadix::NativeScript\n";
        out << "{\n";
        out << "public:\n";
        out << "    void OnStart(fadix::ScriptEntity& entity) override {}\n";
        out << "    void OnUpdate(fadix::ScriptEntity& entity, float dt) override {}\n";
        out << "    void OnDestroy(fadix::ScriptEntity& entity) override {}\n";
        out << "};\n\n";
        out << "extern \"C\" __declspec(dllexport) fadix::NativeScript* FadixCreateScript()\n";
        out << "{\n";
        out << "    return new " << cls << "();\n";
        out << "}\n";
    }
    return out.str();
}

bool IsScriptExtension(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".lua" || extension == ".cpp" || extension == ".hpp";
}
}

ScriptDatabase::ScriptDatabase() = default;
ScriptDatabase::~ScriptDatabase() = default;

void ScriptDatabase::ScanFolder(const std::filesystem::path& scriptsFolder)
{
    m_Folder = scriptsFolder;
    std::error_code error;
    std::filesystem::create_directories(m_Folder, error);

    m_Scripts.clear();
    m_NameIndex.clear();
    if (m_Folder.empty())
    {
        return;
    }
    for (std::filesystem::recursive_directory_iterator it{
             m_Folder, std::filesystem::directory_options::skip_permission_denied, error},
         end;
         it != end && !error;
         it.increment(error))
    {
        if (!it->is_regular_file(error) || error || !IsScriptExtension(it->path()))
        {
            continue;
        }
        const std::string name = it->path().stem().string();
        if (m_NameIndex.count(name) != 0)
        {
            continue; // first file wins if two share a stem
        }
        auto asset = std::make_unique<ScriptAsset>();
        asset->SourcePath = it->path();
        asset->Name = name;
        asset->Language = LanguageForExtension(it->path());
        m_NameIndex.emplace(name, m_Scripts.size());
        m_Scripts.push_back(std::move(asset));
    }
}

void ScriptDatabase::Refresh() { ScanFolder(m_Folder); }

void ScriptDatabase::Reindex()
{
    m_NameIndex.clear();
    for (std::size_t i = 0; i < m_Scripts.size(); ++i)
    {
        m_NameIndex[m_Scripts[i]->Name] = i;
    }
}

ScriptAsset* ScriptDatabase::Find(const std::string& name)
{
    const auto it = m_NameIndex.find(name);
    return it == m_NameIndex.end() ? nullptr : m_Scripts[it->second].get();
}

std::optional<std::reference_wrapper<ScriptAsset>> ScriptDatabase::Get(const std::string& name)
{
    if (ScriptAsset* asset = Find(name))
    {
        return std::ref(*asset);
    }
    return std::nullopt;
}

ScriptAsset* ScriptDatabase::CreateNew(const std::string& name,
    const ScriptLanguage language,
    const std::filesystem::path& destinationFolder)
{
    if (name.empty() || name == "." || name == ".." ||
        name.find_first_of("<>:\"/\\|?*") != std::string::npos || Find(name) != nullptr)
    {
        return nullptr;
    }
    std::error_code error;
    const std::filesystem::path folder =
        destinationFolder.empty() ? m_Folder : destinationFolder;
    const std::filesystem::path root = std::filesystem::weakly_canonical(m_Folder, error);
    if (error)
    {
        return nullptr;
    }
    const std::filesystem::path requestedFolder =
        (folder.is_absolute() ? folder : std::filesystem::absolute(folder, error)).lexically_normal();
    if (error)
    {
        return nullptr;
    }
    const std::filesystem::path requestedRelative = requestedFolder.lexically_relative(root);
    if (requestedFolder != root &&
        (requestedRelative.empty() || *requestedRelative.begin() == ".."))
    {
        return nullptr;
    }
    std::filesystem::create_directories(requestedFolder, error);
    if (error)
    {
        return nullptr;
    }
    const std::filesystem::path resolvedFolder =
        std::filesystem::weakly_canonical(requestedFolder, error);
    const std::filesystem::path resolvedRelative = resolvedFolder.lexically_relative(root);
    if (error || (resolvedFolder != root &&
        (resolvedRelative.empty() || *resolvedRelative.begin() == "..")))
    {
        return nullptr; // also rejects a destination reached through an escaping symlink
    }
    const std::filesystem::path path = resolvedFolder / (name + ExtensionForLanguage(language));
    if (std::filesystem::exists(path, error))
    {
        return nullptr; // never clobber an existing file
    }
    const std::string source = StarterTemplate(name, language);
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out)
    {
        return nullptr;
    }
    out.write(source.data(), static_cast<std::streamsize>(source.size()));
    out.close();
    if (!out)
    {
        return nullptr;
    }

    auto asset = std::make_unique<ScriptAsset>();
    asset->SourcePath = path;
    asset->Name = name;
    asset->Language = language;
    asset->SourceCode.assign(source.begin(), source.end());
    asset->Loaded = true;
    asset->Dirty = false;
    ScriptAsset* raw = asset.get();
    m_NameIndex.emplace(name, m_Scripts.size());
    m_Scripts.push_back(std::move(asset));
    return raw;
}

bool ScriptDatabase::Delete(const std::string& name)
{
    const auto it = m_NameIndex.find(name);
    if (it == m_NameIndex.end())
    {
        return false;
    }
    std::error_code error;
    std::filesystem::remove(m_Scripts[it->second]->SourcePath, error);
    m_Scripts.erase(m_Scripts.begin() + static_cast<std::ptrdiff_t>(it->second));
    Reindex();
    return true;
}

bool ScriptDatabase::Rename(const std::string& oldName, const std::string& newName)
{
    if (newName.empty() || Find(newName) != nullptr)
    {
        return false;
    }
    ScriptAsset* asset = Find(oldName);
    if (asset == nullptr)
    {
        return false;
    }
    const std::filesystem::path target =
        asset->SourcePath.parent_path() / (newName + asset->SourcePath.extension().string());
    std::error_code error;
    std::filesystem::rename(asset->SourcePath, target, error);
    if (error)
    {
        return false;
    }
    asset->SourcePath = target;
    asset->Name = newName;
    Reindex();
    return true;
}

bool ScriptDatabase::LoadSource(ScriptAsset& asset)
{
    std::ifstream in{asset.SourcePath, std::ios::binary};
    if (!in)
    {
        return false;
    }
    asset.SourceCode.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
    asset.Loaded = true;
    asset.Dirty = false;
    return true;
}

bool ScriptDatabase::Save(const std::string& name)
{
    ScriptAsset* asset = Find(name);
    if (asset == nullptr)
    {
        return false;
    }
    std::ofstream out{asset->SourcePath, std::ios::binary | std::ios::trunc};
    if (!out)
    {
        return false;
    }
    out.write(asset->SourceCode.data(), static_cast<std::streamsize>(asset->SourceCode.size()));
    out.close();
    if (!out)
    {
        return false;
    }
    asset->Dirty = false;
    return true;
}

bool ScriptDatabase::SaveAll()
{
    bool allSaved = true;
    for (auto& asset : m_Scripts)
    {
        if (asset->Dirty)
        {
            allSaved = Save(asset->Name) && allSaved;
        }
    }
    return allSaved;
}
}
