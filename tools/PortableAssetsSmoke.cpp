#include "assets/EmbeddedAssetProvider.hpp"

#include <array>
#include <filesystem>
#include <iostream>
#include <string_view>

int main()
{
    std::clog << std::unitbuf << "[portable-assets] materializing\n";
    constexpr std::array<std::string_view, 4> required{
        "editor/icons/fadix-logo.png",
        "fonts/FontAwesome7Free-Solid-900.otf",
        "shaders/viewport.hlsl",
        "templates/empty_3d/template.json"};

    const std::filesystem::path& root = fadix::RuntimeAssetRoot();
    std::clog << "[portable-assets] checking " << root.string() << '\n';
    for (const std::string_view relative : required)
    {
        std::error_code error;
        const std::filesystem::path path = root / relative;
        if (!std::filesystem::is_regular_file(path, error) || error)
        {
            std::cerr << "[portable-assets] missing " << path.string() << '\n';
            return 1;
        }
        const auto embedded = fadix::FindEmbeddedAsset(relative);
        if (!embedded || std::filesystem::file_size(path, error) != embedded->Bytes.size() || error)
        {
            std::cerr << "[portable-assets] invalid " << path.string() << '\n';
            return 2;
        }
    }

    std::cout << "[portable-assets] PASS " << root.string() << '\n';
    return 0;
}
