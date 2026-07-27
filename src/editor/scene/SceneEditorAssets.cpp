// Split out of SceneEditor.cpp to reduce god-file size. Definitions only; all
// declarations remain in editor/scene/SceneEditor.hpp. No behavior change.
#include "editor/scene/SceneEditor.hpp"

#include "assets/AssetDatabase.hpp"
#include "assets/GltfMeshCache.hpp"
#include "editor/command/EntityCommands.hpp"
#include "engine/assets/MaterialAsset.hpp"
#include "engine/command/UndoStack.hpp"
#include "engine/scene/IWorld.hpp"
#include "runtime/Components.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>

namespace fadix
{

bool SceneEditor::AssignMaterial(const AssetHandle handle)
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    MeshComponent* mesh = m_World.Registry().try_get<MeshComponent>(*entity);
    if (mesh == nullptr)
    {
        Report("Material can only be assigned to mesh entities");
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    mesh->Material = handle;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Assign Material"));
    }
    MarkChanged();
    Report("Assigned material to mesh");
    return true;
}

bool SceneEditor::ClearMaterial()
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    MeshComponent* mesh = m_World.Registry().try_get<MeshComponent>(*entity);
    if (mesh == nullptr || !mesh->Material.IsValid())
    {
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    mesh->Material = AssetHandle{};
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Clear Material"));
    }
    MarkChanged();
    Report("Cleared mesh material");
    return true;
}

bool SceneEditor::AssignImportedMesh(const AssetHandle handle)
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    MeshComponent* mesh = m_World.Registry().try_get<MeshComponent>(*entity);
    if (mesh == nullptr)
    {
        Report("Mesh can only be assigned to mesh entities");
        return false;
    }
    if (m_Assets == nullptr || m_GltfMeshes == nullptr)
    {
        Report("Mesh importer is not available");
        return false;
    }
    const AssetMetadata* metadata = m_Assets->Meta(handle);
    if (metadata == nullptr || metadata->Type != "Mesh")
    {
        Report("Selected asset is not an imported mesh");
        return false;
    }
    const std::filesystem::path& path = !metadata->SourcePath.empty()
        ? metadata->SourcePath
        : metadata->ImportedPath;
    if (m_GltfMeshes->Load(handle, path.string(), [this](const std::string& message) {
            Report(message);
        }) == nullptr)
    {
        Report("Could not load imported mesh " + path.filename().string());
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    mesh->ImportedMesh = handle;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Assign Imported Mesh"));
    }
    MarkChanged();
    Report("Assigned imported mesh");
    return true;
}

bool SceneEditor::ClearImportedMesh()
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    MeshComponent* mesh = m_World.Registry().try_get<MeshComponent>(*entity);
    if (mesh == nullptr || !mesh->ImportedMesh.IsValid())
    {
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    mesh->ImportedMesh = AssetHandle{};
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Clear Imported Mesh"));
    }
    MarkChanged();
    Report("Cleared imported mesh");
    return true;
}

bool SceneEditor::AssignSound(const AssetHandle handle)
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    AudioSourceComponent* audio = m_World.Registry().try_get<AudioSourceComponent>(*entity);
    if (audio == nullptr)
    {
        Report("Audio can only be assigned to Audio Source entities");
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    audio->Sound = handle;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Assign Sound"));
    }
    MarkChanged();
    Report("Assigned sound to Audio Source");
    return true;
}

bool SceneEditor::AssignScript(const AssetHandle handle)
{
    if (!m_Selection)
    {
        Report("Select an entity before attaching a script");
        return false;
    }
    if (m_Assets == nullptr)
    {
        Report("Asset database is unavailable");
        return false;
    }
    const AssetMetadata* metadata = m_Assets->Meta(handle);
    if (metadata == nullptr || metadata->Type != "Script")
    {
        Report("Dropped asset is not a script");
        return false;
    }
    const std::string scriptName = metadata->SourcePath.stem().string();
    if (scriptName.empty())
    {
        Report("Script has no valid name");
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }

    entt::registry& registry = m_World.Registry();
    if (const ScriptComponent* scripts = registry.try_get<ScriptComponent>(*entity);
        scripts != nullptr &&
        std::find(scripts->ScriptNames.begin(), scripts->ScriptNames.end(), scriptName) !=
            scripts->ScriptNames.end())
    {
        Report("Script is already attached");
        return false;
    }

    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    registry.get_or_emplace<ScriptComponent>(*entity).ScriptNames.push_back(scriptName);
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Attach Script"));
    }
    MarkChanged();
    Report("Attached script '" + scriptName + "'");
    return true;
}

bool SceneEditor::AssignUIAsset(const AssetHandle handle)
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    UICanvasComponent* ui = m_World.Registry().try_get<UICanvasComponent>(*entity);
    if (ui == nullptr)
    {
        Report("UI can only be assigned to UI Canvas entities");
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    ui->UIAsset = handle;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Assign UI"));
    }
    MarkChanged();
    Report("Assigned UI document");
    return true;
}

bool SceneEditor::AssignUIStyle(const AssetHandle handle)
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    UICanvasComponent* ui = m_World.Registry().try_get<UICanvasComponent>(*entity);
    if (ui == nullptr)
    {
        Report("UIStyle can only be assigned to UI Canvas entities");
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    ui->StyleAsset = handle;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Assign UI Style"));
    }
    MarkChanged();
    Report("Assigned UI style");
    return true;
}

bool SceneEditor::AssignTerrainHeightmap(const AssetHandle handle)
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    TerrainComponent* terrain = m_World.Registry().try_get<TerrainComponent>(*entity);
    if (terrain == nullptr)
    {
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    terrain->Heightmap = handle;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Assign Heightmap"));
    }
    MarkChanged();
    Report("Assigned heightmap");
    return true;
}

bool SceneEditor::AssignTerrainAlbedo(AssetHandle /*handle*/)
{
    // ponytail: per-layer albedo slot requires a layer index; not yet wired.
    return false;
}

bool SceneEditor::AssignScriptTarget(const Uuid entityId)
{
    if (!m_Selection || !entityId.IsValid())
    {
        return false;
    }
    if (entityId == *m_Selection)
    {
        Report("Script Target cannot be the same entity");
        return false;
    }
    if (!m_World.Find(entityId))
    {
        Report("Dropped entity no longer exists");
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    ScriptComponent* scripts = m_World.Registry().try_get<ScriptComponent>(*entity);
    if (scripts == nullptr)
    {
        Report("Add a Script component before setting Target");
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    scripts->Target = entityId;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Set Script Target"));
    }
    MarkChanged();
    if (const auto targetEntity = m_World.Find(entityId))
    {
        if (const NameComponent* name =
                m_World.Registry().try_get<NameComponent>(*targetEntity))
        {
            Report("Script Target set to " + name->Name);
            return true;
        }
    }
    Report("Script Target set");
    return true;
}

bool SceneEditor::ClearScriptTarget()
{
    if (!m_Selection)
    {
        return false;
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    ScriptComponent* scripts = m_World.Registry().try_get<ScriptComponent>(*entity);
    if (scripts == nullptr)
    {
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    scripts->Target = {};
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Clear Script Target"));
    }
    MarkChanged();
    return true;
}

bool SceneEditor::AssignMaterialFromSelection()
{
    if (!m_Selection || m_SelectedMaterial == nullptr)
    {
        Report("Select a Material asset in the Asset Browser, then click Assign");
        return false;
    }
    const std::optional<AssetHandle> handle = m_SelectedMaterial();
    if (!handle || !handle->IsValid())
    {
        Report("Select a Material asset in the Asset Browser, then click Assign");
        return false;
    }
    if (m_Assets != nullptr)
    {
        if (const AssetMetadata* meta = m_Assets->Meta(*handle);
            meta == nullptr || meta->Type != "Material")
        {
            Report("Selected asset is not a Material");
            return false;
        }
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    MeshComponent* mesh = m_World.Registry().try_get<MeshComponent>(*entity);
    if (mesh == nullptr)
    {
        Report("Selected entity has no Mesh component");
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    mesh->Material = *handle;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Assign Material"));
    }
    Report("Assigned material to mesh");
    return true;
}

bool SceneEditor::AssignMeshFromSelection()
{
    if (!m_Selection || m_SelectedMesh == nullptr)
    {
        Report("Select a Mesh asset in the Asset Browser, then click Assign");
        return false;
    }
    const std::optional<AssetHandle> handle = m_SelectedMesh();
    if (!handle || !handle->IsValid())
    {
        Report("Select a Mesh asset in the Asset Browser, then click Assign");
        return false;
    }
    if (m_Assets != nullptr)
    {
        if (const AssetMetadata* meta = m_Assets->Meta(*handle);
            meta == nullptr || meta->Type != "Mesh")
        {
            Report("Selected asset is not a Mesh");
            return false;
        }
    }
    const auto entity = m_World.Find(*m_Selection);
    if (!entity)
    {
        return false;
    }
    MeshComponent* mesh = m_World.Registry().try_get<MeshComponent>(*entity);
    if (mesh == nullptr)
    {
        Report("Selected entity has no Mesh component");
        return false;
    }
    auto before = EntitySnapshot::Capture(m_World, *m_Selection);
    mesh->ImportedMesh = *handle;
    if (auto after = EntitySnapshot::Capture(m_World, *m_Selection); before && after)
    {
        m_History.Push(std::make_unique<SnapshotEntityCommand>(
            m_World, std::move(*before), std::move(*after), "Assign Imported Mesh"));
    }
    Report("Assigned imported mesh");
    return true;
}
} // namespace fadix
