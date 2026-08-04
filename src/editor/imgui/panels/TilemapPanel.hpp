#pragma once

#include "editor/scene/SceneEditor.hpp"
#include "editor/imgui/EditorUiState.hpp"
#include "engine/command/UndoStack.hpp"
#include "engine/Uuid.hpp"
#include "runtime/Components.hpp"

#include <glm/vec2.hpp>

#include <optional>
#include <vector>

namespace fadix
{
class IWorld;
}

namespace fadix::editor
{
enum class TilemapTool
{
    Pencil,
    Eraser,
    Rectangle,
    RectErase,
    Fill,
    Eyedropper,
    Select
};

class TilemapPanel final
{
public:
    void Draw(SceneEditor& scene, EditorUiState& ui, IWorld& world, UndoStack& history);

    // Called from ViewportPanel when in 2D mode with a TileMap selected.
    // Returns true if the event was consumed.
    bool HandleViewportMouseDown(glm::vec2 worldPos, SceneEditor& scene, IWorld& world);
    bool HandleViewportMouseMove(glm::vec2 worldPos, SceneEditor& scene, IWorld& world);
    bool HandleViewportMouseUp(SceneEditor& scene, IWorld& world, UndoStack& history);
    void CancelStroke();

    [[nodiscard]] bool IsActive() const noexcept { return m_HasSelection; }
    [[nodiscard]] TilemapTool ActiveTool() const noexcept { return m_Tool; }
    [[nodiscard]] int SelectedTileId() const noexcept { return m_SelectedTileId; }
    [[nodiscard]] int ActiveLayer() const noexcept { return m_ActiveLayer; }
    [[nodiscard]] glm::ivec2 HoveredTileCoord() const noexcept { return m_HoveredTile; }
    [[nodiscard]] bool HasHoveredTile() const noexcept { return m_HasHoveredTile; }

private:
    [[nodiscard]] static glm::ivec2 WorldToTile(glm::vec2 worldPos, const TileMapComponent& tm);
    [[nodiscard]] static bool TileInBounds(glm::ivec2 coord, const TileMapComponent& tm);
    int& TileAt(TileMapComponent& tm, int x, int y) const;

    void PaintTile(TileMapComponent& tm, int x, int y, int tileId);
    void FloodFill(TileMapComponent& tm, int x, int y, int newTile, int limit);
    void PaintRect(TileMapComponent& tm, glm::ivec2 a, glm::ivec2 b, int tileId);

    TilemapTool m_Tool{TilemapTool::Pencil};
    int m_SelectedTileId{0};
    int m_ActiveLayer{0};
    int m_PaletteColumns{8};

    bool m_HasSelection{false};
    std::optional<Uuid> m_SelectedEntity;

    // Stroke tracking for undo
    bool m_InStroke{false};
    std::optional<TileMapComponent> m_StrokeSnapshot;
    glm::ivec2 m_RectStart{0};
    bool m_HasHoveredTile{false};
    glm::ivec2 m_HoveredTile{0};

    // Layer rename buffer
    char m_LayerNameBuf[64]{};
};
}
