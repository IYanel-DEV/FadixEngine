#include "editor/scripting/FxsEditorSettings.hpp"

#include "project/ProjectJson.hpp"

#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#endif

namespace fadix::editor
{
std::filesystem::path FxsEditorSettings::ConfigPath()
{
#ifdef _WIN32
    PWSTR appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) &&
        appData != nullptr)
    {
        std::filesystem::path path =
            std::filesystem::path{appData} / "FadixEngine" / "fxs_editor.json";
        CoTaskMemFree(appData);
        return path;
    }
#endif
    return std::filesystem::path{"fxs_editor.json"};
}

FxsEditorSettings FxsEditorSettings::Load()
{
    FxsEditorSettings settings;
    const std::filesystem::path path = ConfigPath();
    std::ifstream in{path};
    if (!in)
    {
        return settings;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const auto parsed = project_json::Parse(buffer.str());
    if (!parsed || !parsed->IsObject())
    {
        return settings;
    }
    const project_json::Value& root = *parsed;
    if (root.Contains("zoom"))
    {
        settings.Zoom = static_cast<int>(root.at("zoom").AsNumber());
    }
    if (root.Contains("wordWrap"))
    {
        settings.WordWrap = root.at("wordWrap").AsBool();
    }
    if (root.Contains("viewWhitespace"))
    {
        settings.ViewWhitespace = root.at("viewWhitespace").AsBool();
    }
    if (root.Contains("tabWidth"))
    {
        settings.TabWidth = static_cast<int>(root.at("tabWidth").AsNumber());
        if (settings.TabWidth < 1)
        {
            settings.TabWidth = 4;
        }
    }
    if (root.Contains("useTabs"))
    {
        settings.UseTabs = root.at("useTabs").AsBool();
    }
    return settings;
}

void FxsEditorSettings::Save() const
{
    project_json::Value root = project_json::Value::MakeObject();
    root["zoom"] = project_json::Value::MakeNumber(static_cast<double>(Zoom));
    root["wordWrap"] = project_json::Value::MakeBool(WordWrap);
    root["viewWhitespace"] = project_json::Value::MakeBool(ViewWhitespace);
    root["tabWidth"] = project_json::Value::MakeNumber(static_cast<double>(TabWidth));
    root["useTabs"] = project_json::Value::MakeBool(UseTabs);

    const std::filesystem::path path = ConfigPath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out)
    {
        return;
    }
    out << project_json::Stringify(root, 2);
}
}
