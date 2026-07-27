#pragma once

#include <filesystem>

namespace fadix::editor
{
struct FxsEditorSettings
{
    int Zoom{0};
    bool WordWrap{false};
    bool ViewWhitespace{false};
    int TabWidth{4};
    bool UseTabs{false};

    static std::filesystem::path ConfigPath();
    static FxsEditorSettings Load();
    void Save() const;
};
}
