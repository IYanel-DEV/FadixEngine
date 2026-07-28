#pragma once

#include "editor/command/EntityCommands.hpp"
#include "engine/Uuid.hpp"
#include "engine/assets/AssetHandle.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fadix
{
class AssetDatabase;
class GltfMeshCache;
class IWorld;
class UndoStack;

// UI-neutral selection + mutation layer. No RmlUi headers; safe to include from ImGui panels.
class SceneEditor
{
public:
    SceneEditor(IWorld& world, UndoStack& history);

    IWorld& World() noexcept;
    UndoStack& History() noexcept;
    std::optional<Uuid> Selection() const;
    // recordUndo=true: pushes SelectEntityCommand (Execute fires immediately).
    void SetSelection(std::optional<Uuid> selection, bool recordUndo = false);
    void SetFilter(std::string filter);
    const std::string& Filter() const noexcept;
    std::optional<Uuid> SceneRootId() const;
    bool IsSceneRoot(const Uuid& id) const;

    // Adopts orphan entities under the scene root; calls MarkChanged if any were adopted.
    void EnsureOrphansAdopted();

    struct HierarchyNode
    {
        Uuid Id;
        std::string Name;
        Uuid Parent;
        const char* Icon;
        int Depth;
        bool IsRoot;
    };
    // Recursive DFS order, filter-aware (ancestor of a match is preserved).
    std::vector<HierarchyNode> BuildHierarchy() const;

    std::optional<Uuid> CreateEntity(std::string name = "Entity");
    std::optional<Uuid> CreateImportedMeshEntity(
        std::string name, AssetHandle handle, const glm::vec3& gridPosition);
    // Instantiate a .prefab file into the scene at gridPosition; returns the root.
    std::optional<Uuid> InstantiatePrefab(
        const std::filesystem::path& path, const glm::vec3& gridPosition);
    bool DuplicateSelection();
    bool DeleteSelection();
    bool RenameSelection(std::string name);
    // Reject if newParent is entity itself or a descendant (ReparentEntityCommand already guards).
    bool Reparent(Uuid entity, Uuid newParent);

    // "add-component-mesh" etc. — pushes SnapshotEntityCommand, does NOT close/refresh Rml UI.
    bool AddComponent(std::string_view optionId);
    // "remove-mesh" etc.
    bool RemoveComponent(std::string_view removeId);

    bool ApplyNumericField(std::string_view id, float value);
    [[nodiscard]] std::optional<float> NumericFieldValue(std::string_view id) const;
    [[nodiscard]] static float NumericDragSensitivity(std::string_view id, float startValue);

    // Undo transaction for ImGui DragFloat / scrub sequences.
    bool BeginEditTransaction();
    bool EndEditTransaction(const char* commandName);
    bool CancelEditTransaction();

    bool AssignMaterial(AssetHandle handle);
    bool ClearMaterial();
    bool AssignImportedMesh(AssetHandle handle);
    bool ClearImportedMesh();
    bool AssignSound(AssetHandle handle);
    bool AssignScript(AssetHandle handle);
    bool AssignUIAsset(AssetHandle handle);
    bool AssignUIStyle(AssetHandle handle);
    bool AssignTerrainHeightmap(AssetHandle handle);
    bool AssignTerrainAlbedo(AssetHandle handle);
    bool AssignScriptTarget(Uuid entityId);
    bool ClearScriptTarget();

    // Uses provider set via SetSelectedMaterial/MeshProvider.
    bool AssignMaterialFromSelection();
    bool AssignMeshFromSelection();

    void SetChangedCallback(std::function<void()> callback);
    void SetAssetDatabase(AssetDatabase* database);
    void SetGltfMeshCache(GltfMeshCache* cache);
    void SetSelectedMaterialProvider(std::function<std::optional<AssetHandle>()> provider);
    void SetSelectedMeshProvider(std::function<std::optional<AssetHandle>()> provider);
    void SetStatusReporter(std::function<void(std::string_view)> reporter);

    void MarkChanged();
    void Report(std::string_view message) const;

    AssetDatabase* Assets() const noexcept;
    GltfMeshCache* GltfMeshes() const noexcept;

private:
    struct EditTransaction
    {
        EntitySnapshot Before;
    };

    IWorld& m_World;
    UndoStack& m_History;
    AssetDatabase* m_Assets{};
    GltfMeshCache* m_GltfMeshes{};
    std::optional<Uuid> m_Selection;
    std::string m_Filter;
    std::function<void()> m_Changed;
    std::function<std::optional<AssetHandle>()> m_SelectedMaterial;
    std::function<std::optional<AssetHandle>()> m_SelectedMesh;
    std::function<void(std::string_view)> m_ReportStatus;
    std::optional<EditTransaction> m_EditTx;
};
} // namespace fadix
