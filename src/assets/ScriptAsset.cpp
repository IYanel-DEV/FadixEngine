#include "assets/ScriptAsset.hpp"

#include <algorithm>
#include <cctype>

namespace fadix
{
ScriptLanguage LanguageForExtension(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".lua")
    {
        return ScriptLanguage::Lua;
    }
    // .cpp and .hpp both map to native C++ scripting.
    return ScriptLanguage::Cpp;
}

const char* ExtensionForLanguage(ScriptLanguage language)
{
    switch (language)
    {
    case ScriptLanguage::Cpp: return ".cpp";
    case ScriptLanguage::Lua: break;
    }
    return ".lua";
}
}
