#include "assets/AssetsModule.hpp"

#include "assets/AssetDatabase.hpp"
#include "editor/assets/AssetBrowserController.hpp"
#include "engine/assets/IAssetDatabase.hpp"
#include "engine/project/IProjectService.hpp"

#include <utility>

namespace fadix
{
AssetsModule::AssetsModule() = default;
AssetsModule::~AssetsModule() = default;
AssetsModule::AssetsModule(AssetsModule&&) noexcept = default;
AssetsModule& AssetsModule::operator=(AssetsModule&&) noexcept = default;

AssetsModule RegisterAssetsModule(const std::filesystem::path& projectRoot)
{
    AssetsModule module;
    module.Database = CreateAssetDatabase(projectRoot);
    module.Browser = std::make_unique<AssetBrowserController>(*module.Database);
    return module;
}

AssetsModule RegisterAssetsModule(IProjectService& projects)
{
    const std::filesystem::path projectRoot =
        projects.Active() ? projects.Active()->RootPath : std::filesystem::path{};
    AssetsModule module;
    module.Database = CreateAssetDatabase(projectRoot);
    module.Browser = std::make_unique<AssetBrowserController>(*module.Database, projects);
    return module;
}
}
