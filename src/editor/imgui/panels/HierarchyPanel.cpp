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
#include <vector>

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

[[nodiscard]] bool IsLightIcon(const char* icon)
{
    return icon != nullptr &&
        (std::strcmp(icon, "light") == 0 || std::strcmp(icon, "point-light") == 0 ||
            std::strcmp(icon, "spot-light") == 0);
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

void HierarchyPanel::SyncMultiFromPrimary(SceneEditor& scene)
{
    if (const auto primary = scene.Selection())
    {
        if (!IsMultiSelected(*primary))
        {
            m_Multi = {*primary};
            m_RangeAnchor = *primary;
        }
    }
    else
    {
        m_Multi.clear();
        m_RangeAnchor.reset();
    }
}

bool HierarchyPanel::IsMultiSelected(const Uuid& id) const
{
    return std::find(m_Multi.begin(), m_Multi.end(), id) != m_Multi.end();
}

void HierarchyPanel::SelectOnly(SceneEditor& scene, const Uuid& id, const bool recordUndo)
{
    m_Multi = {id};
    m_RangeAnchor = id;
    scene.SetSelection(id, recordUndo);
}

void HierarchyPanel::ToggleMulti(SceneEditor& scene, const Uuid& id)
{
    const auto it = std::find(m_Multi.begin(), m_Multi.end(), id);
    if (it != m_Multi.end())
    {
        m_Multi.erase(it);
        if (m_Multi.empty())
        {
            scene.SetSelection(std::nullopt, false);
            m_RangeAnchor.reset();
            return;
        }
        scene.SetSelection(m_Multi.back(), false);
        m_RangeAnchor = m_Multi.back();
        return;
    }
    m_Multi.push_back(id);
    m_RangeAnchor = id;
    scene.SetSelection(id, false);
}

void HierarchyPanel::SelectRange(
    SceneEditor& scene, const std::vector<Uuid>& visibleOrder, const Uuid& id)
{
    if (!m_RangeAnchor || visibleOrder.empty())
    {
        SelectOnly(scene, id, true);
        return;
    }
    std::size_t a = static_cast<std::size_t>(-1);
    std::size_t b = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < visibleOrder.size(); ++i)
    {
        if (visibleOrder[i] == *m_RangeAnchor) a = i;
        if (visibleOrder[i] == id) b = i;
    }
    if (a == static_cast<std::size_t>(-1) || b == static_cast<std::size_t>(-1))
    {
        SelectOnly(scene, id, true);
        return;
    }
    if (a > b)
    {
        std::swap(a, b);
    }
    m_Multi.clear();
    for (std::size_t i = a; i <= b; ++i)
    {
        m_Multi.push_back(visibleOrder[i]);
    }
    scene.SetSelection(id, false);
}

bool HierarchyPanel::DeleteMulti(SceneEditor& scene, EditorUiState& ui)
{
    SyncMultiFromPrimary(scene);
    if (m_Multi.empty())
    {
        return false;
    }
    int deleted = 0;
    // Copy: commands mutate the world and selection as we go.
    const std::vector<Uuid> targets = m_Multi;
    for (const Uuid& id : targets)
    {
        if (scene.IsSceneRoot(id))
        {
            continue;
        }
        scene.SetSelection(id, false);
        if (scene.DeleteSelection())
        {
            ++deleted;
        }
    }
    m_Multi.clear();
    m_RangeAnchor.reset();
    scene.SetSelection(std::nullopt, false);
    if (deleted > 0)
    {
        ui.StatusText = "Deleted " + std::to_string(deleted) + " entit" + (deleted == 1 ? "y" : "ies");
        return true;
    }
    ui.StatusText = "Nothing deletable (scene root protected)";
    return false;
}

bool HierarchyPanel::DuplicateMulti(SceneEditor& scene, EditorUiState& ui)
{
    SyncMultiFromPrimary(scene);
    if (m_Multi.empty())
    {
        return false;
    }
    std::vector<Uuid> created;
    created.reserve(m_Multi.size());
    const std::vector<Uuid> targets = m_Multi;
    for (const Uuid& id : targets)
    {
        scene.SetSelection(id, false);
        if (scene.DuplicateSelection())
        {
            if (const auto sel = scene.Selection())
            {
                created.push_back(*sel);
            }
        }
    }
    if (created.empty())
    {
        return false;
    }
    m_Multi = created;
    m_RangeAnchor = created.back();
    scene.SetSelection(created.back(), false);
    ui.StatusText = "Duplicated " + std::to_string(created.size());
    return true;
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
            SelectOnly(scene, created, true);
            ui.StatusText = std::string{"Created "} + name;
            ImGui::CloseCurrentPopup();
        };
        if (ImGui::MenuItem("Empty"))
        {
            if (const auto id = scene.CreateEntity("Entity"))
            {
                SelectOnly(scene, *id, true);
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
    SyncMultiFromPrimary(scene);
    if (!m_Multi.empty())
    {
        if (ImGui::Button("Duplicate"))
        {
            DuplicateMulti(scene, ui);
        }
    }
    else
    {
        DisabledButton("Duplicate", "Select an entity first");
    }
    ImGui::SameLine();
    const bool canDelete = std::any_of(m_Multi.begin(), m_Multi.end(), [&](const Uuid& id) {
        return !scene.IsSceneRoot(id);
    });
    if (canDelete)
    {
        if (ImGui::Button("Delete"))
        {
            DeleteMulti(scene, ui);
        }
    }
    else
    {
        DisabledButton(
            "Delete",
            m_Multi.empty() ? "Select an entity first" : "Cannot delete the scene root");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Hide lights", &m_HideLights);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Hide directional / point / spot lights from this list.\n"
                          "Create > Light still adds them; they keep affecting the scene.");
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
        if (!IsMultiSelected(id))
        {
            SelectOnly(scene, id, false);
        }
        DuplicateMulti(scene, ui);
    }
    if (ImGui::MenuItem("Create Child"))
    {
        if (const auto child = scene.CreateEntity("Entity"))
        {
            scene.Reparent(*child, id);
            SelectOnly(scene, *child, true);
            ui.StatusText = "Created child entity";
        }
    }
    if (ImGui::MenuItem("Select Children"))
    {
        m_Multi.clear();
        for (const auto& node : scene.BuildHierarchy())
        {
            if (node.Parent == id)
            {
                m_Multi.push_back(node.Id);
            }
        }
        if (!m_Multi.empty())
        {
            m_RangeAnchor = m_Multi.front();
            scene.SetSelection(m_Multi.front(), false);
            ui.StatusText = "Selected " + std::to_string(m_Multi.size()) + " children";
        }
    }
    const bool canDelete = !scene.IsSceneRoot(id);
    if (canDelete && ImGui::MenuItem(m_Multi.size() > 1 ? "Delete Selected" : "Delete"))
    {
        if (!IsMultiSelected(id))
        {
            SelectOnly(scene, id, false);
        }
        DeleteMulti(scene, ui);
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
    SyncMultiFromPrimary(scene);
    if (const auto selection = scene.Selection())
    {
        if (!scene.World().Find(*selection))
        {
            scene.SetSelection(std::nullopt, false);
            m_Multi.clear();
            m_RangeAnchor.reset();
        }
    }

    const auto nodes = scene.BuildHierarchy();
    std::vector<SceneEditor::HierarchyNode> visible;
    visible.reserve(nodes.size());
    int hiddenLights = 0;
    for (const auto& node : nodes)
    {
        if (m_HideLights && !node.IsRoot && IsLightIcon(node.Icon))
        {
            ++hiddenLights;
            continue;
        }
        visible.push_back(node);
    }

    std::vector<Uuid> visibleOrder;
    visibleOrder.reserve(visible.size());
    for (const auto& node : visible)
    {
        visibleOrder.push_back(node.Id);
    }

    {
        std::string status = std::to_string(visible.size()) + " shown";
        if (hiddenLights > 0)
        {
            status += " · " + std::to_string(hiddenLights) + " lights hidden";
        }
        if (m_Multi.size() > 1)
        {
            status += " · " + std::to_string(m_Multi.size()) + " selected";
        }
        ImGui::TextDisabled("%s", status.c_str());
        ImGui::TextDisabled("Ctrl+click multi · Shift+click range · Del delete · F2 rename");
    }

    for (const auto& node : visible)
    {
        ImGui::PushID(node.Id.ToString().c_str());
        const int indentLevels = node.IsRoot ? 0 : std::max(node.Depth, 1);
        std::string label(static_cast<std::size_t>(indentLevels) * 4, ' ');
        label += HierarchyGlyph(node.Icon);
        label += ' ';
        label += node.Name;
        label += ComponentBadges(scene, node.Id);

        const bool selected = IsMultiSelected(node.Id);
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
                const ImGuiIO& io = ImGui::GetIO();
                if (io.KeyShift)
                {
                    SelectRange(scene, visibleOrder, node.Id);
                }
                else if (io.KeyCtrl)
                {
                    ToggleMulti(scene, node.Id);
                }
                else
                {
                    SelectOnly(scene, node.Id, true);
                }
            }
            if (node.IsRoot && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Scene file: %s.scene", ui.SceneName.c_str());
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                SelectOnly(scene, node.Id, true);
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

    if (!m_RenameTarget && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::IsKeyPressed(ImGuiKey_Delete) && !m_Multi.empty())
    {
        DeleteMulti(scene, ui);
    }

    if (visible.empty())
    {
        ImGui::TextDisabled(m_HideLights ? "No entities (lights hidden)" : "No entities");
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
        scene.SetFilter(m_FilterBuf);
    }

    DrawToolbar(scene, ui);
    ImGui::Separator();
    DrawTree(scene, ui);
    ImGui::End();
}
}
