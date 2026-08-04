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
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

void HierarchyPanel::DrawCreateItems(SceneEditor& scene, EditorUiState& ui, const Uuid& parent)
{
    // Every preset is one undoable AddEntityCommand carrying its components, so
    // Undo removes the whole entity (and its parenting) in a single step. Never
    // mutate the registry after the command is pushed.
    auto spawn = [&](const char* name, auto&& setup) {
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
        spawn("Entity", [](AddEntityCommand&) {});
    }
    if (ImGui::BeginMenu("2D"))
    {
        if (ImGui::MenuItem("Sprite 2D"))
        {
            spawn("Sprite 2D", [](AddEntityCommand& c) {
                c.SetSprite2D(Sprite2DComponent{}); // opaque white tint by default
            });
        }
        if (ImGui::MenuItem("Animated Sprite 2D"))
        {
            spawn("Animated Sprite 2D", [](AddEntityCommand& c) {
                c.SetSprite2D(Sprite2DComponent{});
                c.SetSpriteFrameAnimator(SpriteFrameAnimatorComponent{});
            });
        }
        if (ImGui::MenuItem("Tile Map 2D"))
        {
            spawn("Tile Map 2D",
                [](AddEntityCommand& c) { c.SetTileMap(MakeDefaultTileMap()); });
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Camera 2D"))
        {
            spawn("Camera 2D", [](AddEntityCommand& c) {
                CameraComponent cam;
                cam.Orthographic = true;
                cam.OrthoSize = 5.0F;
                c.SetCamera(cam);
            });
        }
        ImGui::Separator();
        const auto makeBody = [](Body2DType type, bool sensor) {
            return [type, sensor](AddEntityCommand& c) {
                RigidBody2DComponent rb;
                rb.Type = type;
                c.SetRigidBody2D(rb);
                Collider2DComponent col;
                col.Sensor = sensor;
                c.SetCollider2D(col);
            };
        };
        if (ImGui::MenuItem("Static Body 2D"))
        {
            spawn("Static Body 2D", makeBody(Body2DType::Static, false));
        }
        if (ImGui::MenuItem("Kinematic Body 2D"))
        {
            spawn("Kinematic Body 2D", makeBody(Body2DType::Kinematic, false));
        }
        if (ImGui::MenuItem("Dynamic Body 2D"))
        {
            spawn("Dynamic Body 2D", makeBody(Body2DType::Dynamic, false));
        }
        if (ImGui::MenuItem("Area 2D"))
        {
            spawn("Area 2D", makeBody(Body2DType::Static, true));
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Mesh"))
    {
        const auto mesh = [](MeshKind kind) {
            return [kind](AddEntityCommand& c) {
                MeshComponent m;
                m.Kind = kind;
                c.SetMesh(std::move(m));
            };
        };
        if (ImGui::MenuItem("Cube")) { spawn("Cube", mesh(MeshKind::Cube)); }
        if (ImGui::MenuItem("Sphere")) { spawn("Sphere", mesh(MeshKind::Sphere)); }
        if (ImGui::MenuItem("Plane")) { spawn("Plane", mesh(MeshKind::Plane)); }
        if (ImGui::MenuItem("Cylinder")) { spawn("Cylinder", mesh(MeshKind::Cylinder)); }
        if (ImGui::MenuItem("Capsule")) { spawn("Capsule", mesh(MeshKind::Capsule)); }
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
            spawn("Sun Light",
                [](AddEntityCommand& c) { c.SetDirectionalLight(MakeSunLight()); });
        }
        if (ImGui::MenuItem("Moon"))
        {
            spawn("Moon Light",
                [](AddEntityCommand& c) { c.SetDirectionalLight(MakeMoonLight()); });
        }
        if (ImGui::MenuItem("Directional"))
        {
            spawn("Directional Light", [](AddEntityCommand& c) {
                c.SetDirectionalLight(DirectionalLightComponent{});
            });
        }
        if (ImGui::MenuItem("Point"))
        {
            spawn("Point Light",
                [](AddEntityCommand& c) { c.SetPointLight(PointLightComponent{}); });
        }
        if (ImGui::MenuItem("Spot"))
        {
            spawn("Spot Light",
                [](AddEntityCommand& c) { c.SetSpotLight(SpotLightComponent{}); });
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Environment"))
    {
        spawn("Environment",
            [](AddEntityCommand& c) { c.SetEnvironment(EnvironmentComponent{}); });
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
        DrawCreateItems(scene, ui, scene.SceneRootId().value_or(Uuid{}));
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
    if (ImGui::MenuItem("Copy UUID"))
    {
        const std::string uuid = id.ToString();
        ImGui::SetClipboardText(uuid.c_str());
        ui.StatusText = "Copied entity UUID";
    }
    ImGui::Separator();
    std::optional<Uuid> parent;
    if (const auto entity = scene.World().Find(id))
    {
        if (const auto* relationship =
                scene.World().Registry().try_get<RelationshipComponent>(*entity);
            relationship != nullptr && relationship->Parent.IsValid() &&
            scene.World().Find(relationship->Parent))
        {
            parent = relationship->Parent;
        }
    }
    if (ImGui::BeginMenu("Create Child"))
    {
        // Child is parented to this entity as part of the single AddEntityCommand.
        DrawCreateItems(scene, ui, id);
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Select Parent", nullptr, false, parent.has_value()))
    {
        SelectOnly(scene, *parent, true);
        ui.StatusText = "Selected parent entity";
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
    ImGui::Separator();
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

    // Build parent -> children (in DFS order) so the flat BuildHierarchy() list
    // renders as a real collapsible tree. Keyed by Uuid string for a stable hash.
    std::unordered_set<std::string> visibleIds;
    for (const auto& node : visible)
    {
        visibleIds.insert(node.Id.ToString());
    }
    std::unordered_map<std::string, std::vector<const SceneEditor::HierarchyNode*>> childrenByParent;
    std::vector<const SceneEditor::HierarchyNode*> roots;
    for (const auto& node : visible)
    {
        const bool parented = !node.IsRoot && node.Parent.IsValid() &&
            visibleIds.count(node.Parent.ToString()) > 0;
        if (parented)
        {
            childrenByParent[node.Parent.ToString()].push_back(&node);
        }
        else
        {
            roots.push_back(&node);
        }
    }

    // Per-row renderer, recursive so children get true tree indentation.
    std::function<void(const SceneEditor::HierarchyNode&)> renderNode =
        [&](const SceneEditor::HierarchyNode& node) {
            ImGui::PushID(node.Id.ToString().c_str());
            const auto childIt = childrenByParent.find(node.Id.ToString());
            const bool hasChildren =
                childIt != childrenByParent.end() && !childIt->second.empty();

            bool visibleInEditor = true;
            if (const auto entity = scene.World().Find(node.Id))
            {
                if (const auto* visibility =
                        scene.World().Registry().try_get<VisibilityComponent>(*entity))
                {
                    visibleInEditor = visibility->VisibleInEditor;
                }
            }

            // Inline rename replaces the row label while active.
            if (m_RenameTarget && *m_RenameTarget == node.Id)
            {
                ImGui::SetNextItemWidth(-1.0F);
                if (ImGui::InputText("##rename", m_RenameBuf, sizeof(m_RenameBuf),
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
                ImGui::PopID();
                return;
            }

            std::string label = HierarchyGlyph(node.Icon);
            label += ' ';
            label += node.Name;
            label += ComponentBadges(scene, node.Id);

            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
            if (IsMultiSelected(node.Id))
            {
                flags |= ImGuiTreeNodeFlags_Selected;
            }
            if (!hasChildren)
            {
                flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            }
            if (node.IsRoot)
            {
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            }
            if (!visibleInEditor)
            {
                ImGui::PushStyleColor(
                    ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }
            const bool open = ImGui::TreeNodeEx("##node", flags, "%s", label.c_str());
            if (!visibleInEditor)
            {
                ImGui::PopStyleColor();
            }

            // Left click on the label (not the arrow — OpenOnArrow) selects.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
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
            // Right click targets the entity (select it if not already selected).
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !IsMultiSelected(node.Id))
            {
                SelectOnly(scene, node.Id, false);
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
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FADIX_ENTITY"))
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

            // Visibility toggle, right-aligned on the row.
            const float eyeW = ImGui::GetFrameHeight();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - eyeW);
            if (!visibleInEditor)
            {
                ImGui::PushStyleColor(
                    ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }
            if (ImGui::SmallButton(FADIX_ICON_EYE "##visibility"))
            {
                if (scene.ToggleEditorVisibility(node.Id))
                {
                    ui.StatusText = visibleInEditor ? "Hidden in editor" : "Shown in editor";
                }
            }
            if (!visibleInEditor)
            {
                ImGui::PopStyleColor();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(visibleInEditor ? "Hide in Scene View" : "Show in Scene View");
            }

            if (open && hasChildren)
            {
                for (const SceneEditor::HierarchyNode* child : childIt->second)
                {
                    renderNode(*child);
                }
            }
            if (open && hasChildren)
            {
                ImGui::TreePop();
            }
            ImGui::PopID();
        };

    for (const SceneEditor::HierarchyNode* root : roots)
    {
        renderNode(*root);
    }

    // Right-click empty space -> root-level Create menu. NoOpenOverItems keeps it
    // from firing on top of an entity row (those use the per-item context menu).
    if (ImGui::BeginPopupContextWindow("##hier_blank_ctx",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::BeginMenu("Create"))
        {
            DrawCreateItems(scene, ui, scene.SceneRootId().value_or(Uuid{}));
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
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
