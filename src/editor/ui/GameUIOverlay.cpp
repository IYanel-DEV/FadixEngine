#include "editor/ui/GameUIOverlay.hpp"

#include "engine/assets/IAssetDatabase.hpp"
#include "engine/scene/IWorld.hpp"
#include "runtime/Components.hpp"

#include <RmlUi/Core.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace fadix
{
namespace
{
[[nodiscard]] std::string BuildSignature(const IWorld& world, const IAssetDatabase& assets)
{
    std::ostringstream out;
    std::vector<std::tuple<int, std::string, float, std::string>> rows;
    for (const auto [entity, ui] : world.Registry().view<const UICanvasComponent>().each())
    {
        static_cast<void>(entity);
        if (!ui.RenderInGame || !ui.UIAsset.IsValid())
        {
            continue;
        }
        const AssetMetadata* meta = assets.Meta(ui.UIAsset);
        if (meta == nullptr || meta->SourcePath.empty())
        {
            continue;
        }
        rows.emplace_back(
            ui.Order, ui.UIAsset.ToString(), ui.Scale, meta->SourcePath.generic_string());
    }
    std::sort(rows.begin(), rows.end());
    for (const auto& [order, handle, scale, path] : rows)
    {
        out << order << '|' << handle << '|' << scale << '|' << path << ';';
    }
    return out.str();
}
}

GameUIOverlay::~GameUIOverlay()
{
    Shutdown();
}

bool GameUIOverlay::Initialize(const int width, const int height, const float density)
{
    if (m_Context != nullptr)
    {
        return true;
    }
    m_Context = Rml::CreateContext("game-ui", Rml::Vector2i{std::max(width, 1), std::max(height, 1)});
    if (m_Context == nullptr)
    {
        return false;
    }
    m_Context->SetDensityIndependentPixelRatio(density > 0.0F ? density : 1.0F);
    return true;
}

void GameUIOverlay::Shutdown()
{
    Clear();
    if (m_Context != nullptr)
    {
        Rml::RemoveContext("game-ui");
        m_Context = nullptr;
    }
    m_Active = false;
}

void GameUIOverlay::SetActive(const bool active)
{
    if (!active)
    {
        Clear();
    }
    m_Active = active;
}

void GameUIOverlay::Clear()
{
    if (m_Context != nullptr)
    {
        while (Rml::ElementDocument* doc = m_Context->GetDocument(0))
        {
            doc->Close();
        }
    }
    m_Docs.clear();
    m_Signature.clear();
}

void GameUIOverlay::Reload(
    const IWorld& world,
    const IAssetDatabase& assets,
    const float viewportX,
    const float viewportY,
    const float viewportW,
    const float viewportH)
{
    Clear();
    if (m_Context == nullptr)
    {
        return;
    }

    std::vector<const UICanvasComponent*> canvases;
    for (const auto [entity, ui] : world.Registry().view<const UICanvasComponent>().each())
    {
        static_cast<void>(entity);
        if (ui.RenderInGame && ui.UIAsset.IsValid())
        {
            canvases.push_back(&ui);
        }
    }
    std::sort(canvases.begin(), canvases.end(), [](const UICanvasComponent* a, const UICanvasComponent* b) {
        return a->Order < b->Order;
    });

    for (const UICanvasComponent* ui : canvases)
    {
        const AssetMetadata* meta = assets.Meta(ui->UIAsset);
        if (meta == nullptr || meta->SourcePath.empty())
        {
            continue;
        }
        Rml::ElementDocument* document = m_Context->LoadDocument(meta->SourcePath.string());
        if (document == nullptr)
        {
            continue;
        }
        document->Show();
        LoadedDoc loaded;
        loaded.Handle = ui->UIAsset;
        loaded.Order = ui->Order;
        loaded.Scale = ui->Scale > 0.0F ? ui->Scale : 1.0F;
        loaded.Path = meta->SourcePath;
        loaded.Document = document;
        m_Docs.push_back(loaded);
    }
    LayoutDocuments(viewportX, viewportY, viewportW, viewportH);
}

void GameUIOverlay::LayoutDocuments(
    const float viewportX,
    const float viewportY,
    const float viewportW,
    const float viewportH)
{
    for (LoadedDoc& loaded : m_Docs)
    {
        if (loaded.Document == nullptr)
        {
            continue;
        }
        const float scale = loaded.Scale > 0.0F ? loaded.Scale : 1.0F;
        const float width = std::max(1.0F, viewportW);
        const float height = std::max(1.0F, viewportH);
        loaded.Document->SetProperty("position", "absolute");
        loaded.Document->SetProperty("left", std::to_string(viewportX) + "px");
        loaded.Document->SetProperty("top", std::to_string(viewportY) + "px");
        loaded.Document->SetProperty("width", std::to_string(width) + "px");
        loaded.Document->SetProperty("height", std::to_string(height) + "px");
        loaded.Document->SetProperty("overflow", "hidden");
        loaded.Document->SetProperty("pointer-events", "none");
        if (std::fabs(scale - 1.0F) > 0.001F)
        {
            loaded.Document->SetProperty("transform", "scale(" + std::to_string(scale) + ")");
            loaded.Document->SetProperty("transform-origin", "top left");
        }
        else
        {
            loaded.Document->RemoveProperty("transform");
            loaded.Document->RemoveProperty("transform-origin");
        }
    }
}

void GameUIOverlay::Sync(
    const IWorld& world,
    const IAssetDatabase& assets,
    const int contextWidth,
    const int contextHeight,
    const float density,
    const float viewportX,
    const float viewportY,
    const float viewportW,
    const float viewportH)
{
    if (!m_Active || m_Context == nullptr)
    {
        return;
    }
    m_Context->SetDimensions(Rml::Vector2i{std::max(contextWidth, 1), std::max(contextHeight, 1)});
    if (density > 0.0F)
    {
        m_Context->SetDensityIndependentPixelRatio(density);
    }

    const std::string signature = BuildSignature(world, assets);
    if (signature != m_Signature)
    {
        m_Signature = signature;
        Reload(world, assets, viewportX, viewportY, viewportW, viewportH);
        return;
    }
    LayoutDocuments(viewportX, viewportY, viewportW, viewportH);
}

void GameUIOverlay::Update()
{
    if (m_Active && m_Context != nullptr)
    {
        m_Context->Update();
    }
}

void GameUIOverlay::Render()
{
    if (m_Active && m_Context != nullptr && !m_Docs.empty())
    {
        m_Context->Render();
    }
}
}
