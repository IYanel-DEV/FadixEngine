#include "editor/imgui/panels/TilemapPanel.hpp"

#include "editor/command/EntityCommands.hpp"
#include "engine/scene/IWorld.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace fadix::editor
{
namespace
{
constexpr int kFloodFillLimit = 100000;

const char* ToolLabel(TilemapTool t)
{
    switch (t)
    {
    case TilemapTool::Pencil: return "Pencil";
    case TilemapTool::Eraser: return "Eraser";
    case TilemapTool::Rectangle: return "Rectangle";
    case TilemapTool::RectErase: return "Rect Erase";
    case TilemapTool::Fill: return "Fill";
    case TilemapTool::Eyedropper: return "Eyedrop";
    case TilemapTool::Select: return "Select";
    }
    return "Pencil";
}
}

glm::ivec2 TilemapPanel::WorldToTile(const glm::vec2 worldPos, const TileMapComponent& tm)
{
    const float tileWorldW = static_cast<float>(tm.TileWidth) / std::max(tm.PixelsPerUnit, 0.001F);
    const float tileWorldH = static_cast<float>(tm.TileHeight) / std::max(tm.PixelsPerUnit, 0.001F);
    return {static_cast<int>(std::floor(worldPos.x / tileWorldW)),
        static_cast<int>(std::floor(worldPos.y / tileWorldH))};
}

bool TilemapPanel::TileInBounds(const glm::ivec2 coord, const TileMapComponent& tm)
{
    return coord.x >= 0 && coord.x < tm.GridWidth && coord.y >= 0 && coord.y < tm.GridHeight;
}

int& TilemapPanel::TileAt(TileMapComponent& tm, const int x, const int y) const
{
    const int layer = std::clamp(m_ActiveLayer, 0, tm.LayerCount - 1);
    return tm.TileData[static_cast<std::size_t>(layer) *
            static_cast<std::size_t>(tm.GridWidth) * static_cast<std::size_t>(tm.GridHeight) +
        static_cast<std::size_t>(y) * static_cast<std::size_t>(tm.GridWidth) +
        static_cast<std::size_t>(x)];
}

void TilemapPanel::PaintTile(TileMapComponent& tm, const int x, const int y, const int tileId)
{
    if (TileInBounds({x, y}, tm))
    {
        TileAt(tm, x, y) = tileId;
    }
}

void TilemapPanel::PaintRect(
    TileMapComponent& tm, const glm::ivec2 a, const glm::ivec2 b, const int tileId)
{
    const int x0 = std::clamp(std::min(a.x, b.x), 0, tm.GridWidth - 1);
    const int y0 = std::clamp(std::min(a.y, b.y), 0, tm.GridHeight - 1);
    const int x1 = std::clamp(std::max(a.x, b.x), 0, tm.GridWidth - 1);
    const int y1 = std::clamp(std::max(a.y, b.y), 0, tm.GridHeight - 1);
    for (int y = y0; y <= y1; ++y)
    {
        for (int x = x0; x <= x1; ++x)
        {
            TileAt(tm, x, y) = tileId;
        }
    }
}

void TilemapPanel::FloodFill(
    TileMapComponent& tm, const int startX, const int startY, const int newTile, const int limit)
{
    if (!TileInBounds({startX, startY}, tm))
    {
        return;
    }
    const int oldTile = TileAt(tm, startX, startY);
    if (oldTile == newTile)
    {
        return;
    }
    std::vector<glm::ivec2> stack;
    stack.push_back({startX, startY});
    int iterations = 0;
    while (!stack.empty() && iterations < limit)
    {
        const glm::ivec2 coord = stack.back();
        stack.pop_back();
        if (!TileInBounds(coord, tm) || TileAt(tm, coord.x, coord.y) != oldTile)
        {
            continue;
        }
        TileAt(tm, coord.x, coord.y) = newTile;
        ++iterations;
        stack.push_back({coord.x + 1, coord.y});
        stack.push_back({coord.x - 1, coord.y});
        stack.push_back({coord.x, coord.y + 1});
        stack.push_back({coord.x, coord.y - 1});
    }
}

bool TilemapPanel::HandleViewportMouseDown(
    const glm::vec2 worldPos, SceneEditor& scene, IWorld& world)
{
    const auto sel = scene.Selection();
    if (!sel)
    {
        return false;
    }
    const auto entity = world.Find(*sel);
    if (!entity)
    {
        return false;
    }
    TileMapComponent* tm = world.Registry().try_get<TileMapComponent>(*entity);
    if (tm == nullptr || m_ActiveLayer >= tm->LayerCount)
    {
        return false;
    }
    const glm::ivec2 coord = WorldToTile(worldPos, *tm);
    m_HoveredTile = coord;
    m_HasHoveredTile = true;

    if (m_Tool == TilemapTool::Fill)
    {
        if (!m_InStroke)
        {
            m_StrokeSnapshot = *tm;
            m_InStroke = true;
        }
        if (TileInBounds(coord, *tm))
        {
            FloodFill(*tm, coord.x, coord.y,
                (m_Tool == TilemapTool::Fill ? m_SelectedTileId : -1), kFloodFillLimit);
        }
        return true;
    }

    if (m_Tool == TilemapTool::Eyedropper)
    {
        if (TileInBounds(coord, *tm))
        {
            const int picked = TileAt(*tm, coord.x, coord.y);
            if (picked >= 0)
            {
                m_SelectedTileId = picked;
            }
        }
        return true;
    }

    if (!m_InStroke)
    {
        m_StrokeSnapshot = *tm;
        m_InStroke = true;
        m_RectStart = coord;
    }

    if (m_Tool == TilemapTool::Pencil)
    {
        PaintTile(*tm, coord.x, coord.y, m_SelectedTileId);
    }
    else if (m_Tool == TilemapTool::Eraser)
    {
        PaintTile(*tm, coord.x, coord.y, -1);
    }
    return true;
}

bool TilemapPanel::HandleViewportMouseMove(
    const glm::vec2 worldPos, SceneEditor& scene, IWorld& world)
{
    const auto sel = scene.Selection();
    if (!sel)
    {
        m_HasHoveredTile = false;
        return false;
    }
    const auto entity = world.Find(*sel);
    if (!entity)
    {
        m_HasHoveredTile = false;
        return false;
    }
    TileMapComponent* tm = world.Registry().try_get<TileMapComponent>(*entity);
    if (tm == nullptr)
    {
        m_HasHoveredTile = false;
        return false;
    }
    const glm::ivec2 coord = WorldToTile(worldPos, *tm);
    m_HoveredTile = coord;
    m_HasHoveredTile = true;

    if (!m_InStroke)
    {
        return false;
    }
    if (m_Tool == TilemapTool::Pencil)
    {
        PaintTile(*tm, coord.x, coord.y, m_SelectedTileId);
    }
    else if (m_Tool == TilemapTool::Eraser)
    {
        PaintTile(*tm, coord.x, coord.y, -1);
    }
    // Rectangle/RectErase preview restores and repaints each frame
    else if (m_Tool == TilemapTool::Rectangle && m_StrokeSnapshot)
    {
        const std::size_t layerOffset = static_cast<std::size_t>(
            std::clamp(m_ActiveLayer, 0, m_StrokeSnapshot->LayerCount - 1)) *
            static_cast<std::size_t>(tm->GridWidth) *
            static_cast<std::size_t>(tm->GridHeight);
        const std::size_t layerSize = static_cast<std::size_t>(tm->GridWidth) *
            static_cast<std::size_t>(tm->GridHeight);
        // Restore from snapshot for live preview
        std::copy(
            m_StrokeSnapshot->TileData.begin() + static_cast<std::ptrdiff_t>(layerOffset),
            m_StrokeSnapshot->TileData.begin() + static_cast<std::ptrdiff_t>(layerOffset + layerSize),
            tm->TileData.begin() + static_cast<std::ptrdiff_t>(layerOffset));
        PaintRect(*tm, m_RectStart, coord, m_SelectedTileId);
    }
    else if (m_Tool == TilemapTool::RectErase && m_StrokeSnapshot)
    {
        const std::size_t layerOffset = static_cast<std::size_t>(
            std::clamp(m_ActiveLayer, 0, m_StrokeSnapshot->LayerCount - 1)) *
            static_cast<std::size_t>(tm->GridWidth) *
            static_cast<std::size_t>(tm->GridHeight);
        const std::size_t layerSize = static_cast<std::size_t>(tm->GridWidth) *
            static_cast<std::size_t>(tm->GridHeight);
        std::copy(
            m_StrokeSnapshot->TileData.begin() + static_cast<std::ptrdiff_t>(layerOffset),
            m_StrokeSnapshot->TileData.begin() + static_cast<std::ptrdiff_t>(layerOffset + layerSize),
            tm->TileData.begin() + static_cast<std::ptrdiff_t>(layerOffset));
        PaintRect(*tm, m_RectStart, coord, -1);
    }
    return true;
}

bool TilemapPanel::HandleViewportMouseUp(
    SceneEditor& scene, IWorld& world, UndoStack& history)
{
    if (!m_InStroke || !m_StrokeSnapshot)
    {
        m_InStroke = false;
        return false;
    }
    const auto sel = scene.Selection();
    if (!sel)
    {
        m_InStroke = false;
        m_StrokeSnapshot.reset();
        return false;
    }
    const auto entity = world.Find(*sel);
    if (!entity)
    {
        m_InStroke = false;
        m_StrokeSnapshot.reset();
        return false;
    }
    TileMapComponent* tm = world.Registry().try_get<TileMapComponent>(*entity);
    if (tm == nullptr)
    {
        m_InStroke = false;
        m_StrokeSnapshot.reset();
        return false;
    }

    // Build before/after snapshots and push undo command
    const EntitySnapshot before = [&] {
        EntitySnapshot snap;
        snap.Id = *sel;
        snap.TileMap = m_StrokeSnapshot;
        return snap;
    }();
    EntitySnapshot after;
    after.Id = *sel;
    after.TileMap = *tm;

    const auto fullBefore = EntitySnapshot::Capture(world, *sel);
    const auto fullAfter = EntitySnapshot::Capture(world, *sel);
    if (fullBefore && fullAfter)
    {
        EntitySnapshot realBefore = *fullBefore;
        realBefore.TileMap = m_StrokeSnapshot;
        history.Push(std::make_unique<SnapshotEntityCommand>(world, realBefore, *fullAfter, "Paint Tiles"));
    }

    m_InStroke = false;
    m_StrokeSnapshot.reset();
    return true;
}

void TilemapPanel::CancelStroke()
{
    m_InStroke = false;
    m_StrokeSnapshot.reset();
}

void TilemapPanel::Draw(
    SceneEditor& scene, EditorUiState& ui, IWorld& world, UndoStack& history)
{
    static_cast<void>(ui);
    const auto sel = scene.Selection();
    m_HasSelection = false;
    m_SelectedEntity = std::nullopt;

    if (!sel)
    {
        return;
    }
    const auto entity = world.Find(*sel);
    if (!entity)
    {
        return;
    }
    TileMapComponent* tm = world.Registry().try_get<TileMapComponent>(*entity);
    if (tm == nullptr)
    {
        return;
    }
    m_HasSelection = true;
    m_SelectedEntity = *sel;

    if (!ImGui::Begin("Tilemap Editor"))
    {
        ImGui::End();
        return;
    }

    // Tool bar
    ImGui::TextUnformatted("Tool:");
    ImGui::SameLine();
    constexpr std::array tools{TilemapTool::Pencil, TilemapTool::Eraser,
        TilemapTool::Rectangle, TilemapTool::RectErase, TilemapTool::Fill,
        TilemapTool::Eyedropper, TilemapTool::Select};
    for (const TilemapTool t : tools)
    {
        const bool active = m_Tool == t;
        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::SmallButton(ToolLabel(t)))
        {
            m_Tool = t;
            if (m_InStroke)
            {
                CancelStroke();
            }
        }
        if (active)
        {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();

    ImGui::Separator();
    ImGui::TextUnformatted("Tile Palette");
    ImGui::TextDisabled("TileSet: %s", tm->TileSetTexture.IsValid() ? "assigned" : "(none)");

    // Simple numbered tile palette grid
    const int totalTiles = std::max(1, tm->SheetColumns * tm->SheetRows);
    const int cols = std::max(1, m_PaletteColumns);
    ImGui::SetNextItemWidth(120.0F);
    ImGui::DragInt("Palette cols##TM", &m_PaletteColumns, 1.0F, 1, 32);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{2.0F, 2.0F});
    for (int i = 0; i < totalTiles; ++i)
    {
        if (i % cols != 0)
        {
            ImGui::SameLine();
        }
        const bool selected = m_SelectedTileId == i;
        if (selected)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        char label[16];
        std::snprintf(label, sizeof(label), "%d##T%d", i, i);
        if (ImGui::SmallButton(label))
        {
            m_SelectedTileId = i;
        }
        if (selected)
        {
            ImGui::PopStyleColor();
        }
    }
    ImGui::PopStyleVar();

    ImGui::Separator();
    ImGui::TextUnformatted("Layers");

    m_ActiveLayer = std::clamp(m_ActiveLayer, 0, std::max(0, tm->LayerCount - 1));
    for (int i = 0; i < tm->LayerCount; ++i)
    {
        ImGui::PushID(i);
        const bool active = m_ActiveLayer == i;
        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        char label[32];
        std::snprintf(label, sizeof(label), "Layer %d", i);
        if (ImGui::SmallButton(label))
        {
            m_ActiveLayer = i;
        }
        if (active)
        {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        if (tm->LayerCount > 1)
        {
            if (ImGui::SmallButton("X##Del"))
            {
                // Capture before for undo
                const auto beforeSnap = EntitySnapshot::Capture(world, *sel);
                // Remove the layer from TileData and decrement LayerCount
                const int layerSize = tm->GridWidth * tm->GridHeight;
                const auto begin = tm->TileData.begin() +
                    static_cast<std::ptrdiff_t>(i) * layerSize;
                tm->TileData.erase(begin, begin + layerSize);
                --tm->LayerCount;
                if (m_ActiveLayer >= tm->LayerCount)
                {
                    m_ActiveLayer = tm->LayerCount - 1;
                }
                if (beforeSnap)
                {
                    const auto afterSnap = EntitySnapshot::Capture(world, *sel);
                    if (afterSnap)
                    {
                        history.Push(std::make_unique<SnapshotEntityCommand>(
                            world, *beforeSnap, *afterSnap, "Remove TileMap Layer"));
                    }
                }
            }
        }
        ImGui::SameLine();
        if (i > 0 && ImGui::SmallButton("^##Up"))
        {
            const auto beforeSnap = EntitySnapshot::Capture(world, *sel);
            const int layerSize = tm->GridWidth * tm->GridHeight;
            const auto thisBegin = tm->TileData.begin() +
                static_cast<std::ptrdiff_t>(i) * layerSize;
            const auto prevBegin = thisBegin - layerSize;
            std::rotate(prevBegin, thisBegin, thisBegin + layerSize);
            --m_ActiveLayer;
            if (beforeSnap)
            {
                const auto afterSnap = EntitySnapshot::Capture(world, *sel);
                if (afterSnap)
                {
                    history.Push(std::make_unique<SnapshotEntityCommand>(
                        world, *beforeSnap, *afterSnap, "Reorder TileMap Layer"));
                }
            }
        }
        ImGui::PopID();
    }

    if (ImGui::SmallButton("+ Add Layer"))
    {
        const auto beforeSnap = EntitySnapshot::Capture(world, *sel);
        const int layerSize = tm->GridWidth * tm->GridHeight;
        tm->TileData.resize(
            tm->TileData.size() + static_cast<std::size_t>(layerSize), -1);
        ++tm->LayerCount;
        m_ActiveLayer = tm->LayerCount - 1;
        if (beforeSnap)
        {
            const auto afterSnap = EntitySnapshot::Capture(world, *sel);
            if (afterSnap)
            {
                history.Push(std::make_unique<SnapshotEntityCommand>(
                    world, *beforeSnap, *afterSnap, "Add TileMap Layer"));
            }
        }
    }

    ImGui::Separator();
    ImGui::Text("Grid: %dx%d  Tile: %dx%d px  Selected: %d",
        tm->GridWidth, tm->GridHeight, tm->TileWidth, tm->TileHeight, m_SelectedTileId);
    if (m_HasHoveredTile)
    {
        ImGui::Text("Hovered: (%d, %d)", m_HoveredTile.x, m_HoveredTile.y);
    }

    ImGui::End();
    static_cast<void>(history);
}
}
