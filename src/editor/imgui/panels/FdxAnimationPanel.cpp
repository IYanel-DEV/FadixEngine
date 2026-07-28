#include "editor/imgui/panels/FdxAnimationPanel.hpp"

#include "assets/GltfMeshCache.hpp"
#include "engine/animation/AnimationClip.hpp"
#include "engine/assets/GltfMeshAsset.hpp"
#include "engine/scene/IWorld.hpp"
#include "runtime/AnimationRuntime.hpp"
#include "runtime/Components.hpp"

#include <glm/gtc/quaternion.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace fadix::editor
{
namespace
{
const char* PropertyLabel(const AnimationChannel::Property p)
{
    switch (p)
    {
    case AnimationChannel::Property::Translation: return "T";
    case AnimationChannel::Property::Rotation: return "R";
    case AnimationChannel::Property::Scale: return "S";
    }
    return "?";
}

std::string JointName(const GltfMeshAsset& gltf, const int jointIndex)
{
    if (jointIndex >= 0 && jointIndex < static_cast<int>(gltf.Skeleton.Joints.size()))
    {
        const std::string& name = gltf.Skeleton.Joints[static_cast<std::size_t>(jointIndex)].Name;
        if (!name.empty())
        {
            return name;
        }
    }
    return "joint" + std::to_string(jointIndex);
}

// Sample the channel's own value at `time` so a new key captures the current pose.
glm::vec4 SampleChannelValue(const AnimationChannel& ch, const float time)
{
    glm::vec3 translation{0.0F};
    glm::vec3 scale{1.0F};
    glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    ch.Sample(time, translation, rotation, scale);
    switch (ch.Target)
    {
    case AnimationChannel::Property::Translation: return glm::vec4{translation, 0.0F};
    case AnimationChannel::Property::Rotation:
        return glm::vec4{rotation.x, rotation.y, rotation.z, rotation.w};
    case AnimationChannel::Property::Scale: return glm::vec4{scale, 0.0F};
    }
    return glm::vec4{0.0F};
}

void SortKeyframes(AnimationChannel& ch)
{
    std::sort(ch.Keyframes.begin(), ch.Keyframes.end(),
        [](const AnimationKeyframe& a, const AnimationKeyframe& b) { return a.Time < b.Time; });
}

void RecomputeDuration(AnimationClipAsset& clip)
{
    float duration = 0.0F;
    for (const AnimationChannel& ch : clip.Channels)
    {
        if (!ch.Keyframes.empty())
        {
            duration = std::max(duration, ch.Keyframes.back().Time);
        }
    }
    clip.Duration = duration;
}

std::filesystem::path ClipPath(const std::filesystem::path& projectRoot, const std::string& name)
{
    return projectRoot / "Animations" / (name + ".fdxanim");
}

// Boring line-based text format, matching the project's non-JSON scene style.
// Joint *name* is stored (last token, may contain spaces) and resolved on load, so
// clips survive skeleton reindexing.
bool SaveClip(const std::filesystem::path& projectRoot, const AnimationClipAsset& clip,
    const GltfMeshAsset& gltf)
{
    std::error_code ec;
    std::filesystem::create_directories(projectRoot / "Animations", ec);
    std::ofstream out{ClipPath(projectRoot, clip.Name)};
    if (!out)
    {
        return false;
    }
    out << "fdxanim 1\n";
    out << "name " << clip.Name << "\n";
    out << "duration " << clip.Duration << "\n";
    for (const AnimationChannel& ch : clip.Channels)
    {
        out << "channel " << PropertyLabel(ch.Target) << ' ' << ch.Keyframes.size() << ' '
            << JointName(gltf, ch.JointIndex) << "\n";
        for (const AnimationKeyframe& k : ch.Keyframes)
        {
            out << "key " << k.Time << ' ' << k.Value.x << ' ' << k.Value.y << ' ' << k.Value.z
                << ' ' << k.Value.w << "\n";
        }
    }
    return true;
}

std::optional<AnimationClipAsset> LoadClip(
    const std::filesystem::path& projectRoot, const std::string& name, const GltfMeshAsset& gltf)
{
    std::ifstream in{ClipPath(projectRoot, name)};
    if (!in)
    {
        return std::nullopt;
    }
    AnimationClipAsset clip;
    clip.Name = name;
    AnimationChannel* current = nullptr;
    std::string line;
    while (std::getline(in, line))
    {
        std::istringstream ss{line};
        std::string tag;
        ss >> tag;
        if (tag == "name")
        {
            std::getline(ss >> std::ws, clip.Name);
        }
        else if (tag == "duration")
        {
            ss >> clip.Duration;
        }
        else if (tag == "channel")
        {
            std::string prop;
            std::size_t count = 0;
            ss >> prop >> count;
            std::string jointName;
            std::getline(ss >> std::ws, jointName);
            AnimationChannel ch;
            ch.Target = prop == "R" ? AnimationChannel::Property::Rotation
                : prop == "S"      ? AnimationChannel::Property::Scale
                                   : AnimationChannel::Property::Translation;
            ch.JointIndex = -1;
            for (std::size_t i = 0; i < gltf.Skeleton.Joints.size(); ++i)
            {
                if (gltf.Skeleton.Joints[i].Name == jointName)
                {
                    ch.JointIndex = static_cast<int>(i);
                    break;
                }
            }
            clip.Channels.push_back(std::move(ch));
            current = &clip.Channels.back();
        }
        else if (tag == "key" && current != nullptr)
        {
            AnimationKeyframe k;
            ss >> k.Time >> k.Value.x >> k.Value.y >> k.Value.z >> k.Value.w;
            current->Keyframes.push_back(k);
        }
    }
    // Drop channels whose joint no longer exists so playback never indexes garbage.
    clip.Channels.erase(
        std::remove_if(clip.Channels.begin(), clip.Channels.end(),
            [](const AnimationChannel& ch) { return ch.JointIndex < 0; }),
        clip.Channels.end());
    return clip;
}

// Add or replace a clip in the model's pool by name, so it becomes selectable and
// plays through the normal runtime path.
AnimationClipAsset& MergeClip(GltfMeshAsset& gltf, AnimationClipAsset clip)
{
    for (AnimationClipAsset& existing : gltf.Animations)
    {
        if (existing.Name == clip.Name)
        {
            existing = std::move(clip);
            return existing;
        }
    }
    gltf.Animations.push_back(std::move(clip));
    return gltf.Animations.back();
}

const char* TransformChannelLabel(const AnimationChannel::Property p)
{
    switch (p)
    {
    case AnimationChannel::Property::Translation: return "Position";
    case AnimationChannel::Property::Rotation: return "Rotation";
    case AnimationChannel::Property::Scale: return "Scale";
    }
    return "?";
}

// FDX Animation UI for an entity's own transform clip. Mirrors the skeletal transport
// + timeline + keyframe editing, but drives the entity's TransformComponent live so a
// bare cube (no skinned mesh) animates in Scene view while scrubbing.
// ponytail: dedicated animation preview viewport (own camera/framebuffer) is the P2
// upgrade; this reuses the shared Scene view for live pose feedback.
void DrawTransformSection(SceneEditor& scene, entt::registry& registry, const entt::entity entity,
    TransformAnimatorComponent& anim, int& selChannel, int& selKey)
{
    ImGui::Separator();
    ImGui::TextUnformatted("Transform Animation");
    TransformComponent* transformPtr = registry.try_get<TransformComponent>(entity);
    if (transformPtr == nullptr)
    {
        ImGui::TextDisabled("Entity has no Transform component.");
        return;
    }
    TransformComponent& transform = *transformPtr;
    AnimationClipAsset& clip = anim.Clip;

    // Transport.
    if (ImGui::Button(anim.Playing ? "Pause##t" : "Play##t"))
    {
        anim.Playing = !anim.Playing;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop##t"))
    {
        anim.Playing = false;
        anim.CurrentTime = 0.0F;
        ApplyTransformClip(clip, 0.0F, transform);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Loop##t", &anim.Loop);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0F);
    ImGui::DragFloat("Speed##t", &anim.Speed, 0.01F, 0.0F, 10.0F);

    // Timeline scrubber. Editing pauses playback and previews the pose immediately.
    float time = anim.CurrentTime;
    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::SliderFloat("##ttime", &time, 0.0F, std::max(clip.Duration, 0.001F), "t = %.3f"))
    {
        anim.CurrentTime = time;
        anim.Playing = false;
        ApplyTransformClip(clip, time, transform);
    }

    // Key the entity's current TRS at the scrub time.
    ImGui::TextUnformatted("Key:");
    ImGui::SameLine();
    if (ImGui::SmallButton("Pos##tk")) { KeyTransformProperty(anim, transform, AnimationChannel::Property::Translation, anim.CurrentTime); scene.MarkChanged(); }
    ImGui::SameLine();
    if (ImGui::SmallButton("Rot##tk")) { KeyTransformProperty(anim, transform, AnimationChannel::Property::Rotation, anim.CurrentTime); scene.MarkChanged(); }
    ImGui::SameLine();
    if (ImGui::SmallButton("Scale##tk")) { KeyTransformProperty(anim, transform, AnimationChannel::Property::Scale, anim.CurrentTime); scene.MarkChanged(); }

    ImGui::Columns(2, "tfxcols");
    ImGui::TextUnformatted("Channels");
    ImGui::BeginChild("tchannels", ImVec2{0.0F, 160.0F}, true);
    for (int i = 0; i < static_cast<int>(clip.Channels.size()); ++i)
    {
        const AnimationChannel& ch = clip.Channels[static_cast<std::size_t>(i)];
        char label[96];
        std::snprintf(label, sizeof(label), "%s (%zu keys)##tch%d",
            TransformChannelLabel(ch.Target), ch.Keyframes.size(), i);
        if (ImGui::Selectable(label, i == selChannel))
        {
            selChannel = i;
            selKey = -1;
        }
    }
    ImGui::EndChild();

    ImGui::NextColumn();
    ImGui::TextUnformatted("Keyframes");
    ImGui::BeginChild("tkeys", ImVec2{0.0F, 160.0F}, true);
    if (selChannel >= 0 && selChannel < static_cast<int>(clip.Channels.size()))
    {
        AnimationChannel& ch = clip.Channels[static_cast<std::size_t>(selChannel)];
        for (int i = 0; i < static_cast<int>(ch.Keyframes.size()); ++i)
        {
            char sel[32];
            std::snprintf(sel, sizeof(sel), "%.3f##tk%d",
                ch.Keyframes[static_cast<std::size_t>(i)].Time, i);
            if (ImGui::Selectable(sel, i == selKey))
            {
                selKey = i;
            }
        }
        if (selKey >= 0 && selKey < static_cast<int>(ch.Keyframes.size()))
        {
            AnimationKeyframe& key = ch.Keyframes[static_cast<std::size_t>(selKey)];
            ImGui::Separator();
            if (ImGui::DragFloat("Time##tv", &key.Time, 0.01F, 0.0F, 10000.0F))
            {
                SortKeyframes(ch);
                RecomputeDuration(clip);
                scene.MarkChanged();
                selKey = -1;
            }
            if (ch.Target == AnimationChannel::Property::Rotation)
            {
                if (ImGui::DragFloat4("Quat xyzw##tv", &key.Value.x, 0.01F))
                {
                    const glm::quat q = glm::normalize(
                        glm::quat{key.Value.w, key.Value.x, key.Value.y, key.Value.z});
                    key.Value = glm::vec4{q.x, q.y, q.z, q.w};
                    scene.MarkChanged();
                }
            }
            else if (ImGui::DragFloat3("Value##tv", &key.Value.x, 0.01F))
            {
                scene.MarkChanged();
            }
            if (ImGui::Button("Delete Key##tv"))
            {
                ch.Keyframes.erase(ch.Keyframes.begin() + selKey);
                RecomputeDuration(clip);
                scene.MarkChanged();
                selKey = -1;
            }
        }
    }
    else
    {
        ImGui::TextDisabled("Select a channel");
    }
    ImGui::EndChild();
    ImGui::Columns(1);
}
}

void FdxAnimationPanel::Draw(
    SceneEditor& scene, EditorUiState& ui, const std::filesystem::path& projectRoot)
{
    if (!ui.ShowFdxAnimation)
    {
        return;
    }
    if (!ImGui::Begin("FDX Animation###FDX Animation", &ui.ShowFdxAnimation))
    {
        ImGui::End();
        return;
    }

    IWorld& world = scene.World();
    entt::registry& registry = world.Registry();

    // Inspector's "Open FDX Animation" raises this so the tab pops to the front.
    if (ui.FocusFdxAnimation)
    {
        ImGui::SetWindowFocus();
        ui.FocusFdxAnimation = false;
    }

    const Uuid target = (m_Pinned && m_Pinned->IsValid())
        ? *m_Pinned
        : scene.Selection().value_or(Uuid{});
    const std::optional<entt::entity> entity =
        target.IsValid() ? world.Find(target) : std::nullopt;

    AnimatorComponent* animator = nullptr;
    GltfMeshAsset* gltf = nullptr;
    const MeshComponent* mesh = nullptr;
    bool hasSkeletonComp = false;
    std::string entityName = "(none)";
    if (entity)
    {
        animator = registry.try_get<AnimatorComponent>(*entity);
        hasSkeletonComp = registry.all_of<SkeletonComponent>(*entity);
        mesh = registry.try_get<MeshComponent>(*entity);
        if (const NameComponent* nameComp = registry.try_get<NameComponent>(*entity))
        {
            entityName = nameComp->Name;
        }
        if (mesh != nullptr && mesh->ImportedMesh.IsValid() && scene.GltfMeshes() != nullptr)
        {
            gltf = scene.GltfMeshes()->GetMutable(mesh->ImportedMesh);
        }
    }

    bool pinned = m_Pinned.has_value();
    if (ImGui::Checkbox("Pin", &pinned))
    {
        m_Pinned = pinned && target.IsValid() ? std::optional<Uuid>{target} : std::nullopt;
    }
    ImGui::SameLine();
    ImGui::Text("Target: %s", entityName.c_str());

    // Always-on readout once a target resolves, so state is never a mystery.
    if (entity)
    {
        const int clipCount = gltf != nullptr ? static_cast<int>(gltf->Animations.size()) : 0;
        const int jointCount =
            gltf != nullptr ? static_cast<int>(gltf->Skeleton.Joints.size()) : 0;
        const char* meshLabel = gltf != nullptr
            ? (gltf->DebugName.empty() ? "imported" : gltf->DebugName.c_str())
            : (mesh != nullptr && mesh->ImportedMesh.IsValid() ? "loading..." : "none");
        ImGui::TextDisabled("mesh: %s | clips: %d | joints: %d | skeleton: %s", meshLabel,
            clipCount, jointCount, hasSkeletonComp ? "yes" : "no");
    }
    ImGui::Separator();

    // One-click help shared by the empty states below.
    const auto drawAssignHelp = [&]() {
        if (ImGui::Button("Select this entity in Hierarchy"))
        {
            scene.SetSelection(target, true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Assign selected mesh to target"))
        {
            scene.SetSelection(target, false);
            if (!scene.AssignMeshFromSelection())
            {
                scene.Report("[FDX Animation] select an imported skinned mesh in the "
                             "Content Browser first");
            }
        }
    };

    if (!entity)
    {
        ImGui::TextWrapped("Select a skinned entity in the Hierarchy (or pin one).");
        ImGui::End();
        return;
    }
    TransformAnimatorComponent* transformAnim =
        registry.try_get<TransformAnimatorComponent>(*entity);
    const bool hasSkinnedModel = gltf != nullptr && gltf->HasSkeleton;
    const bool hasSkeletalClips = hasSkinnedModel && !gltf->Animations.empty();

    // No skeletal clips: offer/edit an entity-transform clip (works on a bare cube),
    // and still surface the skinned-import hint when relevant.
    if (!hasSkeletalClips)
    {
        if (transformAnim == nullptr)
        {
            ImGui::TextWrapped("%s",
                hasSkinnedModel
                    ? "This model imported with 0 animation clips. Animate this entity's own "
                      "transform below, or re-export from your DCC with clips."
                    : "This entity has no imported skinned mesh. Animate its own transform "
                      "(position / rotation / scale), or import a .glb/.gltf with skin + clips.");
            if (ImGui::Button("Add Transform Animation"))
            {
                TransformAnimatorComponent created;
                created.Clip.Name = entityName;
                created.Playing = false;
                transformAnim =
                    &registry.emplace<TransformAnimatorComponent>(*entity, std::move(created));
                scene.MarkChanged();
            }
            if (!hasSkinnedModel)
            {
                drawAssignHelp();
            }
        }
        if (transformAnim != nullptr)
        {
            DrawTransformSection(scene, registry, *entity, *transformAnim, m_TSelectedChannel,
                m_TSelectedKey);
        }
        ImGui::End();
        return;
    }
    // Skinned + has clips: make sure the Animator exists so scrubbing works even before
    // the viewport's per-frame attach runs (idempotent).
    if (animator == nullptr)
    {
        AttachImportedAnimation(registry, *entity, *gltf);
        animator = registry.try_get<AnimatorComponent>(*entity);
        if (animator == nullptr)
        {
            ImGui::TextWrapped("Could not attach an Animator to this entity.");
            ImGui::End();
            return;
        }
    }

    // Resolve the active clip (mutable) by animator clip name, default first.
    int clipIndex = 0;
    for (int i = 0; i < static_cast<int>(gltf->Animations.size()); ++i)
    {
        if (gltf->Animations[static_cast<std::size_t>(i)].Name == animator->ClipName)
        {
            clipIndex = i;
            break;
        }
    }
    if (ImGui::BeginCombo("Clip", gltf->Animations[static_cast<std::size_t>(clipIndex)].Name.c_str()))
    {
        for (int i = 0; i < static_cast<int>(gltf->Animations.size()); ++i)
        {
            const std::string& n = gltf->Animations[static_cast<std::size_t>(i)].Name;
            if (ImGui::Selectable(n.c_str(), i == clipIndex))
            {
                animator->ClipName = n;
                animator->CurrentTime = 0.0F;
                m_SelectedChannel = -1;
                m_SelectedKey = -1;
            }
        }
        ImGui::EndCombo();
    }
    AnimationClipAsset& clip = gltf->Animations[static_cast<std::size_t>(clipIndex)];

    // Transport.
    if (ImGui::Button(animator->Playing ? "Pause" : "Play"))
    {
        animator->Playing = !animator->Playing;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop"))
    {
        animator->Playing = false;
        animator->CurrentTime = 0.0F;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &animator->Loop);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0F);
    ImGui::DragFloat("Speed", &animator->Speed, 0.01F, 0.0F, 10.0F);

    // Timeline scrubber. Editing pauses playback so the scrub sticks.
    float time = animator->CurrentTime;
    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::SliderFloat("##fdxtime", &time, 0.0F, std::max(clip.Duration, 0.001F), "t = %.3f"))
    {
        animator->CurrentTime = time;
        animator->Playing = false;
    }
    const ImVec2 barMin = ImGui::GetItemRectMin();
    const ImVec2 barMax = ImGui::GetItemRectMax();

    if (clip.Duration > 0.0F)
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        // Keyframe markers for the selected channel drawn over the scrubber track.
        if (m_SelectedChannel >= 0 && m_SelectedChannel < static_cast<int>(clip.Channels.size()))
        {
            for (const AnimationKeyframe& k :
                clip.Channels[static_cast<std::size_t>(m_SelectedChannel)].Keyframes)
            {
                const float u = std::clamp(k.Time / clip.Duration, 0.0F, 1.0F);
                const float x = barMin.x + u * (barMax.x - barMin.x);
                draw->AddLine({x, barMin.y}, {x, barMax.y}, IM_COL32(255, 200, 60, 255), 2.0F);
            }
        }
        // Playhead at the current time.
        const float pu = std::clamp(animator->CurrentTime / clip.Duration, 0.0F, 1.0F);
        const float px = barMin.x + pu * (barMax.x - barMin.x);
        draw->AddLine({px, barMin.y}, {px, barMax.y}, IM_COL32(90, 200, 255, 255), 2.0F);
    }

    // Current-time readout: mm:ss.ms and seconds / duration.
    const int minutes = static_cast<int>(animator->CurrentTime) / 60;
    const float seconds = animator->CurrentTime - static_cast<float>(minutes * 60);
    ImGui::Text("%02d:%06.3f  (%.3f / %.3f s)", minutes, static_cast<double>(seconds),
        static_cast<double>(animator->CurrentTime), static_cast<double>(clip.Duration));

    ImGui::Separator();
    ImGui::Columns(2, "fdxcols");

    // Channel list.
    ImGui::TextUnformatted("Channels");
    ImGui::BeginChild("channels", ImVec2{0.0F, 200.0F}, true);
    for (int i = 0; i < static_cast<int>(clip.Channels.size()); ++i)
    {
        const AnimationChannel& ch = clip.Channels[static_cast<std::size_t>(i)];
        char label[160];
        std::snprintf(label, sizeof(label), "%s [%s] (%zu keys)##ch%d",
            JointName(*gltf, ch.JointIndex).c_str(), PropertyLabel(ch.Target),
            ch.Keyframes.size(), i);
        if (ImGui::Selectable(label, i == m_SelectedChannel))
        {
            m_SelectedChannel = i;
            m_SelectedKey = -1;
        }
    }
    ImGui::EndChild();

    ImGui::NextColumn();

    // Keyframe editing for the selected channel.
    ImGui::TextUnformatted("Keyframes");
    ImGui::BeginChild("keys", ImVec2{0.0F, 200.0F}, true);
    if (m_SelectedChannel >= 0 && m_SelectedChannel < static_cast<int>(clip.Channels.size()))
    {
        AnimationChannel& ch = clip.Channels[static_cast<std::size_t>(m_SelectedChannel)];
        if (ImGui::Button("Add Key @ current time"))
        {
            AnimationKeyframe k;
            k.Time = animator->CurrentTime;
            k.Value = SampleChannelValue(ch, animator->CurrentTime);
            ch.Keyframes.push_back(k);
            SortKeyframes(ch);
            RecomputeDuration(clip);
            scene.MarkChanged();
        }
        for (int i = 0; i < static_cast<int>(ch.Keyframes.size()); ++i)
        {
            char sel[32];
            std::snprintf(sel, sizeof(sel), "%.3f##k%d", ch.Keyframes[static_cast<std::size_t>(i)].Time, i);
            if (ImGui::Selectable(sel, i == m_SelectedKey))
            {
                m_SelectedKey = i;
            }
        }
        if (m_SelectedKey >= 0 && m_SelectedKey < static_cast<int>(ch.Keyframes.size()))
        {
            AnimationKeyframe& key = ch.Keyframes[static_cast<std::size_t>(m_SelectedKey)];
            ImGui::Separator();
            if (ImGui::DragFloat("Time", &key.Time, 0.01F, 0.0F, 10000.0F))
            {
                SortKeyframes(ch);
                RecomputeDuration(clip);
                scene.MarkChanged();
                m_SelectedKey = -1; // indices moved after re-sort
            }
            if (ch.Target == AnimationChannel::Property::Rotation)
            {
                if (ImGui::DragFloat4("Quat xyzw", &key.Value.x, 0.01F))
                {
                    const glm::quat q =
                        glm::normalize(glm::quat{key.Value.w, key.Value.x, key.Value.y, key.Value.z});
                    key.Value = glm::vec4{q.x, q.y, q.z, q.w};
                    scene.MarkChanged();
                }
            }
            else if (ImGui::DragFloat3("Value", &key.Value.x, 0.01F))
            {
                scene.MarkChanged();
            }
            if (ImGui::Button("Delete Key"))
            {
                ch.Keyframes.erase(ch.Keyframes.begin() + m_SelectedKey);
                RecomputeDuration(clip);
                scene.MarkChanged();
                m_SelectedKey = -1;
            }
        }
    }
    else
    {
        ImGui::TextDisabled("Select a channel");
    }
    ImGui::EndChild();

    ImGui::Columns(1);
    ImGui::Separator();

    // Persistence: round-trip the active clip to Animations/*.fdxanim.
    if (m_SaveName[0] == '\0')
    {
        std::snprintf(m_SaveName, sizeof(m_SaveName), "%s", clip.Name.c_str());
    }
    ImGui::SetNextItemWidth(200.0F);
    ImGui::InputText("File name", m_SaveName, sizeof(m_SaveName));
    ImGui::SameLine();
    if (ImGui::Button("Save"))
    {
        AnimationClipAsset saved = clip;
        saved.Name = m_SaveName;
        scene.Report(SaveClip(projectRoot, saved, *gltf)
                ? "[FDX Animation] Saved " + saved.Name + ".fdxanim"
                : "[FDX Animation] Save failed");
    }
    ImGui::SameLine();
    if (ImGui::Button("Load"))
    {
        if (auto loaded = LoadClip(projectRoot, m_SaveName, *gltf))
        {
            AnimationClipAsset& merged = MergeClip(*gltf, std::move(*loaded));
            animator->ClipName = merged.Name;
            animator->CurrentTime = 0.0F;
            m_SelectedChannel = -1;
            m_SelectedKey = -1;
            scene.MarkChanged();
            scene.Report("[FDX Animation] Loaded " + merged.Name + ".fdxanim");
        }
        else
        {
            scene.Report("[FDX Animation] Load failed (no such .fdxanim)");
        }
    }

    // Coexistence: a skinned entity may also carry an entity-transform clip.
    if (transformAnim != nullptr)
    {
        DrawTransformSection(
            scene, registry, *entity, *transformAnim, m_TSelectedChannel, m_TSelectedKey);
    }

    ImGui::End();
}
}
