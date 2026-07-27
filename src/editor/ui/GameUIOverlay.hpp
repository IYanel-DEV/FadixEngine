#pragma once

#include "engine/assets/AssetHandle.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace Rml
{
class Context;
class ElementDocument;
}

namespace fadix
{
class IAssetDatabase;
class IWorld;

class GameUIOverlay
{
public:
    GameUIOverlay() = default;
    ~GameUIOverlay();

    GameUIOverlay(const GameUIOverlay&) = delete;
    GameUIOverlay& operator=(const GameUIOverlay&) = delete;

    bool Initialize(int width, int height, float density);
    void Shutdown();

    void SetActive(bool active);
    void Clear();

    void Sync(
        const IWorld& world,
        const IAssetDatabase& assets,
        int contextWidth,
        int contextHeight,
        float density,
        float viewportX,
        float viewportY,
        float viewportW,
        float viewportH);

    void Update();
    void Render();

private:
    struct LoadedDoc
    {
        AssetHandle Handle{};
        int Order{0};
        float Scale{1.0F};
        std::filesystem::path Path{};
        Rml::ElementDocument* Document{nullptr};
    };

    void Reload(
        const IWorld& world,
        const IAssetDatabase& assets,
        float viewportX,
        float viewportY,
        float viewportW,
        float viewportH);

    void LayoutDocuments(float viewportX, float viewportY, float viewportW, float viewportH);

    Rml::Context* m_Context{nullptr};
    std::vector<LoadedDoc> m_Docs;
    std::string m_Signature;
    bool m_Active{false};
};
}
