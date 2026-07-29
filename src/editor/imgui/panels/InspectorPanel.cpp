#include "editor/imgui/panels/InspectorPanel.hpp"

#include "editor/imgui/EditorIcons.hpp"
#include "editor/assets/AssetBrowserController.hpp"
#include "assets/GltfMeshCache.hpp"
#include "engine/assets/GltfMeshAsset.hpp"
#include "engine/scene/IWorld.hpp"
#include "runtime/AnimationRuntime.hpp"
#include "runtime/Components.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

namespace fadix::editor
{
namespace
{
void DrawFloat(SceneEditor& scene, const char* label, const char* fieldId)
{
    const auto current = scene.NumericFieldValue(fieldId);
    if (!current)
    {
        return;
    }
    float value = *current;
    const float speed = SceneEditor::NumericDragSensitivity(fieldId, value);
    const bool changed = ImGui::DragFloat(label, &value, speed);
    if (ImGui::IsItemActivated())
    {
        scene.BeginEditTransaction();
    }
    if (changed)
    {
        scene.ApplyNumericField(fieldId, value);
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        scene.EndEditTransaction("Edit Component");
    }
    else if (ImGui::IsItemDeactivated())
    {
        scene.CancelEditTransaction();
    }
}

void DrawClampedFloat(SceneEditor& scene, const char* label, float& target,
    const float minimum, const float maximum, const float speed = 0.05F)
{
    float value = target;
    const bool changed = ImGui::DragFloat(label, &value, speed, minimum, maximum);
    if (ImGui::IsItemActivated())
    {
        scene.BeginEditTransaction();
    }
    if (changed)
    {
        target = std::clamp(value, minimum, maximum);
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        scene.EndEditTransaction("Edit Component");
    }
    else if (ImGui::IsItemDeactivated())
    {
        scene.CancelEditTransaction();
    }
}

void DrawVec3(SceneEditor& scene, const char* label, const char* prefix)
{
    char idX[64];
    char idY[64];
    char idZ[64];
    std::snprintf(idX, sizeof(idX), "%s-x", prefix);
    std::snprintf(idY, sizeof(idY), "%s-y", prefix);
    std::snprintf(idZ, sizeof(idZ), "%s-z", prefix);
    const auto x = scene.NumericFieldValue(idX);
    const auto y = scene.NumericFieldValue(idY);
    const auto z = scene.NumericFieldValue(idZ);
    if (!x || !y || !z)
    {
        return;
    }
    float v[3] = {*x, *y, *z};
    const float speed = SceneEditor::NumericDragSensitivity(idX, v[0]);
    const bool changed = ImGui::DragFloat3(label, v, speed);
    if (ImGui::IsItemActivated())
    {
        scene.BeginEditTransaction();
    }
    if (changed)
    {
        scene.ApplyNumericField(idX, v[0]);
        scene.ApplyNumericField(idY, v[1]);
        scene.ApplyNumericField(idZ, v[2]);
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        scene.EndEditTransaction("Edit Component");
    }
    else if (ImGui::IsItemDeactivated())
    {
        scene.CancelEditTransaction();
    }
}

// FDX Animation keying row under the Transform header. Works on any entity with a
// TransformComponent (no skinned mesh needed). Keys capture the entity's current TRS
// at the transform animator's scrub time and open FDX Animation for further editing.
void DrawTransformKeys(
    SceneEditor& scene, EditorUiState& ui, entt::registry& registry, const entt::entity entity)
{
    const auto* transformPtr = registry.try_get<TransformComponent>(entity);
    if (transformPtr == nullptr)
    {
        return;
    }
    auto* anim = registry.try_get<TransformAnimatorComponent>(entity);
    const float time = anim != nullptr ? anim->CurrentTime : 0.0F;

    const auto keyed = [&](const AnimationChannel::Property property) {
        const AnimationClipAsset* clip = anim != nullptr ? FindTransformClip(*anim) : nullptr;
        return clip != nullptr && TransformKeyedAt(*clip, property, time);
    };
    const auto ensureAnim = [&]() -> TransformAnimatorComponent& {
        if (anim == nullptr)
        {
            TransformAnimatorComponent created;
            const NameComponent* name = registry.try_get<NameComponent>(entity);
            static_cast<void>(EnsureTransformClip(
                created, name != nullptr ? name->Name : std::string{"Transform"}));
            created.Playing = false; // authoring: don't auto-run while keying
            anim = &registry.emplace<TransformAnimatorComponent>(entity, std::move(created));
        }
        return *anim;
    };
    const auto keyProperty = [&](const AnimationChannel::Property property) {
        scene.BeginEditTransaction();
        TransformAnimatorComponent& target = ensureAnim();
        KeyTransformProperty(target, registry.get<TransformComponent>(entity), property, target.CurrentTime);
        scene.EndEditTransaction("Insert Transform Animation Key");
        ui.ShowFdxAnimation = true;
        ui.FocusFdxAnimation = true;
    };

    ImGui::TextDisabled("Key @ t=%.3f", static_cast<double>(time));
    ImGui::SameLine();
    const ImVec4 on{0.35F, 0.85F, 0.40F, 1.0F};
    const ImVec4 off{0.55F, 0.55F, 0.55F, 1.0F};
    ImGui::PushStyleColor(ImGuiCol_Text, keyed(AnimationChannel::Property::Translation) ? on : off);
    if (ImGui::SmallButton("Pos##keypos")) { keyProperty(AnimationChannel::Property::Translation); }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, keyed(AnimationChannel::Property::Rotation) ? on : off);
    if (ImGui::SmallButton("Rot##keyrot")) { keyProperty(AnimationChannel::Property::Rotation); }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, keyed(AnimationChannel::Property::Scale) ? on : off);
    if (ImGui::SmallButton("Scale##keyscl")) { keyProperty(AnimationChannel::Property::Scale); }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("All##keyall"))
    {
        keyProperty(AnimationChannel::Property::Translation);
        keyProperty(AnimationChannel::Property::Rotation);
        keyProperty(AnimationChannel::Property::Scale);
    }
    const AnimationClipAsset* activeClip = anim != nullptr ? FindTransformClip(*anim) : nullptr;
    if (activeClip != nullptr && !activeClip->Channels.empty())
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Open FDX##keyopen"))
        {
            ui.ShowFdxAnimation = true;
            ui.FocusFdxAnimation = true;
        }
    }
}

void DrawColor3(SceneEditor& scene, const char* label, const char* prefix)
{
    char idR[64];
    char idG[64];
    char idB[64];
    std::snprintf(idR, sizeof(idR), "%s-r", prefix);
    std::snprintf(idG, sizeof(idG), "%s-g", prefix);
    std::snprintf(idB, sizeof(idB), "%s-b", prefix);
    const auto r = scene.NumericFieldValue(idR);
    const auto g = scene.NumericFieldValue(idG);
    const auto b = scene.NumericFieldValue(idB);
    if (!r || !g || !b)
    {
        return;
    }
    float v[3] = {*r, *g, *b};
    const bool changed = ImGui::ColorEdit3(label, v, ImGuiColorEditFlags_Float);
    if (ImGui::IsItemActivated())
    {
        scene.BeginEditTransaction();
    }
    if (changed)
    {
        scene.ApplyNumericField(idR, v[0]);
        scene.ApplyNumericField(idG, v[1]);
        scene.ApplyNumericField(idB, v[2]);
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        scene.EndEditTransaction("Edit Component");
    }
    else if (ImGui::IsItemDeactivated())
    {
        scene.CancelEditTransaction();
    }
}

void DrawAssetSlot(
    const char* label,
    const AssetHandle& handle,
    const std::function<bool(AssetHandle)>& assign,
    const std::function<bool()>& clear,
    EditorUiState& ui)
{
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    char buf[64]{};
    if (handle.IsValid())
    {
        std::snprintf(buf, sizeof(buf), "%s", handle.ToString().c_str());
    }
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputText("##asset", buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FADIX_ASSET"))
        {
            if (const auto asset =
                    ParseAssetDragDropBlob(payload->Data, static_cast<std::size_t>(payload->DataSize)))
            {
                if (assign(asset->Handle))
                {
                    ui.StatusText = std::string{"Assigned "} + label;
                }
            }
            else if (payload->DataSize > 0)
            {
                // Legacy uuid-string payload.
                const char* text = static_cast<const char*>(payload->Data);
                if (const auto id = Uuid::Parse(text ? text : ""))
                {
                    if (assign(*id))
                    {
                        ui.StatusText = std::string{"Assigned "} + label;
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::Button("Clear"))
    {
        if (clear())
        {
            ui.StatusText = std::string{"Cleared "} + label;
        }
    }
    ImGui::PopID();
}

template <typename Fn>
void DrawBoolToggle(SceneEditor& scene, const char* label, bool& value, Fn&& /*unused*/)
{
    bool staged = value;
    if (ImGui::Checkbox(label, &staged))
    {
        scene.BeginEditTransaction();
        value = staged;
        scene.EndEditTransaction("Edit Component");
    }
}

void RemoveButton(SceneEditor& scene, const char* removeId, EditorUiState& ui)
{
    ImGui::PushID(removeId);
    if (ImGui::SmallButton("Remove"))
    {
        if (scene.RemoveComponent(removeId))
        {
            ui.StatusText = "Removed component";
        }
    }
    ImGui::PopID();
}
}

void InspectorPanel::Draw(SceneEditor& scene, EditorUiState& ui)
{
    if (!ui.ShowInspector)
    {
        return;
    }
    if (!ImGui::Begin(FADIX_ICON_SLIDERS " Inspector###Inspector", &ui.ShowInspector))
    {
        ImGui::End();
        return;
    }

    if (!scene.Selection())
    {
        ImGui::TextDisabled("No entity selected");
        ImGui::End();
        return;
    }
    const Uuid id = *scene.Selection();
    const auto entity = scene.World().Find(id);
    if (!entity)
    {
        scene.SetSelection(std::nullopt, false);
        ImGui::TextDisabled("No entity selected");
        ImGui::End();
        return;
    }

    auto& registry = scene.World().Registry();
    if (!m_NameBound || *m_NameBound != id)
    {
        m_NameBound = id;
        if (const auto* name = registry.try_get<NameComponent>(*entity))
        {
            std::snprintf(m_NameBuf, sizeof(m_NameBuf), "%s", name->Name.c_str());
        }
        else
        {
            std::snprintf(m_NameBuf, sizeof(m_NameBuf), "Entity");
        }
    }

    const bool isSceneRoot = scene.IsSceneRoot(id);
    ImGui::TextUnformatted(isSceneRoot ? "Scene" : "Entity");
    // On the scene root, its name IS the Hierarchy display name; the disk file keeps
    // its own path (renaming here does not rename the file).
    const char* nameLabel = isSceneRoot ? "Display Name" : "Name";
    if (ImGui::InputText(nameLabel, m_NameBuf, sizeof(m_NameBuf), ImGuiInputTextFlags_EnterReturnsTrue))
    {
        scene.RenameSelection(m_NameBuf);
        ui.StatusText = "Renamed entity";
    }
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        scene.RenameSelection(m_NameBuf);
    }
    if (isSceneRoot)
    {
        ImGui::TextDisabled("File: %s.scene", ui.SceneName.c_str());
    }

    if (registry.all_of<TransformComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawVec3(scene, "Position", "position");
            DrawVec3(scene, "Rotation", "rotation");
            DrawVec3(scene, "Scale", "scale");
            DrawTransformKeys(scene, ui, registry, *entity);
        }
    }

    if (MeshComponent* mesh = registry.try_get<MeshComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const char* kinds[] = {"Cube", "Sphere", "Plane", "Cylinder", "Capsule"};
            int kind = static_cast<int>(SanitizeMeshKind(static_cast<unsigned>(mesh->Kind)));
            if (ImGui::Combo("Primitive", &kind, kinds, IM_ARRAYSIZE(kinds)))
            {
                scene.BeginEditTransaction();
                mesh->Kind = static_cast<MeshKind>(kind);
                scene.EndEditTransaction("Change Mesh Primitive");
            }
            DrawAssetSlot(
                "Imported Mesh",
                mesh->ImportedMesh,
                [&](AssetHandle h) { return scene.AssignImportedMesh(h); },
                [&] { return scene.ClearImportedMesh(); },
                ui);
            DrawAssetSlot(
                "Material",
                mesh->Material,
                [&](AssetHandle h) { return scene.AssignMaterial(h); },
                [&] { return scene.ClearMaterial(); },
                ui);
            if (!mesh->Material.IsValid())
            {
                DrawColor3(scene, "Base Color", "mesh-color");
                DrawFloat(scene, "Metallic", "mesh-metallic");
                DrawFloat(scene, "Roughness", "mesh-roughness");
            }
            RemoveButton(scene, "remove-mesh", ui);
        }
    }

    if (DirectionalLightComponent* light = registry.try_get<DirectionalLightComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen))
        {
            static constexpr const char* kCelestialTypes[] = {"Directional", "Sun", "Moon"};
            int celestial = static_cast<int>(light->CelestialBody);
            if (ImGui::Combo("Celestial Body", &celestial, kCelestialTypes,
                    IM_ARRAYSIZE(kCelestialTypes)))
            {
                scene.BeginEditTransaction();
                light->CelestialBody =
                    SanitizeCelestialLightType(static_cast<unsigned>(celestial));
                if (light->CelestialBody != CelestialLightType::Moon)
                {
                    light->LinkOppositeSun = false;
                }
                scene.EndEditTransaction("Change Celestial Light Type");
            }
            if (light->CelestialBody == CelestialLightType::Moon)
            {
                DrawBoolToggle(scene, "Opposite Sun", light->LinkOppositeSun, [] {});
                if (light->LinkOppositeSun)
                {
                    ImGui::TextDisabled("Direction follows the opposite side of the Sun");
                }
            }
            DrawColor3(scene, "Color", "light-color");
            DrawFloat(scene, "Intensity", "light-intensity");
            DrawBoolToggle(scene, "Cast Shadows", light->CastShadows, [] {});
            DrawFloat(scene, "Shadow Bias", "light-shadow-bias");
            DrawFloat(scene, "Shadow Strength", "light-shadow-strength");
            DrawFloat(scene, "Shadow Distance", "light-shadow-distance");
            DrawFloat(scene, "Cascade Lambda", "light-cascade-lambda");
            DrawFloat(scene, "Shadow Softness", "light-shadow-softness");
            RemoveButton(scene, "remove-dirlight", ui);
        }
    }

    if (PointLightComponent* light = registry.try_get<PointLightComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Point Light", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawColor3(scene, "Color", "pointlight-color");
            DrawFloat(scene, "Intensity", "pointlight-intensity");
            DrawFloat(scene, "Range", "pointlight-range");
            DrawFloat(scene, "Falloff", "pointlight-falloff");
            DrawBoolToggle(scene, "Enabled", light->Enabled, [] {});
            DrawBoolToggle(scene, "Cast Shadows", light->CastShadows, [] {});
            DrawFloat(scene, "Shadow Bias", "pointlight-shadow-bias");
            DrawFloat(scene, "Shadow Resolution", "pointlight-shadow-resolution");
            DrawFloat(scene, "Shadow Strength", "pointlight-shadow-strength");
            DrawFloat(scene, "Softness", "pointlight-softness");
            RemoveButton(scene, "remove-pointlight", ui);
        }
    }

    if (SpotLightComponent* light = registry.try_get<SpotLightComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Spot Light", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawColor3(scene, "Color", "spotlight-color");
            DrawFloat(scene, "Intensity", "spotlight-intensity");
            DrawFloat(scene, "Range", "spotlight-range");
            DrawFloat(scene, "Inner Cone", "spotlight-inner");
            DrawFloat(scene, "Outer Cone", "spotlight-outer");
            DrawFloat(scene, "Falloff", "spotlight-falloff");
            DrawBoolToggle(scene, "Enabled", light->Enabled, [] {});
            DrawBoolToggle(scene, "Cast Shadows", light->CastShadows, [] {});
            DrawFloat(scene, "Shadow Bias", "spotlight-shadow-bias");
            DrawFloat(scene, "Shadow Resolution", "spotlight-shadow-resolution");
            DrawFloat(scene, "Shadow Strength", "spotlight-shadow-strength");
            DrawFloat(scene, "Softness", "spotlight-softness");
            RemoveButton(scene, "remove-spotlight", ui);
        }
    }

    if (EnvironmentComponent* env = registry.try_get<EnvironmentComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Image-based lighting: HDR environment map + controls. Empty = the
            // procedural sky/hemisphere ambient below is the fallback.
            DrawAssetSlot(
                "Environment Map (.hdr)", env->EnvironmentMap,
                [&](AssetHandle h) {
                    scene.BeginEditTransaction();
                    env->EnvironmentMap = h;
                    scene.EndEditTransaction("Assign Environment Map");
                    return true;
                },
                [&]() {
                    scene.BeginEditTransaction();
                    env->EnvironmentMap = InvalidAssetHandle;
                    scene.EndEditTransaction("Clear Environment Map");
                    return true;
                },
                ui);
            DrawFloat(scene, "Env Rotation", "env-environment-rotation");
            DrawFloat(scene, "Env Intensity", "env-environment-intensity");
            DrawBoolToggle(scene, "Use Env As Sky", env->UseEnvironmentAsSky, [] {});
            // Screen-space ambient occlusion (affects ambient/IBL only).
            DrawBoolToggle(scene, "Ambient Occlusion", env->AoEnabled, [] {});
            DrawFloat(scene, "AO Radius", "env-ao-radius");
            DrawFloat(scene, "AO Intensity", "env-ao-intensity");
            DrawFloat(scene, "AO Power", "env-ao-power");
            DrawFloat(scene, "AO Bias", "env-ao-bias");
            DrawColor3(scene, "Zenith", "env-zenith");
            DrawColor3(scene, "Horizon", "env-horizon");
            DrawColor3(scene, "Ground", "env-ground");
            DrawColor3(scene, "Ambient", "env-ambient");
            DrawFloat(scene, "Ambient Intensity", "env-ambient-intensity");
            DrawFloat(scene, "Exposure", "env-exposure");
            DrawFloat(scene, "Exposure Comp (EV)", "env-exposure-ev");
            DrawFloat(scene, "WB Temperature (K)", "env-wb-temp");
            DrawFloat(scene, "WB Tint", "env-wb-tint");
            DrawFloat(scene, "Contrast", "env-contrast");
            DrawFloat(scene, "Saturation", "env-saturation");
            DrawFloat(scene, "Highlight Comp", "env-highlight-compression");
            if (ImGui::Button("Reset to Filmic Neutral"))
            {
                // One undoable transaction restoring the neutral HDR grade:
                // no cast, no crush, no clip; D65 white balance.
                scene.BeginEditTransaction();
                scene.ApplyNumericField("env-exposure", 1.0F);
                scene.ApplyNumericField("env-exposure-ev", 0.0F);
                scene.ApplyNumericField("env-wb-temp", 6500.0F);
                scene.ApplyNumericField("env-wb-tint", 0.0F);
                scene.ApplyNumericField("env-contrast", 1.0F);
                scene.ApplyNumericField("env-saturation", 1.0F);
                scene.ApplyNumericField("env-highlight-compression", 0.0F);
                scene.EndEditTransaction("Reset to Filmic Neutral");
            }
            DrawFloat(scene, "Fog Start", "env-fog-start");
            DrawFloat(scene, "Fog End", "env-fog-end");
            DrawFloat(scene, "Fog Density", "env-fog-density");
            DrawColor3(scene, "Fog Color", "env-fog-color");
            DrawFloat(scene, "Priority", "env-priority");
            RemoveButton(scene, "remove-environment", ui);
        }
    }

    if (VisibilityComponent* vis = registry.try_get<VisibilityComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Visibility", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawBoolToggle(scene, "Editor", vis->VisibleInEditor, [] {});
            DrawBoolToggle(scene, "Game", vis->VisibleInGame, [] {});
            DrawBoolToggle(scene, "Selectable", vis->Selectable, [] {});
            RemoveButton(scene, "remove-visibility", ui);
        }
    }

    if (registry.all_of<CameraComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawFloat(scene, "Field of View", "camera-fov");
            DrawFloat(scene, "Near", "camera-near");
            DrawFloat(scene, "Far", "camera-far");
            RemoveButton(scene, "remove-camera", ui);
        }
    }

    if (NetworkIdentityComponent* net = registry.try_get<NetworkIdentityComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Network Identity", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawFloat(scene, "Network Id", "network-id");
            DrawBoolToggle(scene, "Replicate", net->Replicate, [] {});
        }
    }

    if (CharacterControllerComponent* character =
            registry.try_get<CharacterControllerComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Character Controller (Jolt)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawClampedFloat(scene, "Radius", character->Radius, 0.05F, 10.0F);
            DrawClampedFloat(scene, "Height", character->Height,
                character->Radius * 2.0F + 0.02F, 20.0F);
            DrawClampedFloat(scene, "Step Height", character->StepHeight, 0.0F, character->Height);
            DrawClampedFloat(scene, "Max Slope", character->MaxSlopeDegrees, 0.0F, 89.0F, 0.5F);
            DrawClampedFloat(scene, "Move Speed", character->MoveSpeed, 0.0F, 100.0F);
            DrawClampedFloat(scene, "Jump Speed", character->JumpSpeed, 0.0F, 100.0F);
            DrawClampedFloat(scene, "Gravity", character->Gravity, -100.0F, 0.0F);
            DrawClampedFloat(scene, "Air Control", character->AirControl, 0.0F, 1.0F, 0.01F);
            DrawClampedFloat(scene, "Push Strength", character->PushStrength, 0.0F, 100000.0F, 1.0F);
            ImGui::TextDisabled(character->Grounded ? "Grounded" : "In air");
            ImGui::TextDisabled("Upright capsule with slopes, stairs and collision sliding");
            RemoveButton(scene, "remove-character", ui);
        }
    }

    if (JoltBodyComponent* body = registry.try_get<JoltBodyComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Collider 3D (Jolt)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            static constexpr const char* kShapes[] = {
                "Auto (match mesh)", "Box", "Sphere", "Capsule", "Cylinder", "Plane", "Mesh"};
            int shape = static_cast<int>(body->Shape);
            if (ImGui::Combo("Shape", &shape, kShapes, IM_ARRAYSIZE(kShapes)))
            {
                scene.BeginEditTransaction();
                body->Shape = SanitizeColliderShape3D(static_cast<unsigned>(shape));
                scene.EndEditTransaction("Change Collider Shape");
            }
            DrawBoolToggle(scene, "Auto Fit Mesh", body->AutoFit, [] {});
            ImGui::BeginDisabled(body->AutoFit);
            DrawVec3(scene, "Half Extent", "jolt-extent");
            ImGui::EndDisabled();
            DrawBoolToggle(scene, "Dynamic", body->Dynamic, [] {});
            ImGui::BeginDisabled(!body->Dynamic);
            DrawClampedFloat(scene, "Mass (kg)", body->Mass, 0.001F, 1000000.0F, 0.1F);
            ImGui::EndDisabled();
            DrawClampedFloat(scene, "Friction", body->Friction, 0.0F, 2.0F, 0.01F);
            ImGui::TextDisabled("0 = ice, 0.8 = normal, 1+ = rough");
            if (body->Shape == ColliderShape3D::Mesh || body->Shape == ColliderShape3D::Auto)
            {
                ImGui::TextDisabled(body->Dynamic
                        ? "Dynamic mesh collision uses a convex hull"
                        : "Static mesh collision uses exact triangles");
            }
            RemoveButton(scene, "remove-jolt", ui);
        }
    }

    if (Box2DBodyComponent* body = registry.try_get<Box2DBodyComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Box2D Body", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (const auto x = scene.NumericFieldValue("box2d-extent-x"))
            {
                float v[2] = {*x, scene.NumericFieldValue("box2d-extent-y").value_or(0.5F)};
                if (ImGui::DragFloat2("Half Extent", v, 0.01F))
                {
                    if (ImGui::IsItemActivated())
                    {
                        scene.BeginEditTransaction();
                    }
                    scene.ApplyNumericField("box2d-extent-x", v[0]);
                    scene.ApplyNumericField("box2d-extent-y", v[1]);
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    scene.EndEditTransaction("Edit Component");
                }
            }
            DrawBoolToggle(scene, "Dynamic", body->Dynamic, [] {});
            RemoveButton(scene, "remove-box2d", ui);
        }
    }

    if (ScriptComponent* script = registry.try_get<ScriptComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawBoolToggle(scene, "Enabled", script->Enabled, [] {});
            ImGui::Text("Scripts: %d", static_cast<int>(script->ScriptNames.size()));
            for (const auto& name : script->ScriptNames)
            {
                ImGui::BulletText("%s", name.c_str());
            }
            ImGui::TextUnformatted("Target");
            ImGui::SameLine();
            const std::string targetText =
                script->Target.IsValid() ? script->Target.ToString() : "(none)";
            ImGui::TextUnformatted(targetText.c_str());
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload("FADIX_ENTITY"))
                {
                    const char* text = static_cast<const char*>(payload->Data);
                    if (const auto ref = Uuid::Parse(text ? text : ""))
                    {
                        if (scene.AssignScriptTarget(*ref))
                        {
                            ui.StatusText = "Assigned script target";
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::Button("Clear Target"))
            {
                scene.ClearScriptTarget();
            }
            RemoveButton(scene, "remove-script", ui);
        }
    }

    if (ParticleEmitterComponent* emitter = registry.try_get<ParticleEmitterComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Particle Emitter", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawBoolToggle(scene, "Enabled", emitter->Enabled, [] {});
            DrawFloat(scene, "Emit Rate", "particle-emit-rate");
            DrawFloat(scene, "Lifetime Min", "particle-life-min");
            DrawFloat(scene, "Lifetime Max", "particle-life-max");
            DrawFloat(scene, "Speed Min", "particle-speed-min");
            DrawFloat(scene, "Speed Max", "particle-speed-max");
            RemoveButton(scene, "remove-particle", ui);
        }
    }

    if (AudioSourceComponent* audio = registry.try_get<AudioSourceComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Audio Source", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawAssetSlot(
                "Sound",
                audio->Sound,
                [&](AssetHandle h) { return scene.AssignSound(h); },
                [&] {
                    scene.BeginEditTransaction();
                    audio->Sound = {};
                    scene.EndEditTransaction("Clear Sound");
                    return true;
                },
                ui);
            DrawBoolToggle(scene, "Play On Start", audio->PlayOnStart, [] {});
            DrawBoolToggle(scene, "Loop", audio->Loop, [] {});
            DrawFloat(scene, "Volume", "audio-volume");
            RemoveButton(scene, "remove-audio-source", ui);
        }
    }

    if (AudioListenerComponent* listener = registry.try_get<AudioListenerComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Audio Listener", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawBoolToggle(scene, "Active", listener->Active, [] {});
            RemoveButton(scene, "remove-audio-listener", ui);
        }
    }

    if (UICanvasComponent* canvas = registry.try_get<UICanvasComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("UI Canvas", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawAssetSlot(
                "UI Asset",
                canvas->UIAsset,
                [&](AssetHandle h) { return scene.AssignUIAsset(h); },
                [&] {
                    scene.BeginEditTransaction();
                    canvas->UIAsset = {};
                    scene.EndEditTransaction("Clear UI");
                    return true;
                },
                ui);
            DrawAssetSlot(
                "Style",
                canvas->StyleAsset,
                [&](AssetHandle h) { return scene.AssignUIStyle(h); },
                [&] {
                    scene.BeginEditTransaction();
                    canvas->StyleAsset = {};
                    scene.EndEditTransaction("Clear UI Style");
                    return true;
                },
                ui);
            DrawBoolToggle(scene, "Render In Editor", canvas->RenderInEditor, [] {});
            DrawBoolToggle(scene, "Render In Game", canvas->RenderInGame, [] {});
            DrawFloat(scene, "Order", "ui-order");
            DrawFloat(scene, "Scale", "ui-scale");
            RemoveButton(scene, "remove-ui-canvas", ui);
        }
    }

    if (TerrainComponent* terrain = registry.try_get<TerrainComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawAssetSlot(
                "Heightmap",
                terrain->Heightmap,
                [&](AssetHandle h) { return scene.AssignTerrainHeightmap(h); },
                [&] {
                    scene.BeginEditTransaction();
                    terrain->Heightmap = {};
                    scene.EndEditTransaction("Clear Heightmap");
                    return true;
                },
                ui);
            RemoveButton(scene, "remove-terrain", ui);
        }
    }

    if (registry.all_of<SkeletonComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Skeleton", ImGuiTreeNodeFlags_DefaultOpen))
        {
            RemoveButton(scene, "remove-skeleton", ui);
        }
    }

    if (AnimatorComponent* animator = registry.try_get<AnimatorComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Animator", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Clip picker from the imported model's clips (see FDX Animation panel).
            const GltfMeshAsset* gltf = nullptr;
            if (const MeshComponent* mesh = registry.try_get<MeshComponent>(*entity);
                mesh != nullptr && mesh->ImportedMesh.IsValid() && scene.GltfMeshes() != nullptr)
            {
                gltf = scene.GltfMeshes()->Get(mesh->ImportedMesh);
            }
            if (gltf != nullptr && !gltf->Animations.empty())
            {
                const char* preview = animator->ClipName.empty()
                    ? gltf->Animations.front().Name.c_str()
                    : animator->ClipName.c_str();
                if (ImGui::BeginCombo("Clip", preview))
                {
                    for (const AnimationClipAsset& clip : gltf->Animations)
                    {
                        // ponytail: clip pick is a direct write, not an undo step.
                        if (ImGui::Selectable(clip.Name.c_str(), clip.Name == animator->ClipName))
                        {
                            animator->ClipName = clip.Name;
                            animator->CurrentTime = 0.0F;
                            scene.MarkChanged();
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            else
            {
                ImGui::TextColored(ImVec4{1.0F, 0.6F, 0.3F, 1.0F}, "%s",
                    gltf == nullptr
                        ? "No imported skinned mesh - playback will not run.\n"
                          "Assign a rigged .glb to Mesh -> Imported Mesh."
                        : "Imported mesh has 0 animation clips - playback will not run.");
            }
            ImGui::TextDisabled("Starts stopped. Call entity:playAnimation(...) from code.");
            DrawBoolToggle(scene, "Loop", animator->Loop, [] {});
            DrawFloat(scene, "Speed", "animator-speed");
            if (ImGui::Button("Open FDX Animation"))
            {
                ui.ShowFdxAnimation = true;
                ui.FocusFdxAnimation = true; // panel follows current selection == this entity
            }
            RemoveButton(scene, "remove-animator", ui);
        }
    }

    if (TransformAnimatorComponent* transformAnim =
            registry.try_get<TransformAnimatorComponent>(*entity))
    {
        if (ImGui::CollapsingHeader("Transform Animator", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const AnimationClipAsset* clip = FindTransformClip(*transformAnim);
            ImGui::TextDisabled("Clip: %s  |  %zu clips  |  %.3f s  |  %zu channels",
                clip != nullptr ? clip->Name.c_str() : "None", transformAnim->Clips.size(),
                clip != nullptr ? static_cast<double>(clip->Duration) : 0.0,
                clip != nullptr ? clip->Channels.size() : 0U);
            ImGui::TextDisabled("Starts stopped. Call entity:playAnimation() from code.");
            DrawBoolToggle(scene, "Loop##ta", transformAnim->Loop, [] {});
            ImGui::TextDisabled("Speed %.2f  time %.3f", static_cast<double>(transformAnim->Speed),
                static_cast<double>(transformAnim->CurrentTime));
            if (ImGui::Button("Open FDX Animation##ta"))
            {
                ui.ShowFdxAnimation = true;
                ui.FocusFdxAnimation = true;
            }
            RemoveButton(scene, "remove-transform-animator", ui);
        }
    }

    ImGui::Separator();
    if (ImGui::BeginCombo("Add Component", "Add Component..."))
    {
        struct Option
        {
            const char* Id;
            const char* Label;
            bool Missing;
        };
        const Option options[] = {
            {"add-component-mesh", "Mesh", !registry.all_of<MeshComponent>(*entity)},
            {"add-component-visibility",
             "Visibility",
             !registry.all_of<VisibilityComponent>(*entity)},
            {"add-component-light",
             "Directional Light",
             !registry.all_of<DirectionalLightComponent>(*entity)},
            {"add-component-pointlight",
             "Point Light",
             !registry.all_of<PointLightComponent>(*entity)},
            {"add-component-spotlight",
             "Spot Light",
             !registry.all_of<SpotLightComponent>(*entity)},
            {"add-component-environment",
             "Environment",
             !registry.all_of<EnvironmentComponent>(*entity)},
            {"add-component-camera", "Camera", !registry.all_of<CameraComponent>(*entity)},
            {"add-component-particle",
             "Particle Emitter",
             !registry.all_of<ParticleEmitterComponent>(*entity)},
            {"add-component-audio-source",
             "Audio Source",
             !registry.all_of<AudioSourceComponent>(*entity)},
            {"add-component-audio-listener",
             "Audio Listener",
             !registry.all_of<AudioListenerComponent>(*entity)},
            {"add-component-ui-canvas",
             "UI Canvas",
             !registry.all_of<UICanvasComponent>(*entity)},
            {"add-component-terrain", "Terrain", !registry.all_of<TerrainComponent>(*entity)},
            {"add-component-skeleton",
             "Skeleton",
             !registry.all_of<SkeletonComponent>(*entity)},
            {"add-component-animator",
             "Animator",
             !registry.all_of<AnimatorComponent>(*entity)},
            {"add-component-transform-animator",
             "Transform Animator",
             !registry.all_of<TransformAnimatorComponent>(*entity)},
            {"add-component-jolt", "Collider 3D (Jolt)", !registry.all_of<JoltBodyComponent>(*entity)},
            {"add-component-character",
             "Character Controller (Jolt)",
             !registry.all_of<CharacterControllerComponent>(*entity)},
            {"add-component-box2d", "Box2D Body", !registry.all_of<Box2DBodyComponent>(*entity)},
            {"add-component-network",
             "Network Identity",
             !registry.all_of<NetworkIdentityComponent>(*entity)},
            {"add-component-script", "Script", !registry.all_of<ScriptComponent>(*entity)},
        };
        for (const Option& option : options)
        {
            if (!option.Missing)
            {
                continue;
            }
            if (ImGui::Selectable(option.Label))
            {
                if (scene.AddComponent(option.Id))
                {
                    ui.StatusText = std::string{"Added "} + option.Label;
                }
            }
        }
        ImGui::EndCombo();
    }

    ImGui::End();
}
}
