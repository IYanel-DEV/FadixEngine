#include "editor/imgui/panels/HierarchyPanel.hpp"

#include "editor/command/EntityCommands.hpp"
#include "editor/assets/AssetBrowserController.hpp"
#include "editor/imgui/EditorIcons.hpp"
#include "engine/command/UndoStack.hpp"
#include "engine/scene/IWorld.hpp"
#include "runtime/Components.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace fadix::editor
{
namespace
{
std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

void DisabledButton(const char* label, const char* reason)
{
    ImGui::BeginDisabled();
    ImGui::Button(label);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", reason);
    }
}

const char* HierarchyGlyph(const std::string_view icon)
{
    if (icon == "scene") return FADIX_ICON_SITEMAP;
    if (icon == "camera") return FADIX_ICON_CAMERA;
    if (icon == "light" || icon == "point-light" || icon == "spot-light")
        return FADIX_ICON_LIGHT;
    if (icon == "environment") return FADIX_ICON_GLOBE;
    if (icon == "mesh") return FADIX_ICON_CUBE;
    if (icon == "physics3d" || icon == "physics2d") return FADIX_ICON_GEAR;
    if (icon == "network") return FADIX_ICON_SITEMAP;
    return FADIX_ICON_CUBE;
}

std::string ComponentBadges(SceneEditor& scene, const Uuid& id)
{
    const auto entity = scene.World().Find(id);
    if (!entity)
    {
        return {};
    }
    const entt::registry& registry = scene.World().Registry();
    std::string badges;
    if (registry.all_of<ScriptComponent>(*entity)) badges += "  " FADIX_ICON_CODE;
    if (registry.any_of<JoltBodyComponent, CharacterControllerComponent, Box2DBodyComponent>(*entity))
        badges += "  " FADIX_ICON_GEAR;
    if (registry.any_of<AudioSourceComponent, AudioListenerComponent>(*entity))
        badges += "  " FADIX_ICON_AUDIO;
    return badges;
}
}

void HierarchyPanel::DrawToolbar(SceneEditor& scene, EditorUiState& ui)
{
    if (ImGui::Button(FADIX_ICON_PLUS " Create"))
    {
        ImGui::OpenPopup("##hier_create");
    }
    if (ImGui::BeginPopup("##hier_create"))
    {
        auto spawn = [&](const char* name, auto&& setup) {
            const Uuid parent = scene.SceneRootId().value_or(Uuid{});
            auto command = std::make_unique<AddEntityCommand>(scene.World(), name, parent);
            const Uuid created = command->EntityId();
            setup(*command);
            scene.History().Push(std::move(command));
            scene.SetSelection(created, true);
            ui.StatusText = std::string{"Created "} + name;
            ImGui::CloseCurrentPopup();
        };
        if (ImGui::MenuItem("Empty"))
        {
            if (const auto id = scene.CreateEntity("Entity"))
            {
                scene.SetSelection(id, true);
                ui.StatusText = "Created entity";
            }
        }
        if (ImGui::BeginMenu("Mesh"))
        {
            if (ImGui::MenuItem("Cube"))
            {
                spawn("Cube", [](AddEntityCommand& c) {
                    MeshComponent m;
                    m.Kind = MeshKind::Cube;
                    c.SetMesh(std::move(m));
                });
            }
            if (ImGui::MenuItem("Sphere"))
            {
                spawn("Sphere", [](AddEntityCommand& c) {
                    MeshComponent m;
                    m.Kind = MeshKind::Sphere;
                    c.SetMesh(std::move(m));
                });
            }
            if (ImGui::MenuItem("Plane"))
            {
                spawn("Plane", [](AddEntityCommand& c) {
                    MeshComponent m;
                    m.Kind = MeshKind::Plane;
                    c.SetMesh(std::move(m));
                });
            }
            if (ImGui::MenuItem("Cylinder"))
            {
                spawn("Cylinder", [](AddEntityCommand& c) {
                    MeshComponent m;
                    m.Kind = MeshKind::Cylinder;
                    c.SetMesh(std::move(m));
                });
            }
            if (ImGui::MenuItem("Capsule"))
            {
                spawn("Capsule", [](AddEntityCommand& c) {
                    MeshComponent m;
                    m.Kind = MeshKind::Capsule;
                    c.SetMesh(std::move(m));
                });
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Camera"))
        {
            spawn("Camera", [](AddEntityCommand& c) { c.SetCamera(CameraComponent{}); });
        }
        if (ImGui::BeginMenu("Light"))
        {
            if (ImGui::MenuItem("Sun"))
            {
                spawn("Sun Light", [](AddEntityCommand& c) {
                    c.SetDirectionalLight(MakeSunLight());
                });
            }
            if (ImGui::MenuItem("Moon"))
            {
                spawn("Moon Light", [](AddEntityCommand& c) {
                    c.SetDirectionalLight(MakeMoonLight());
                });
            }
            if (ImGui::MenuItem("Directional"))
            {
                spawn("Directional Light", [](AddEntityCommand& c) {
                    c.SetDirectionalLight(DirectionalLightComponent{});
                });
            }
            if (ImGui::MenuItem("Point"))
            {
                spawn("Point Light", [](AddEntityCommand& c) {
                    c.SetPointLight(PointLightComponent{});
                });
            }
            if (ImGui::MenuItem("Spot"))
            {
                spawn("Spot Light", [](AddEntityCommand& c) {
                    c.SetSpotLight(SpotLightComponent{});
                });
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Environment"))
        {
            spawn("Environment", [](AddEntityCommand& c) {
                c.SetEnvironment(EnvironmentComponent{});
            });
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (scene.Selection())
    {
        if (ImGui::Button("Duplicate"))
        {
            if (scene.DuplicateSelection())
            {
                ui.StatusText = "Duplicated entity";
            }
        }
    }
    else
    {
        DisabledButton("Duplicate", "Select an entity first");
    }
    ImGui::SameLine();
    if (scene.Selection() && !scene.IsSceneRoot(*scene.Selection()))
    {
        if (ImGui::Button("Delete"))
        {
            if (scene.DeleteSelection())
            {
                ui.StatusText = "Deleted entity";
            }
        }
    }
    else
    {
        DisabledButton(
            "Delete",
            scene.Selection() ? "Cannot delete the scene root" : "Select an entity first");
    }
}

void HierarchyPanel::DrawContextMenu(SceneEditor& scene, const Uuid& id, EditorUiState& ui)
{
    if (!ImGui::BeginPopupContextItem("hierarchy_ctx"))
    {
        return;
    }
    if (ImGui::MenuItem("Rename"))
    {
        m_RenameTarget = id;
        if (const auto entity = scene.World().Find(id))
        {
            if (const auto* name =
                    scene.World().Registry().try_get<NameComponent>(*entity))
            {
                std::snprintf(m_RenameBuf, sizeof(m_RenameBuf), "%s", name->Name.c_str());
            }
        }
    }
    if (ImGui::MenuItem("Duplicate"))
    {
        scene.SetSelection(id, false);
        if (scene.DuplicateSelection())
        {
            ui.StatusText = "Duplicated entity";
        }
    }
    if (ImGui::MenuItem("Create Child"))
    {
        if (const auto child = scene.CreateEntity("Entity"))
        {
            scene.Reparent(*child, id);
            scene.SetSelection(child, true);
            ui.StatusText = "Created child entity";
        }
    }
    const bool canDelete = !scene.IsSceneRoot(id);
    if (canDelete && ImGui::MenuItem("Delete"))
    {
        scene.SetSelection(id, false);
        if (scene.DeleteSelection())
        {
            ui.StatusText = "Deleted entity";
        }
    }
    else if (!canDelete)
    {
        ImGui::BeginDisabled();
        ImGui::MenuItem("Delete");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Cannot delete the scene root");
        }
    }
    ImGui::EndPopup();
}

void HierarchyPanel::DrawTree(SceneEditor& scene, EditorUiState& ui)
{
    scene.EnsureOrphansAdopted();
    if (const auto selection = scene.Selection())
    {
        if (!scene.World().Find(*selection))
        {
            scene.SetSelection(std::nullopt, false);
        }
    }

    const auto nodes = scene.BuildHierarchy();
    for (const auto& node : nodes)
    {
        ImGui::PushID(node.Id.ToString().c_str());
        const int indentLevels = node.IsRoot ? 0 : std::max(node.Depth, 1);
        std::string label(static_cast<std::size_t>(indentLevels) * 4, ' ');
        label += HierarchyGlyph(node.Icon);
        label += ' ';
        label += node.Name;
        label += ComponentBadges(scene, node.Id);

        const bool selected = scene.Selection() && *scene.Selection() == node.Id;
        if (m_RenameTarget && *m_RenameTarget == node.Id)
        {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + static_cast<float>(indentLevels) * 16.0F);
            ImGui::SetNextItemWidth(-1.0F);
            if (ImGui::InputText(
                    "##rename",
                    m_RenameBuf,
                    sizeof(m_RenameBuf),
                    ImGuiInputTextFlags_EnterReturnsTrue |
                        ImGuiInputTextFlags_AutoSelectAll))
            {
                scene.SetSelection(node.Id, false);
                scene.RenameSelection(m_RenameBuf);
                m_RenameTarget.reset();
                ui.StatusText = "Renamed entity";
            }
            if (!ImGui::IsItemActive() && !ImGui::IsItemFocused() &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                // click away cancels — keep editing until enter or explicit cancel
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
                m_RenameTarget.reset();
            }
        }
        else
        {
            const ImGuiSelectableFlags flags = ImGuiSelectableFlags_AllowDoubleClick;
            if (ImGui::Selectable(label.c_str(), selected, flags))
            {
                scene.SetSelection(node.Id, true);
            }
            if (node.IsRoot && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Scene file: %s.scene", ui.SceneName.c_str());
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                scene.SetSelection(node.Id, true);
                ui.RequestFocusSelection = true;
            }
            if (ImGui::IsItemHovered() && ImGui::IsKeyPressed(ImGuiKey_F2))
            {
                m_RenameTarget = node.Id;
                std::snprintf(m_RenameBuf, sizeof(m_RenameBuf), "%s", node.Name.c_str());
            }

            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
            {
                const std::string idStr = node.Id.ToString();
                ImGui::SetDragDropPayload("FADIX_ENTITY", idStr.data(), idStr.size() + 1);
                ImGui::Text("Move %s", node.Name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("FADIX_ENTITY"))
                {
                    const char* text = static_cast<const char*>(payload->Data);
                    if (const auto src = Uuid::Parse(text ? text : ""))
                    {
                        if (scene.Reparent(*src, node.Id))
                        {
                            ui.StatusText = "Reparented entity";
                        }
                    }
                }
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FADIX_ASSET"))
                {
                    if (const auto asset = ParseAssetDragDropBlob(
                            payload->Data, static_cast<std::size_t>(payload->DataSize));
                        asset && asset->AssetType == "Script")
                    {
                        scene.SetSelection(node.Id, false);
                        if (scene.AssignScript(asset->Handle))
                        {
                            ui.StatusText = "Attached " + asset->SourcePath.stem().string() +
                                " to " + node.Name;
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            DrawContextMenu(scene, node.Id, ui);
        }

        ImGui::PopID();
    }

    if (nodes.empty())
    {
        ImGui::TextDisabled("No entities");
    }
}

void HierarchyPanel::Draw(SceneEditor& scene, EditorUiState& ui)
{
    if (!ui.ShowHierarchy)
    {
        return;
    }
    if (!ImGui::Begin(FADIX_ICON_SITEMAP " Hierarchy###Hierarchy", &ui.ShowHierarchy))
    {
        ImGui::End();
        return;
    }

    if (!m_FilterSynced)
    {
        std::snprintf(m_FilterBuf, sizeof(m_FilterBuf), "%s", scene.Filter().c_str());
        m_FilterSynced = true;
    }
    if (ImGui::InputTextWithHint("##filter", "Search...", m_FilterBuf, sizeof(m_FilterBuf)))
    {
        scene.SetFilter(Lower(m_FilterBuf));
    }

    DrawToolbar(scene, ui);
    ImGui::Separator();
    DrawTree(scene, ui);
    ImGui::End();
}
}
