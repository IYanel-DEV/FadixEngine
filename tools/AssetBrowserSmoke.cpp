#include "assets/AssetDatabase.hpp"
#include "editor/assets/AssetBrowserController.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <utility>

namespace
{
int failures = 0;

void Check(const bool condition, const std::string_view message)
{
    std::cout << (condition ? "  ok   " : "  FAIL ") << message << '\n';
    failures += condition ? 0 : 1;
}

bool HasEntry(const fadix::AssetBrowserController& browser,
    const std::string_view name,
    const std::string_view type = {})
{
    for (const fadix::AssetBrowserEntry& entry : browser.Entries())
    {
        if (entry.DisplayName == name && (type.empty() || entry.AssetType == type))
        {
            return true;
        }
    }
    return false;
}
}

int main()
{
    using namespace fadix;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "fadix-asset-browser-smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "Assets", error);
    std::filesystem::create_directories(root / "Scenes", error);
    {
        std::ofstream scene{root / "Scenes" / "Main.scene"};
        scene << "FADIX_SCENE 1\n";
        std::filesystem::create_directories(root / "Assets" / "Animations", error);
        std::ofstream animation{root / "Assets" / "Animations" / "Idle.fdxanim"};
        animation << "fdxanim 1\nname Idle\nduration 1\n";
        std::ofstream controller{root / "Assets" / "Animations" / "Hero.fdxcontroller"};
        controller << "fdxcontroller 1 \"Hero\" \"Idle\" 0 0 0\n";
        std::ofstream external{root.parent_path() / "fadix-external-model.glb", std::ios::binary};
        external << "glTF";
    }

    AssetDatabase database{root};
    AssetBrowserController browser{database};
    Check(HasEntry(browser, "Project Scenes"), "Project Scenes is visible from Assets root");
    Check(browser.OpenFolder(root / "Assets" / "Animations").IsOk(),
        "Animation asset folder opens in Content Browser");
    Check(HasEntry(browser, "Idle.fdxanim", "Animation"),
        "FDX animation is indexed as an animation asset");
    Check(HasEntry(browser, "Hero.fdxcontroller", "AnimatorController"),
        "FDX controller is indexed as an Animator Controller asset");
    Check(browser.OpenFolder(root / "Assets").IsOk(), "Assets root reopens");

    Check(browser.OpenFolder(root / "Scenes").IsOk(), "Project Scenes opens in Content Browser");
    Check(HasEntry(browser, "Main.scene", "Scene"), "Scene file is indexed as a scene asset");

    const std::filesystem::path external = root.parent_path() / "fadix-external-model.glb";
    const Result<std::filesystem::path> imported = browser.ImportExternalModel(external);
    Check(imported.IsOk() && std::filesystem::is_regular_file(imported ? imported.Value() : "", error),
        "External GLB drop copies into Assets");
    Check(HasEntry(browser, "fadix-external-model.glb", "Mesh"),
        "Imported model appears in Content Browser");
    bool meshActivated = false;
    AssetBrowserCallbacks callbacks;
    callbacks.CreateMeshEntity = [&meshActivated](const AssetHandle&, const std::filesystem::path&) {
        meshActivated = true;
    };
    browser.SetCallbacks(std::move(callbacks));
    for (const AssetBrowserEntry& entry : browser.Entries())
    {
        if (entry.AssetType == "Mesh")
        {
            browser.Activate(entry);
            break;
        }
    }
    Check(meshActivated, "Opening a model creates its scene entity");

    std::filesystem::remove(external, error);
    std::filesystem::remove_all(root, error);
    return failures == 0 ? 0 : 1;
}
