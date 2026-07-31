#include "editor/imgui/panels/FdxAnimationPanel.hpp"

#include "editor/imgui/EditorIcons.hpp"
#include "editor/imgui/panels/ViewportPanel.hpp"
#include "editor/scene/EntityTextIO.hpp"

#include "assets/AssetDatabase.hpp"
#include "assets/GltfMeshCache.hpp"
#include "engine/command/ICommand.hpp"
#include "engine/command/UndoStack.hpp"
#include "engine/animation/AnimationClip.hpp"
#include "engine/animation/AnimatorControllerIO.hpp"
#include "engine/assets/GltfMeshAsset.hpp"
#include "engine/scene/IWorld.hpp"
#include "runtime/AnimationRuntime.hpp"
#include "runtime/Components.hpp"

#include <glm/gtc/quaternion.hpp>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>

namespace fadix::editor
{
namespace
{
struct AnimationEditHooks
{
    std::function<void()> Begin;
    std::function<void(const char*)> End;
    std::function<void()> Cancel;
};

class AnimationClipEditCommand final : public ICommand
{
public:
    AnimationClipEditCommand(GltfMeshAsset& model, const int clipIndex,
        AnimationClipAsset before, AnimationClipAsset after, std::string name,
        std::function<void()> changed)
        : m_Model(&model), m_ClipIndex(clipIndex), m_Before(std::move(before)),
          m_After(std::move(after)), m_Name(std::move(name)), m_Changed(std::move(changed))
    {
    }

    void Execute() override
    {
        if (m_FirstExecute)
        {
            m_FirstExecute = false;
            return;
        }
        Apply(m_After);
    }
    void Undo() override { Apply(m_Before); }
    [[nodiscard]] std::string_view Name() const noexcept override { return m_Name; }

private:
    void Apply(const AnimationClipAsset& clip)
    {
        if (m_Model != nullptr && m_ClipIndex >= 0 &&
            m_ClipIndex < static_cast<int>(m_Model->Animations.size()))
        {
            if (m_Changed)
            {
                m_Changed();
            }
            m_Model->Animations[static_cast<std::size_t>(m_ClipIndex)] = clip;
        }
    }

    GltfMeshAsset* m_Model{};
    int m_ClipIndex{-1};
    AnimationClipAsset m_Before;
    AnimationClipAsset m_After;
    std::string m_Name;
    std::function<void()> m_Changed;
    bool m_FirstExecute{true};
};

class TransformAnimationEditCommand final : public ICommand
{
public:
    TransformAnimationEditCommand(IWorld& world, Uuid entity,
        TransformAnimatorComponent before, TransformAnimatorComponent after, std::string name)
        : m_World(&world), m_Entity(entity), m_Before(std::move(before)),
          m_After(std::move(after)), m_Name(std::move(name))
    {
    }

    void Execute() override
    {
        if (m_FirstExecute)
        {
            m_FirstExecute = false;
            return;
        }
        Apply(m_After);
    }
    void Undo() override { Apply(m_Before); }
    [[nodiscard]] std::string_view Name() const noexcept override { return m_Name; }

private:
    void Apply(const TransformAnimatorComponent& value)
    {
        if (m_World != nullptr)
        {
            if (const auto entity = m_World->Find(m_Entity))
            {
                m_World->Registry().emplace_or_replace<TransformAnimatorComponent>(*entity, value);
            }
        }
    }

    IWorld* m_World{};
    Uuid m_Entity;
    TransformAnimatorComponent m_Before;
    TransformAnimatorComponent m_After;
    std::string m_Name;
    bool m_FirstExecute{true};
};

bool AdvancePreview(AnimationClipAsset& clip, float& time, bool& playing,
    const bool loop, const float speed)
{
    if (!playing || clip.Duration <= 0.0F)
    {
        playing = playing && clip.Duration > 0.0F;
        return false;
    }

    time += ImGui::GetIO().DeltaTime * speed;
    if (loop)
    {
        time = std::fmod(time, clip.Duration);
    }
    else if (time >= clip.Duration)
    {
        time = clip.Duration;
        playing = false;
    }
    return true;
}

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

const char* InterpolationLabel(const AnimationChannel::Interpolation interpolation)
{
    switch (interpolation)
    {
    case AnimationChannel::Interpolation::Step: return "Step";
    case AnimationChannel::Interpolation::Smooth: return "Smooth";
    case AnimationChannel::Interpolation::Linear: break;
    }
    return "Linear";
}

[[nodiscard]] std::string UniqueTransformClipName(const TransformAnimatorComponent& animator,
    const std::string& requested, const AnimationClipAsset* ignored = nullptr)
{
    const std::string base = requested.empty() ? "Animation" : requested;
    const auto available = [&](const std::string& candidate) {
        return std::none_of(animator.Clips.begin(), animator.Clips.end(),
            [&](const AnimationClipAsset& clip) {
                return &clip != ignored && clip.Name == candidate;
            });
    };
    if (available(base))
    {
        return base;
    }
    for (int suffix = 2;; ++suffix)
    {
        std::string candidate = base + " " + std::to_string(suffix);
        if (available(candidate))
        {
            return candidate;
        }
    }
}

bool DrawInterpolationCombo(const AnimationChannel& channel, const char* id,
    AnimationChannel::Interpolation& requested)
{
    static constexpr std::array<const char*, 3> labels{"Step", "Linear", "Smooth"};
    int selected = static_cast<int>(channel.InterpolationMode);
    ImGui::SetNextItemWidth(110.0F);
    const bool changed =
        ImGui::Combo(id, &selected, labels.data(), static_cast<int>(labels.size()));
    if (changed)
    {
        requested = static_cast<AnimationChannel::Interpolation>(selected);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Interpolation between keys on this channel");
    }
    return changed;
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

void SortEvents(AnimationClipAsset& clip)
{
    std::stable_sort(clip.Events.begin(), clip.Events.end(),
        [](const AnimationEvent& a, const AnimationEvent& b) { return a.Time < b.Time; });
}

void RecomputeDuration(AnimationClipAsset& clip)
{
    float duration = 0.0F;
    for (const AnimationChannel& ch : clip.Channels)
    {
        for (const AnimationKeyframe& key : ch.Keyframes)
        {
            duration = std::max(duration, key.Time);
        }
    }
    for (const AnimationEvent& event : clip.Events)
    {
        duration = std::max(duration, event.Time);
    }
    clip.Duration = duration;
}

std::string SafeClipFileName(std::string name)
{
    for (char& character : name)
    {
        if (std::strchr("<>:\"/\\|?*", character) != nullptr ||
            static_cast<unsigned char>(character) < 32)
        {
            character = '_';
        }
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
    {
        name.pop_back();
    }
    return name.empty() ? "Animation" : name;
}

std::filesystem::path ClipPath(const std::filesystem::path& projectRoot, const std::string& name)
{
    return projectRoot / "Assets" / "Animations" / (SafeClipFileName(name) + ".fdxanim");
}

std::filesystem::path ControllerPath(
    const std::filesystem::path& projectRoot, const std::string& name)
{
    return projectRoot / "Assets" / "Animations" /
        (SafeClipFileName(name) + ".fdxcontroller");
}

bool SaveController(const std::filesystem::path& projectRoot,
    const AnimatorController& controller)
{
    std::error_code ec;
    const std::filesystem::path path = ControllerPath(projectRoot, controller.Name);
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        return false;
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    std::ofstream out{temporary, std::ios::trunc};
    if (!out)
    {
        return false;
    }
    out << "fdxcontroller 1 ";
    WriteAnimatorControllerData(out, controller);
    out << '\n';
    out.flush();
    if (!out.good())
    {
        return false;
    }
    out.close();
    return AtomicReplaceFile(temporary, path).IsOk();
}

std::optional<AnimatorController> LoadController(
    const std::filesystem::path& projectRoot, const std::string& name)
{
    std::ifstream in{ControllerPath(projectRoot, name)};
    std::string tag;
    int version = 0;
    AnimatorController controller;
    if (!(in >> tag >> version) || tag != "fdxcontroller" || version != 1 ||
        !ReadAnimatorControllerData(in, controller))
    {
        return std::nullopt;
    }
    return controller;
}

// Boring line-based text format, matching the project's non-JSON scene style.
// Joint *name* is stored (last token, may contain spaces) and resolved on load, so
// clips survive skeleton reindexing.
bool SaveClip(const std::filesystem::path& projectRoot, const AnimationClipAsset& clip,
    const GltfMeshAsset& gltf)
{
    std::error_code ec;
    const std::filesystem::path path = ClipPath(projectRoot, clip.Name);
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
    {
        return false;
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    std::filesystem::path backup = path;
    backup += ".bak";
    std::ofstream out{temporary, std::ios::trunc};
    if (!out)
    {
        return false;
    }
    out << "fdxanim 2\n";
    out << "name " << clip.Name << "\n";
    out << "duration " << clip.Duration << "\n";
    for (const AnimationEvent& event : clip.Events)
    {
        out << "event " << event.Time << ' ' << std::quoted(event.Name) << ' '
            << std::quoted(event.Payload) << "\n";
    }
    for (const AnimationChannel& ch : clip.Channels)
    {
        out << "channel " << PropertyLabel(ch.Target) << ' ' << ch.Keyframes.size() << ' '
            << JointName(gltf, ch.JointIndex) << "\n";
        out << "interpolation " << InterpolationLabel(ch.InterpolationMode) << "\n";
        for (const AnimationKeyframe& k : ch.Keyframes)
        {
            out << "key " << k.Time << ' ' << k.Value.x << ' ' << k.Value.y << ' ' << k.Value.z
                << ' ' << k.Value.w << "\n";
        }
    }
    out.flush();
    if (!out.good())
    {
        return false;
    }
    out.close();

    std::filesystem::remove(backup, ec);
    ec.clear();
    const bool hadExisting = std::filesystem::exists(path, ec) && !ec;
    if (hadExisting)
    {
        std::filesystem::rename(path, backup, ec);
        if (ec)
        {
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec)
    {
        if (hadExisting)
        {
            std::error_code restoreError;
            std::filesystem::rename(backup, path, restoreError);
        }
        return false;
    }
    std::filesystem::remove(backup, ec);
    return true;
}

std::optional<AnimationClipAsset> LoadClip(
    const std::filesystem::path& projectRoot, const std::string& name, const GltfMeshAsset& gltf)
{
    std::ifstream in{ClipPath(projectRoot, name)};
    if (!in)
    {
        std::filesystem::path backup = ClipPath(projectRoot, name);
        backup += ".bak";
        in.clear();
        in.open(backup);
    }
    if (!in)
    {
        // Compatibility with animations saved before they became first-class assets.
        in.clear();
        in.open(projectRoot / "Animations" / (SafeClipFileName(name) + ".fdxanim"));
        if (!in)
        {
            return std::nullopt;
        }
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
        else if (tag == "event")
        {
            AnimationEvent event;
            ss >> event.Time >> std::quoted(event.Name) >> std::quoted(event.Payload);
            if (ss && !event.Name.empty())
            {
                clip.Events.push_back(std::move(event));
            }
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
        else if (tag == "interpolation" && current != nullptr)
        {
            std::string mode;
            ss >> mode;
            current->InterpolationMode = mode == "Step" ? AnimationChannel::Interpolation::Step
                : mode == "Smooth" ? AnimationChannel::Interpolation::Smooth
                                     : AnimationChannel::Interpolation::Linear;
        }
    }
    // Drop channels whose joint no longer exists so playback never indexes garbage.
    clip.Channels.erase(
        std::remove_if(clip.Channels.begin(), clip.Channels.end(),
            [](const AnimationChannel& ch) { return ch.JointIndex < 0; }),
        clip.Channels.end());
    std::sort(clip.Events.begin(), clip.Events.end(),
        [](const AnimationEvent& a, const AnimationEvent& b) { return a.Time < b.Time; });
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

float PreviousKeyTime(const AnimationClipAsset& clip, const float time)
{
    float result = 0.0F;
    for (const AnimationChannel& channel : clip.Channels)
    {
        for (const AnimationKeyframe& key : channel.Keyframes)
        {
            if (key.Time < time - 0.0001F)
            {
                result = std::max(result, key.Time);
            }
        }
    }
    return result;
}

float NextKeyTime(const AnimationClipAsset& clip, const float time)
{
    float result = clip.Duration;
    bool found = false;
    for (const AnimationChannel& channel : clip.Channels)
    {
        for (const AnimationKeyframe& key : channel.Keyframes)
        {
            if (key.Time > time + 0.0001F && (!found || key.Time < result))
            {
                result = key.Time;
                found = true;
            }
        }
    }
    return found ? result : time;
}

float SnapTime(const float time, const int framesPerSecond, const bool enabled)
{
    if (!enabled || framesPerSecond <= 0)
    {
        return std::max(time, 0.0F);
    }
    return std::max(std::round(time * static_cast<float>(framesPerSecond)) /
            static_cast<float>(framesPerSecond),
        0.0F);
}

bool TimelineButton(const char* label, const char* tooltip, const bool active = false)
{
    if (active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    const bool pressed = ImGui::Button(label, ImVec2{30.0F, 26.0F});
    if (active)
    {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", tooltip);
    }
    return pressed;
}

void InsertOrReplaceKey(AnimationChannel& channel, const float time)
{
    const glm::vec4 value = SampleChannelValue(channel, time);
    for (AnimationKeyframe& key : channel.Keyframes)
    {
        if (std::abs(key.Time - time) < 0.0001F)
        {
            key.Value = value;
            return;
        }
    }
    channel.Keyframes.push_back({time, value});
    SortKeyframes(channel);
}

std::string UniqueEventName(const AnimationClipAsset& clip)
{
    const auto exists = [&](const std::string& name) {
        return std::any_of(clip.Events.begin(), clip.Events.end(),
            [&](const AnimationEvent& event) { return event.Name == name; });
    };
    if (!exists("Event"))
    {
        return "Event";
    }
    for (int suffix = 2;; ++suffix)
    {
        std::string name = "Event " + std::to_string(suffix);
        if (!exists(name))
        {
            return name;
        }
    }
}

bool DrawEventInspector(AnimationClipAsset& clip, int& selectedEvent, const int timelineId,
    const int framesPerSecond, const bool snapToFrames, const AnimationEditHooks& edits)
{
    if (selectedEvent < 0 || selectedEvent >= static_cast<int>(clip.Events.size()))
    {
        return false;
    }
    struct EditorState
    {
        std::string Clip;
        int Selected{-1};
        char Name[128]{};
        char Payload[256]{};
    };
    static std::array<EditorState, 3> states;
    EditorState& state = states[static_cast<std::size_t>(std::clamp(timelineId, 0, 2))];
    AnimationEvent& event = clip.Events[static_cast<std::size_t>(selectedEvent)];
    if (state.Clip != clip.Name || state.Selected != selectedEvent)
    {
        state.Clip = clip.Name;
        state.Selected = selectedEvent;
        std::snprintf(state.Name, sizeof(state.Name), "%s", event.Name.c_str());
        std::snprintf(state.Payload, sizeof(state.Payload), "%s", event.Payload.c_str());
    }

    ImGui::Separator();
    ImGui::PushID(timelineId == 1 ? "SkeletalEventInspector" : "TransformEventInspector");
    ImGui::BeginChild("EventInspector", ImVec2{0.0F, 126.0F}, true);
    ImGui::TextUnformatted(FADIX_ICON_COMMENT "  Animation Event");
    ImGui::TextDisabled("Name");
    ImGui::SameLine(78.0F);
    ImGui::SetNextItemWidth(-1.0F);
    const bool nameChanged = ImGui::InputText("##EventName", state.Name, sizeof(state.Name));
    if (ImGui::IsItemActivated()) edits.Begin();
    if (nameChanged) event.Name = state.Name;
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        if (event.Name.empty())
        {
            event.Name = "Event";
            std::snprintf(state.Name, sizeof(state.Name), "%s", event.Name.c_str());
        }
        edits.End("Rename Animation Event");
    }
    else if (ImGui::IsItemDeactivated()) edits.Cancel();

    ImGui::TextDisabled("Payload");
    ImGui::SameLine(78.0F);
    ImGui::SetNextItemWidth(-1.0F);
    const bool payloadChanged =
        ImGui::InputText("##EventPayload", state.Payload, sizeof(state.Payload));
    if (ImGui::IsItemActivated()) edits.Begin();
    if (payloadChanged) event.Payload = state.Payload;
    if (ImGui::IsItemDeactivatedAfterEdit()) edits.End("Edit Animation Event Payload");
    else if (ImGui::IsItemDeactivated()) edits.Cancel();

    ImGui::TextDisabled("Time");
    ImGui::SameLine(78.0F);
    ImGui::SetNextItemWidth(180.0F);
    if (ImGui::DragFloat("##EventTime", &event.Time,
            1.0F / static_cast<float>(std::max(framesPerSecond, 1)), 0.0F, 3600.0F, "%.3f s"))
    {
        event.Time = SnapTime(event.Time, framesPerSecond,
            snapToFrames && !ImGui::GetIO().KeyAlt);
        RecomputeDuration(clip);
    }
    if (ImGui::IsItemActivated()) edits.Begin();
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        const float movedTime = event.Time;
        const std::string movedName = event.Name;
        SortEvents(clip);
        selectedEvent = -1;
        for (int i = 0; i < static_cast<int>(clip.Events.size()); ++i)
        {
            const AnimationEvent& candidate = clip.Events[static_cast<std::size_t>(i)];
            if (candidate.Name == movedName && std::abs(candidate.Time - movedTime) < 0.0001F)
            {
                selectedEvent = i;
                break;
            }
        }
        edits.End("Move Animation Event");
    }
    else if (ImGui::IsItemDeactivated()) edits.Cancel();
    ImGui::SameLine();
    if (ImGui::Button(FADIX_ICON_TRASH "  Delete Event"))
    {
        edits.Begin();
        clip.Events.erase(clip.Events.begin() + selectedEvent);
        selectedEvent = -1;
        RecomputeDuration(clip);
        edits.End("Delete Animation Event");
    }
    ImGui::EndChild();
    ImGui::PopID();
    return selectedEvent >= 0;
}

template <typename ChannelLabel, typename Preview>
bool DrawDopeSheet(const char* id, const int timelineId, AnimationClipAsset& clip,
    float& currentTime, bool& playing, bool& loop, float& speed,
    int& selectedChannel, int& selectedKey, int& selectedEvent,
    int& framesPerSecond, bool& snapToFrames, float& pixelsPerSecond, float& startTime,
    float& endTime, int& dragTimeline, int& dragChannel, int& dragKey, float& dragStartTime,
    float& dragStartMouseX, const AnimationEditHooks& edits,
    ChannelLabel&& channelLabel, Preview&& preview)
{
    ImGui::PushID(id);
    const float frame = 1.0F / static_cast<float>(std::max(framesPerSecond, 1));
    endTime = std::max(endTime, std::max(clip.Duration, frame));
    const auto setTime = [&](const float requested) {
        currentTime = std::clamp(requested, 0.0F, endTime);
        playing = false;
        preview();
    };
    const bool shortcuts = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsAnyItemActive() && !ImGui::GetIO().WantTextInput;
    const bool leftPressed = shortcuts && ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
    const bool rightPressed = shortcuts && ImGui::IsKeyPressed(ImGuiKey_RightArrow);
    const bool previousKeyPressed = leftPressed && ImGui::GetIO().KeyShift;
    const bool nextKeyPressed = rightPressed && ImGui::GetIO().KeyShift;
    const bool previousFramePressed = leftPressed && !ImGui::GetIO().KeyShift;
    const bool nextFramePressed = rightPressed && !ImGui::GetIO().KeyShift;
    bool insertRequested = shortcuts && ImGui::IsKeyPressed(ImGuiKey_K);

    ImGui::BeginChild("Transport", ImVec2{0.0F, 64.0F}, true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (TimelineButton(FADIX_ICON_FIRST "##First", "First frame (Home)") ||
        (shortcuts && ImGui::IsKeyPressed(ImGuiKey_Home)))
    {
        setTime(0.0F);
    }
    ImGui::SameLine();
    if (TimelineButton(FADIX_ICON_PREVIOUS_KEY "##PreviousKey", "Previous key (Shift+Left)") ||
        previousKeyPressed)
    {
        setTime(PreviousKeyTime(clip, currentTime));
    }
    ImGui::SameLine();
    if (TimelineButton(FADIX_ICON_PREVIOUS_FRAME "##PreviousFrame", "Previous frame (Left)") ||
        previousFramePressed)
    {
        setTime(currentTime - frame);
    }
    ImGui::SameLine();
    if (TimelineButton(playing ? FADIX_ICON_PAUSE "##Pause" : FADIX_ICON_PLAY "##Play",
            playing ? "Pause (Space)" : "Play (Space)", playing) ||
        (shortcuts && ImGui::IsKeyPressed(ImGuiKey_Space)))
    {
        playing = !playing;
    }
    ImGui::SameLine();
    if (TimelineButton(FADIX_ICON_STOP "##Stop", "Stop and rewind (S)") ||
        (shortcuts && !ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)))
    {
        playing = false;
        currentTime = 0.0F;
        preview();
    }
    ImGui::SameLine();
    if (TimelineButton(FADIX_ICON_NEXT_FRAME "##NextFrame", "Next frame (Right)") ||
        nextFramePressed)
    {
        setTime(currentTime + frame);
    }
    ImGui::SameLine();
    if (TimelineButton(FADIX_ICON_STEP "##NextKey", "Next key (Shift+Right)") || nextKeyPressed)
    {
        setTime(NextKeyTime(clip, currentTime));
    }
    ImGui::SameLine();
    if (TimelineButton(FADIX_ICON_LAST "##Last", "Last frame (End)") ||
        (shortcuts && ImGui::IsKeyPressed(ImGuiKey_End)))
    {
        setTime(endTime);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(126.0F);
    float editedFrame = currentTime * static_cast<float>(framesPerSecond);
    if (ImGui::DragFloat("##CurrentTime", &editedFrame, 1.0F, 0.0F,
            endTime * static_cast<float>(framesPerSecond), "F %04.0f"))
    {
        const float requested = editedFrame / static_cast<float>(framesPerSecond);
        setTime(snapToFrames ? SnapTime(requested, framesPerSecond, true) : requested);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Current frame: %d\nTime: %.3f seconds",
            static_cast<int>(std::round(currentTime * static_cast<float>(framesPerSecond))),
            static_cast<double>(currentTime));
    }

    ImGui::Checkbox("Loop", &loop);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(88.0F);
    ImGui::DragFloat("Speed", &speed, 0.05F, 0.0F, 10.0F, "%.2fx");
    ImGui::SameLine();
    ImGui::Checkbox("Snap", &snapToFrames);
    ImGui::SameLine();
    static constexpr std::array<const char*, 3> fpsLabels{"24 fps", "30 fps", "60 fps"};
    int fpsIndex = framesPerSecond == 24 ? 0 : framesPerSecond == 60 ? 2 : 1;
    ImGui::SetNextItemWidth(76.0F);
    if (ImGui::Combo("##FPS", &fpsIndex, fpsLabels.data(), static_cast<int>(fpsLabels.size())))
    {
        framesPerSecond = fpsIndex == 0 ? 24 : fpsIndex == 2 ? 60 : 30;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(88.0F);
    if (ImGui::DragFloat("End", &endTime, frame, std::max(clip.Duration, frame), 3600.0F, "%.2fs"))
    {
        endTime = std::max(endTime, std::max(clip.Duration, frame));
        currentTime = std::min(currentTime, endTime);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(selectedChannel < 0 || selectedChannel >= static_cast<int>(clip.Channels.size()));
    if (ImGui::Button(FADIX_ICON_KEY "  Insert Key##Timeline", ImVec2{0.0F, 0.0F}))
    {
        insertRequested = true;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Insert or replace a key on the selected channel (K)");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(FADIX_ICON_PLUS "  Event##Timeline"))
    {
        edits.Begin();
        AnimationEvent event;
        event.Time = SnapTime(currentTime, framesPerSecond, snapToFrames);
        event.Name = UniqueEventName(clip);
        const std::string selectedName = event.Name;
        clip.Events.push_back(std::move(event));
        SortEvents(clip);
        selectedEvent = -1;
        for (int i = 0; i < static_cast<int>(clip.Events.size()); ++i)
        {
            if (clip.Events[static_cast<std::size_t>(i)].Name == selectedName)
            {
                selectedEvent = i;
                break;
            }
        }
        selectedChannel = -1;
        selectedKey = -1;
        RecomputeDuration(clip);
        edits.End("Add Animation Event");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add an event at the playhead");
    ImGui::EndChild();

    constexpr float labelWidth = 190.0F;
    constexpr float rulerHeight = 28.0F;
    constexpr float rowHeight = 26.0F;
    const float canvasHeight = std::clamp(
        rulerHeight + rowHeight * static_cast<float>(clip.Channels.size() + 1U),
        96.0F, 220.0F);
    ImGui::BeginChild("DopeSheet", ImVec2{0.0F, canvasHeight}, true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size{std::max(ImGui::GetContentRegionAvail().x, labelWidth + 100.0F), canvasHeight};
    ImGui::InvisibleButton("Canvas", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 end{origin.x + size.x, origin.y + size.y};
    const float timelineLeft = origin.x + labelWidth;
    const float timelineWidth = std::max(size.x - labelWidth, 1.0F);

    draw->AddRectFilled(origin, end, IM_COL32(28, 31, 38, 255));
    draw->AddRectFilled(origin, ImVec2{timelineLeft, end.y}, IM_COL32(34, 38, 46, 255));
    draw->AddRectFilled(ImVec2{timelineLeft, origin.y}, ImVec2{end.x, origin.y + rulerHeight},
        IM_COL32(24, 27, 33, 255));

    if (hovered && ImGui::GetIO().MouseWheel != 0.0F)
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl)
        {
            const float mouseTime = startTime +
                std::max(io.MousePos.x - timelineLeft, 0.0F) / pixelsPerSecond;
            pixelsPerSecond = std::clamp(
                pixelsPerSecond * std::pow(1.18F, io.MouseWheel), 35.0F, 1200.0F);
            startTime = mouseTime -
                std::max(io.MousePos.x - timelineLeft, 0.0F) / pixelsPerSecond;
        }
        else
        {
            startTime -= io.MouseWheel * 80.0F / pixelsPerSecond;
        }
    }
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
    {
        startTime -= ImGui::GetIO().MouseDelta.x / pixelsPerSecond;
    }
    const float visibleDuration = timelineWidth / pixelsPerSecond;
    startTime = std::clamp(startTime, 0.0F, std::max(endTime - visibleDuration, 0.0F));

    static constexpr std::array<float, 12> tickSteps{
        1.0F / 60.0F, 1.0F / 30.0F, 1.0F / 15.0F, 0.1F, 0.2F, 0.5F,
        1.0F, 2.0F, 5.0F, 10.0F, 30.0F, 60.0F};
    float tickStep = tickSteps.back();
    for (const float candidate : tickSteps)
    {
        if (candidate * pixelsPerSecond >= 58.0F)
        {
            tickStep = candidate;
            break;
        }
    }
    const float visibleEnd = startTime + visibleDuration;
    const float firstTick = std::floor(startTime / tickStep) * tickStep;
    for (float tick = firstTick; tick <= visibleEnd + tickStep; tick += tickStep)
    {
        const float x = timelineLeft + (tick - startTime) * pixelsPerSecond;
        if (x < timelineLeft - 1.0F || x > end.x + 1.0F)
        {
            continue;
        }
        draw->AddLine(ImVec2{x, origin.y + rulerHeight}, ImVec2{x, end.y},
            IM_COL32(58, 62, 72, 150));
        draw->AddLine(ImVec2{x, origin.y + rulerHeight - 7.0F},
            ImVec2{x, origin.y + rulerHeight}, IM_COL32(150, 155, 166, 255));
        char text[32];
        std::snprintf(text, sizeof(text), tickStep < 1.0F ? "%.2f" : "%.0f", tick);
        draw->AddText(ImVec2{x + 3.0F, origin.y + 5.0F}, IM_COL32(175, 180, 190, 255), text);
    }
    draw->AddText(ImVec2{origin.x + 8.0F, origin.y + 6.0F},
        IM_COL32(150, 155, 166, 255), "CHANNELS");

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool timelineHovered = hovered && mouse.x >= timelineLeft && mouse.x <= end.x;
    const int hoveredTrack = hovered && mouse.y >= origin.y + rulerHeight
        ? static_cast<int>((mouse.y - origin.y - rulerHeight) / rowHeight)
        : -1;
    bool keyHovered = false;

    const float eventTop = origin.y + rulerHeight;
    const bool eventRowSelected = selectedEvent >= 0;
    const bool eventRowHovered = hoveredTrack == 0;
    draw->AddRectFilled(ImVec2{origin.x, eventTop}, ImVec2{end.x, eventTop + rowHeight},
        eventRowSelected ? IM_COL32(58, 48, 68, 255)
                         : eventRowHovered ? IM_COL32(44, 40, 51, 255)
                                           : IM_COL32(31, 30, 37, 255));
    draw->AddRectFilled(ImVec2{origin.x, eventTop + 3.0F},
        ImVec2{origin.x + 3.0F, eventTop + rowHeight - 3.0F}, IM_COL32(208, 112, 255, 255), 1.5F);
    draw->AddText(ImVec2{origin.x + 9.0F, eventTop + 5.0F}, IM_COL32(190, 180, 205, 255),
        (std::string{"EVENTS   "} + std::to_string(clip.Events.size())).c_str());
    draw->AddLine(ImVec2{origin.x, eventTop + rowHeight}, ImVec2{end.x, eventTop + rowHeight},
        IM_COL32(48, 52, 61, 255));
    for (int eventIndex = 0; eventIndex < static_cast<int>(clip.Events.size()); ++eventIndex)
    {
        const AnimationEvent& event = clip.Events[static_cast<std::size_t>(eventIndex)];
        const float x = timelineLeft + (event.Time - startTime) * pixelsPerSecond;
        if (x < timelineLeft - 8.0F || x > end.x + 8.0F)
        {
            continue;
        }
        const float y = eventTop + rowHeight * 0.5F;
        const bool selected = eventIndex == selectedEvent;
        const bool markerHovered = eventRowHovered && std::abs(mouse.x - x) <= 9.0F;
        keyHovered = keyHovered || markerHovered;
        const ImU32 color = selected ? IM_COL32(255, 218, 92, 255)
            : markerHovered          ? IM_COL32(236, 156, 255, 255)
                                     : IM_COL32(208, 112, 255, 255);
        draw->AddCircleFilled(ImVec2{x, y}, selected ? 7.0F : 5.5F, color);
        draw->AddLine(ImVec2{x, eventTop + 3.0F}, ImVec2{x, eventTop + rowHeight - 3.0F}, color, 1.5F);
        if (markerHovered)
        {
            ImGui::SetTooltip("%s\n%s\n%.3f s", event.Name.c_str(), event.Payload.c_str(),
                static_cast<double>(event.Time));
        }
    }

    for (int channelIndex = 0; channelIndex < static_cast<int>(clip.Channels.size()); ++channelIndex)
    {
        const float top = origin.y + rulerHeight + rowHeight * static_cast<float>(channelIndex + 1);
        const bool selected = channelIndex == selectedChannel;
        const bool rowHovered = hoveredTrack == channelIndex + 1;
        draw->AddRectFilled(ImVec2{origin.x, top}, ImVec2{end.x, top + rowHeight},
            selected ? IM_COL32(45, 58, 74, 255)
                     : rowHovered ? IM_COL32(39, 44, 54, 255)
                     : (channelIndex % 2 == 0 ? IM_COL32(31, 34, 41, 255)
                                             : IM_COL32(27, 30, 37, 255)));
        draw->AddLine(ImVec2{origin.x, top + rowHeight}, ImVec2{end.x, top + rowHeight},
            IM_COL32(48, 52, 61, 255));
        const std::string label = channelLabel(channelIndex, clip.Channels[static_cast<std::size_t>(channelIndex)]);
        const AnimationChannel& channel = clip.Channels[static_cast<std::size_t>(channelIndex)];
        const ImU32 channelColor = channel.Target == AnimationChannel::Property::Translation
            ? IM_COL32(67, 170, 255, 255)
            : channel.Target == AnimationChannel::Property::Rotation
            ? IM_COL32(255, 165, 70, 255)
            : IM_COL32(92, 205, 130, 255);
        draw->AddRectFilled(ImVec2{origin.x, top + 3.0F}, ImVec2{origin.x + 3.0F, top + rowHeight - 3.0F},
            channelColor, 1.5F);
        draw->AddText(ImVec2{origin.x + 9.0F, top + 5.0F},
            selected ? IM_COL32(235, 240, 248, 255) : IM_COL32(175, 181, 191, 255),
            label.c_str());

        for (int keyIndex = 0; keyIndex < static_cast<int>(channel.Keyframes.size()); ++keyIndex)
        {
            const float x = timelineLeft +
                (channel.Keyframes[static_cast<std::size_t>(keyIndex)].Time - startTime) *
                    pixelsPerSecond;
            if (x < timelineLeft - 8.0F || x > end.x + 8.0F)
            {
                continue;
            }
            const float y = top + rowHeight * 0.5F;
            const bool keySelected = selected && keyIndex == selectedKey;
            const bool thisKeyHovered = rowHovered && std::abs(mouse.x - x) <= 9.0F &&
                std::abs(mouse.y - y) <= 10.0F;
            keyHovered = keyHovered || thisKeyHovered;
            const float radius = keySelected ? 7.0F : thisKeyHovered ? 6.5F : 5.5F;
            const std::array<ImVec2, 4> diamond{
                ImVec2{x, y - radius}, ImVec2{x + radius, y},
                ImVec2{x, y + radius}, ImVec2{x - radius, y}};
            draw->AddConvexPolyFilled(diamond.data(), static_cast<int>(diamond.size()),
                keySelected ? IM_COL32(255, 218, 92, 255)
                            : thisKeyHovered ? IM_COL32(255, 194, 82, 255) : channelColor);
            draw->AddPolyline(diamond.data(), static_cast<int>(diamond.size()),
                IM_COL32(30, 24, 14, 255), ImDrawFlags_Closed, 1.0F);
        }
    }

    if (timelineHovered)
    {
        const float hoverTime = std::clamp(
            startTime + (mouse.x - timelineLeft) / pixelsPerSecond, 0.0F,
            endTime);
        draw->AddLine(ImVec2{mouse.x, origin.y + rulerHeight}, ImVec2{mouse.x, end.y},
            IM_COL32(185, 195, 210, 135), 1.0F);
        char hoverLabel[64];
        std::snprintf(hoverLabel, sizeof(hoverLabel), "Frame %d   %.3fs",
            static_cast<int>(std::round(hoverTime * static_cast<float>(framesPerSecond))),
            static_cast<double>(hoverTime));
        const ImVec2 textSize = ImGui::CalcTextSize(hoverLabel);
        const float labelX = std::clamp(mouse.x - textSize.x * 0.5F,
            timelineLeft + 3.0F, end.x - textSize.x - 11.0F);
        draw->AddRectFilled(ImVec2{labelX - 5.0F, origin.y + 3.0F},
            ImVec2{labelX + textSize.x + 5.0F, origin.y + rulerHeight - 3.0F},
            IM_COL32(45, 54, 67, 245), 4.0F);
        draw->AddText(ImVec2{labelX, origin.y + 6.0F}, IM_COL32(224, 232, 244, 255), hoverLabel);
    }
    if (keyHovered)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    const float playheadX = timelineLeft + (currentTime - startTime) * pixelsPerSecond;
    if (playheadX >= timelineLeft && playheadX <= end.x)
    {
        draw->AddLine(ImVec2{playheadX, origin.y}, ImVec2{playheadX, end.y},
            IM_COL32(66, 190, 255, 255), 2.0F);
        const std::array<ImVec2, 3> marker{ImVec2{playheadX - 6.0F, origin.y},
            ImVec2{playheadX + 6.0F, origin.y}, ImVec2{playheadX, origin.y + 8.0F}};
        draw->AddConvexPolyFilled(marker.data(), static_cast<int>(marker.size()),
            IM_COL32(66, 190, 255, 255));
    }
    draw->AddLine(ImVec2{timelineLeft, origin.y}, ImVec2{timelineLeft, end.y},
        IM_COL32(72, 78, 90, 255));

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        bool hitKey = false;
        if (mouse.y >= origin.y + rulerHeight)
        {
            const int row = static_cast<int>((mouse.y - origin.y - rulerHeight) / rowHeight);
            if (row == 0)
            {
                selectedEvent = -1;
                selectedChannel = -1;
                selectedKey = -1;
                if (mouse.x >= timelineLeft)
                {
                    float bestDistance = 9.0F;
                    for (int eventIndex = 0; eventIndex < static_cast<int>(clip.Events.size()); ++eventIndex)
                    {
                        const float eventX = timelineLeft +
                            (clip.Events[static_cast<std::size_t>(eventIndex)].Time - startTime) *
                                pixelsPerSecond;
                        const float distance = std::abs(mouse.x - eventX);
                        if (distance <= bestDistance)
                        {
                            bestDistance = distance;
                            selectedEvent = eventIndex;
                        }
                    }
                    if (selectedEvent >= 0)
                    {
                        edits.Begin();
                        hitKey = true;
                        dragTimeline = timelineId;
                        dragChannel = -3;
                        dragKey = selectedEvent;
                        dragStartMouseX = mouse.x;
                        dragStartTime = clip.Events[static_cast<std::size_t>(selectedEvent)].Time;
                        setTime(dragStartTime);
                    }
                }
            }
            else if (row > 0 && row <= static_cast<int>(clip.Channels.size()))
            {
                const int channelIndex = row - 1;
                selectedEvent = -1;
                selectedChannel = channelIndex;
                selectedKey = -1;
                if (mouse.x >= timelineLeft)
                {
                    AnimationChannel& channel = clip.Channels[static_cast<std::size_t>(channelIndex)];
                    float bestDistance = 9.0F;
                    for (int keyIndex = 0; keyIndex < static_cast<int>(channel.Keyframes.size()); ++keyIndex)
                    {
                        const float keyX = timelineLeft +
                            (channel.Keyframes[static_cast<std::size_t>(keyIndex)].Time - startTime) *
                                pixelsPerSecond;
                        const float distance = std::abs(mouse.x - keyX);
                        if (distance <= bestDistance)
                        {
                            bestDistance = distance;
                            selectedKey = keyIndex;
                        }
                    }
                    if (selectedKey >= 0)
                    {
                        edits.Begin();
                        hitKey = true;
                        dragTimeline = timelineId;
                        dragChannel = channelIndex;
                        dragKey = selectedKey;
                        dragStartMouseX = mouse.x;
                        dragStartTime = channel.Keyframes[static_cast<std::size_t>(selectedKey)].Time;
                        setTime(dragStartTime);
                    }
                }
            }
        }
        // Empty channel rows only select/hover. The ruler is the explicit scrub zone,
        // so inspecting a row can never jump the blue playhead.
        if (!hitKey && mouse.y < origin.y + rulerHeight && mouse.x >= timelineLeft)
        {
            dragTimeline = timelineId;
            dragChannel = -2;
            dragKey = -1;
            dragStartMouseX = mouse.x;
            setTime(SnapTime(startTime + (mouse.x - timelineLeft) / pixelsPerSecond,
                framesPerSecond, snapToFrames));
        }
    }

    if (dragTimeline == timelineId && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const float mouseX = ImGui::GetIO().MousePos.x;
        if (dragChannel == -2)
        {
            setTime(SnapTime(startTime + (mouseX - timelineLeft) / pixelsPerSecond,
                framesPerSecond, snapToFrames));
        }
        else if (dragChannel >= 0 && dragChannel < static_cast<int>(clip.Channels.size()))
        {
            AnimationChannel& channel = clip.Channels[static_cast<std::size_t>(dragChannel)];
            if (dragKey >= 0 && dragKey < static_cast<int>(channel.Keyframes.size()))
            {
                const float moved = SnapTime(
                    dragStartTime + (mouseX - dragStartMouseX) / pixelsPerSecond,
                    framesPerSecond, snapToFrames && !ImGui::GetIO().KeyAlt);
                channel.Keyframes[static_cast<std::size_t>(dragKey)].Time = moved;
                selectedChannel = dragChannel;
                selectedKey = dragKey;
                currentTime = moved;
                playing = false;
                RecomputeDuration(clip);
                preview();
            }
        }
        else if (dragChannel == -3 && dragKey >= 0 &&
            dragKey < static_cast<int>(clip.Events.size()))
        {
            const float moved = SnapTime(
                dragStartTime + (mouseX - dragStartMouseX) / pixelsPerSecond,
                framesPerSecond, snapToFrames && !ImGui::GetIO().KeyAlt);
            clip.Events[static_cast<std::size_t>(dragKey)].Time = moved;
            selectedEvent = dragKey;
            currentTime = moved;
            playing = false;
            RecomputeDuration(clip);
            preview();
        }
    }
    if (dragTimeline == timelineId && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        const bool movedEvent = dragChannel == -3;
        const bool movedItem = movedEvent ||
            (dragChannel >= 0 && dragChannel < static_cast<int>(clip.Channels.size()));
        if (dragChannel >= 0 && dragChannel < static_cast<int>(clip.Channels.size()))
        {
            AnimationChannel& channel = clip.Channels[static_cast<std::size_t>(dragChannel)];
            const float movedTime = currentTime;
            SortKeyframes(channel);
            selectedChannel = dragChannel;
            selectedKey = -1;
            for (int i = 0; i < static_cast<int>(channel.Keyframes.size()); ++i)
            {
                if (std::abs(channel.Keyframes[static_cast<std::size_t>(i)].Time - movedTime) < 0.0001F)
                {
                    selectedKey = i;
                    break;
                }
            }
        }
        else if (dragChannel == -3 && dragKey >= 0 &&
            dragKey < static_cast<int>(clip.Events.size()))
        {
            const float movedTime = clip.Events[static_cast<std::size_t>(dragKey)].Time;
            const std::string movedName = clip.Events[static_cast<std::size_t>(dragKey)].Name;
            SortEvents(clip);
            selectedEvent = -1;
            for (int i = 0; i < static_cast<int>(clip.Events.size()); ++i)
            {
                const AnimationEvent& event = clip.Events[static_cast<std::size_t>(i)];
                if (event.Name == movedName && std::abs(event.Time - movedTime) < 0.0001F)
                {
                    selectedEvent = i;
                    break;
                }
            }
        }
        dragTimeline = 0;
        dragChannel = -1;
        dragKey = -1;
        if (movedItem)
        {
            edits.End(movedEvent ? "Move Animation Event" : "Move Animation Key");
        }
    }

    if ((hovered || shortcuts) && ImGui::IsKeyPressed(ImGuiKey_Delete) && selectedEvent >= 0 &&
        selectedEvent < static_cast<int>(clip.Events.size()))
    {
        edits.Begin();
        clip.Events.erase(clip.Events.begin() + selectedEvent);
        selectedEvent = -1;
        RecomputeDuration(clip);
        edits.End("Delete Animation Event");
    }
    else if ((hovered || shortcuts) && ImGui::IsKeyPressed(ImGuiKey_Delete) &&
        selectedChannel >= 0 && selectedChannel < static_cast<int>(clip.Channels.size()))
    {
        AnimationChannel& channel = clip.Channels[static_cast<std::size_t>(selectedChannel)];
        if (selectedKey >= 0 && selectedKey < static_cast<int>(channel.Keyframes.size()))
        {
            edits.Begin();
            channel.Keyframes.erase(channel.Keyframes.begin() + selectedKey);
            selectedKey = -1;
            RecomputeDuration(clip);
            preview();
            edits.End("Delete Animation Key");
        }
    }
    ImGui::EndChild();
    const float maxStart = std::max(endTime - visibleDuration, 0.0F);
    if (maxStart > 0.0F)
    {
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::SliderFloat("##TimelinePan", &startTime, 0.0F, maxStart, "", ImGuiSliderFlags_NoInput);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Pan left/right through the timeline");
        }
    }
    ImGui::TextDisabled(
        "MMB drag / wheel Pan  |  Ctrl+wheel Zoom  |  Space Play  |  K Insert  |  Del Remove");
    ImGui::PopID();
    return insertRequested && selectedChannel >= 0 &&
        selectedChannel < static_cast<int>(clip.Channels.size());
}

std::string UniqueAnimatorStateName(const AnimatorController& controller)
{
    for (int suffix = 1;; ++suffix)
    {
        const std::string candidate = "State " + std::to_string(suffix);
        if (std::none_of(controller.States.begin(), controller.States.end(),
                [&](const AnimatorState& state) { return state.Name == candidate; }))
        {
            return candidate;
        }
    }
}

std::string UniqueAnimatorParameterName(const AnimatorController& controller)
{
    for (int suffix = 1;; ++suffix)
    {
        const std::string candidate = "Parameter " + std::to_string(suffix);
        if (FindAnimatorParameter(controller, candidate) == nullptr)
        {
            return candidate;
        }
    }
}

template <typename Edit>
void CommitControllerEdit(SceneEditor& scene, const char* name, Edit&& edit)
{
    scene.BeginEditTransaction();
    edit();
    scene.EndEditTransaction(name);
}

void TrackControllerEditItem(SceneEditor& scene, const char* name)
{
    if (ImGui::IsItemActivated()) scene.BeginEditTransaction();
    if (ImGui::IsItemDeactivated()) scene.EndEditTransaction(name);
}

template <typename Animator>
void DrawAnimatorController(SceneEditor& scene, const char* id, Animator& animator,
    const std::vector<std::string>& clipNames, const std::filesystem::path& projectRoot,
    int& selectedState, int& selectedTransition, glm::vec2& graphPan,
    int& connectingFrom, ImVec2& connectingEnd)
{
    ImGui::PushID(id);
    AnimatorController& controller = animator.Controller;
    if (controller.Name.empty())
    {
        controller.Name = "Animator";
    }

    ImGui::SeparatorText(FADIX_ICON_SITEMAP "  Animator State Machine");
    ImGui::SetNextItemWidth(170.0F);
    char controllerName[128]{};
    std::snprintf(controllerName, sizeof(controllerName), "%s", controller.Name.c_str());
    if (ImGui::InputText("##ControllerName", controllerName, sizeof(controllerName)))
    {
        controller.Name = controllerName;
    }
    TrackControllerEditItem(scene, "Rename Animator Controller");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reusable controller asset name");
    ImGui::SameLine();
    if (ImGui::Button(FADIX_ICON_SAVE "  Save Controller"))
    {
        SaveController(projectRoot, controller);
    }
    ImGui::SameLine();
    if (ImGui::Button(FADIX_ICON_FOLDER_OPEN "  Load"))
    {
        if (std::optional<AnimatorController> loaded = LoadController(projectRoot, controller.Name))
        {
            CommitControllerEdit(scene, "Load Animator Controller", [&]() {
                controller = std::move(*loaded);
                animator.ClearControllerRuntime();
                animator.Playing = false;
                animator.Paused = false;
                selectedState = -1;
                selectedTransition = -1;
            });
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(FADIX_ICON_PLUS "  State"))
    {
        CommitControllerEdit(scene, "Add Animator State", [&]() {
            AnimatorState state;
            state.Name = UniqueAnimatorStateName(controller);
            state.ClipName = clipNames.empty() ? std::string{} : clipNames.front();
            const float offset = static_cast<float>(controller.States.size() % 4U) * 175.0F;
            const float row = static_cast<float>(controller.States.size() / 4U) * 90.0F;
            state.Position = {30.0F + offset, 35.0F + row};
            controller.States.push_back(std::move(state));
            selectedState = static_cast<int>(controller.States.size()) - 1;
            if (controller.EntryState.empty())
            {
                controller.EntryState = controller.States.back().Name;
            }
        });
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Starts stopped - call entity:startAnimator()");

    constexpr float panelHeight = 280.0F;
    constexpr float parameterWidth = 235.0F;
    ImGui::BeginChild("Parameters", ImVec2{parameterWidth, panelHeight}, true);
    ImGui::TextUnformatted("PARAMETERS");
    ImGui::SameLine();
    if (ImGui::SmallButton(FADIX_ICON_PLUS "##Parameter"))
    {
        CommitControllerEdit(scene, "Add Animator Parameter", [&]() {
            AnimatorParameter parameter;
            parameter.Name = UniqueAnimatorParameterName(controller);
            controller.Parameters.push_back(std::move(parameter));
        });
    }
    int removeParameter = -1;
    for (int index = 0; index < static_cast<int>(controller.Parameters.size()); ++index)
    {
        AnimatorParameter& parameter = controller.Parameters[static_cast<std::size_t>(index)];
        ImGui::PushID(index);
        char name[96]{};
        std::snprintf(name, sizeof(name), "%s", parameter.Name.c_str());
        ImGui::SetNextItemWidth(135.0F);
        if (ImGui::InputText("##Name", name, sizeof(name)))
        {
            const std::string oldName = parameter.Name;
            parameter.Name = name;
            for (AnimatorTransition& transition : controller.Transitions)
            {
                for (AnimatorCondition& condition : transition.Conditions)
                {
                    if (condition.Parameter == oldName) condition.Parameter = parameter.Name;
                }
            }
        }
        TrackControllerEditItem(scene, "Rename Animator Parameter");
        ImGui::SameLine();
        if (ImGui::SmallButton(FADIX_ICON_TRASH "##Remove")) removeParameter = index;
        static constexpr std::array<const char*, 4> typeLabels{"Bool", "Float", "Int", "Trigger"};
        int type = static_cast<int>(parameter.Type);
        ImGui::SetNextItemWidth(82.0F);
        if (ImGui::Combo("##Type", &type, typeLabels.data(), static_cast<int>(typeLabels.size())))
        {
            CommitControllerEdit(scene, "Change Animator Parameter Type", [&]() {
                parameter.Type = static_cast<AnimatorParameterType>(type);
            });
        }
        ImGui::SameLine();
        if (parameter.Type == AnimatorParameterType::Bool ||
            parameter.Type == AnimatorParameterType::Trigger)
        {
            bool value = parameter.BoolValue;
            if (ImGui::Checkbox("Value", &value))
            {
                CommitControllerEdit(scene, "Set Animator Parameter",
                    [&]() { parameter.BoolValue = value; });
            }
        }
        else if (parameter.Type == AnimatorParameterType::Float)
        {
            ImGui::SetNextItemWidth(112.0F);
            ImGui::DragFloat("##Value", &parameter.FloatValue, 0.05F);
            TrackControllerEditItem(scene, "Set Animator Parameter");
        }
        else
        {
            ImGui::SetNextItemWidth(112.0F);
            ImGui::DragInt("##Value", &parameter.IntValue, 1.0F);
            TrackControllerEditItem(scene, "Set Animator Parameter");
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    if (removeParameter >= 0)
    {
        CommitControllerEdit(scene, "Delete Animator Parameter", [&]() {
            const std::string removed =
                controller.Parameters[static_cast<std::size_t>(removeParameter)].Name;
            controller.Parameters.erase(controller.Parameters.begin() + removeParameter);
            for (AnimatorTransition& transition : controller.Transitions)
            {
                std::erase_if(transition.Conditions,
                    [&](const AnimatorCondition& condition) { return condition.Parameter == removed; });
            }
        });
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("Graph", ImVec2{0.0F, panelHeight}, true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("GraphCanvas", size,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
            ImGuiButtonFlags_AllowOverlap);
    const bool canvasHovered = ImGui::IsItemHovered();
    if (canvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
    {
        graphPan.x += ImGui::GetIO().MouseDelta.x;
        graphPan.y += ImGui::GetIO().MouseDelta.y;
    }
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && connectingFrom < 0)
    {
        selectedState = -1;
        selectedTransition = -1;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    constexpr ImVec2 nodeSize{150.0F, 54.0F};
    if (connectingFrom >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        int releaseTarget = -1;
        for (int ti = 0; ti < static_cast<int>(controller.States.size()); ++ti)
        {
            if (ti == connectingFrom) continue;
            const AnimatorState& tgt = controller.States[static_cast<std::size_t>(ti)];
            const ImVec2 tl{origin.x + graphPan.x + tgt.Position.x,
                origin.y + graphPan.y + tgt.Position.y};
            if (ImGui::IsMouseHoveringRect(tl,
                    ImVec2{tl.x + nodeSize.x, tl.y + nodeSize.y}))
            {
                releaseTarget = ti;
                break;
            }
        }
        if (releaseTarget >= 0)
        {
            const std::string fromName =
                controller.States[static_cast<std::size_t>(connectingFrom)].Name;
            const std::string toName =
                controller.States[static_cast<std::size_t>(releaseTarget)].Name;
            CommitControllerEdit(scene, "Add Animator Transition", [&]() {
                AnimatorTransition t;
                t.From = fromName;
                t.To = toName;
                t.HasExitTime = true;
                controller.Transitions.push_back(std::move(t));
                selectedTransition =
                    static_cast<int>(controller.Transitions.size()) - 1;
                selectedState = -1;
            });
        }
        connectingFrom = -1;
    }
    if (connectingFrom >= 0 && ImGui::IsKeyPressed(ImGuiKey_Escape))
        connectingFrom = -1;
    draw->PushClipRect(origin, ImVec2{origin.x + size.x, origin.y + size.y}, true);
    constexpr float grid = 32.0F;
    for (float x = std::fmod(graphPan.x, grid); x < size.x; x += grid)
    {
        draw->AddLine(ImVec2{origin.x + x, origin.y}, ImVec2{origin.x + x, origin.y + size.y},
            IM_COL32(48, 52, 61, 150));
    }
    for (float y = std::fmod(graphPan.y, grid); y < size.y; y += grid)
    {
        draw->AddLine(ImVec2{origin.x, origin.y + y}, ImVec2{origin.x + size.x, origin.y + y},
            IM_COL32(48, 52, 61, 150));
    }
    const auto stateCenter = [&](const std::string& name) {
        const AnimatorState* state = FindAnimatorState(controller, name);
        return state == nullptr ? origin : ImVec2{origin.x + graphPan.x + state->Position.x + 75.0F,
            origin.y + graphPan.y + state->Position.y + 27.0F};
    };
    for (int index = 0; index < static_cast<int>(controller.Transitions.size()); ++index)
    {
        const AnimatorTransition& transition = controller.Transitions[static_cast<std::size_t>(index)];
        if (FindAnimatorState(controller, transition.From) == nullptr ||
            FindAnimatorState(controller, transition.To) == nullptr)
        {
            continue;
        }
        const ImVec2 from = stateCenter(transition.From);
        const ImVec2 to = stateCenter(transition.To);
        const ImU32 color = index == selectedTransition ? IM_COL32(255, 208, 76, 255)
                                                       : IM_COL32(132, 145, 165, 255);
        draw->AddBezierCubic(from, ImVec2{from.x + 55.0F, from.y},
            ImVec2{to.x - 55.0F, to.y}, to, color, index == selectedTransition ? 3.0F : 2.0F);
        draw->AddCircleFilled(to, 4.0F, color);
        const ImVec2 midpoint{(from.x + to.x) * 0.5F, (from.y + to.y) * 0.5F};
        ImGui::SetCursorScreenPos(ImVec2{midpoint.x - 8.0F, midpoint.y - 8.0F});
        ImGui::PushID(index + 10000);
        ImGui::InvisibleButton("Transition", ImVec2{16.0F, 16.0F});
        if (ImGui::IsItemClicked())
        {
            selectedTransition = index;
            selectedState = -1;
        }
        ImGui::PopID();
    }
    for (int index = 0; index < static_cast<int>(controller.States.size()); ++index)
    {
        AnimatorState& state = controller.States[static_cast<std::size_t>(index)];
        const ImVec2 topLeft{origin.x + graphPan.x + state.Position.x,
            origin.y + graphPan.y + state.Position.y};
        ImGui::SetCursorScreenPos(topLeft);
        ImGui::PushID(index);
        ImGui::InvisibleButton("State", nodeSize);
        if (ImGui::IsItemActivated()) scene.BeginEditTransaction();
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            state.Position.x += ImGui::GetIO().MouseDelta.x;
            state.Position.y += ImGui::GetIO().MouseDelta.y;
        }
        if (ImGui::IsItemDeactivated()) scene.EndEditTransaction("Move Animator State");
        if (ImGui::IsItemClicked())
        {
            selectedState = index;
            selectedTransition = -1;
        }
        if (ImGui::BeginPopupContextItem("NodeCtx"))
        {
            if (ImGui::MenuItem("Set as Entry State"))
            {
                CommitControllerEdit(scene, "Set Animator Entry State",
                    [&]() { controller.EntryState = state.Name; });
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete State"))
            {
                CommitControllerEdit(scene, "Delete Animator State", [&]() {
                    const std::string removed = state.Name;
                    controller.States.erase(controller.States.begin() + index);
                    std::erase_if(controller.Transitions,
                        [&](const AnimatorTransition& tr) {
                            return tr.From == removed || tr.To == removed;
                        });
                    if (controller.EntryState == removed)
                        controller.EntryState = controller.States.empty()
                            ? std::string{}
                            : controller.States.front().Name;
                    if (animator.ActiveState == removed)
                        animator.ClearControllerRuntime();
                    selectedState = -1;
                    selectedTransition = -1;
                });
            }
            ImGui::EndPopup();
        }
        ImGui::OpenPopupOnItemClick("NodeCtx", ImGuiPopupFlags_MouseButtonRight);
        const bool active = animator.ActiveState == state.Name;
        const bool entry = controller.EntryState == state.Name;
        const bool selected = selectedState == index;
        const ImU32 fill = active ? IM_COL32(39, 103, 70, 255)
            : selected          ? IM_COL32(45, 82, 122, 255)
                                : IM_COL32(46, 50, 60, 255);
        const ImU32 border = entry ? IM_COL32(255, 171, 64, 255)
            : active             ? IM_COL32(89, 226, 139, 255)
                                 : IM_COL32(101, 111, 128, 255);
        draw->AddRectFilled(topLeft, ImVec2{topLeft.x + nodeSize.x, topLeft.y + nodeSize.y}, fill, 6.0F);
        draw->AddRect(topLeft, ImVec2{topLeft.x + nodeSize.x, topLeft.y + nodeSize.y}, border, 6.0F,
            0, selected || active ? 2.5F : 1.5F);
        draw->AddText(ImVec2{topLeft.x + 10.0F, topLeft.y + 8.0F}, IM_COL32_WHITE, state.Name.c_str());
        draw->AddText(ImVec2{topLeft.x + 10.0F, topLeft.y + 29.0F}, IM_COL32(174, 182, 195, 255),
            state.ClipName.empty() ? "No clip" : state.ClipName.c_str());
        if (entry)
        {
            draw->AddTriangleFilled(ImVec2{topLeft.x - 12.0F, topLeft.y + 20.0F},
                ImVec2{topLeft.x - 2.0F, topLeft.y + 27.0F},
                ImVec2{topLeft.x - 12.0F, topLeft.y + 34.0F}, border);
        }
        // Port circle on right edge
        const ImVec2 portPos{topLeft.x + nodeSize.x, topLeft.y + nodeSize.y * 0.5F};
        const bool portHit = canvasHovered &&
            ImGui::IsMouseHoveringRect(
                ImVec2{portPos.x - 7.0F, portPos.y - 7.0F},
                ImVec2{portPos.x + 7.0F, portPos.y + 7.0F});
        if (canvasHovered || connectingFrom == index)
        {
            const ImU32 portCol = portHit || connectingFrom == index
                ? IM_COL32(255, 208, 76, 255)
                : IM_COL32(140, 152, 168, 210);
            draw->AddCircleFilled(portPos, 5.0F, portCol);
            draw->AddCircle(portPos, 5.5F, IM_COL32(220, 228, 238, 180), 12, 1.2F);
        }
        if (portHit && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            connectingFrom = index;
            connectingEnd = portPos;
            selectedState = -1;
            selectedTransition = -1;
        }
        ImGui::PopID();
    }
    // In-progress connection line
    if (connectingFrom >= 0 && connectingFrom < static_cast<int>(controller.States.size()))
    {
        const AnimatorState& srcState =
            controller.States[static_cast<std::size_t>(connectingFrom)];
        const ImVec2 srcPort{origin.x + graphPan.x + srcState.Position.x + nodeSize.x,
            origin.y + graphPan.y + srcState.Position.y + nodeSize.y * 0.5F};
        connectingEnd = ImGui::GetMousePos();
        draw->AddBezierCubic(srcPort,
            ImVec2{srcPort.x + 60.0F, srcPort.y},
            ImVec2{connectingEnd.x - 60.0F, connectingEnd.y},
            connectingEnd, IM_COL32(255, 208, 76, 200), 2.0F);
        draw->AddCircleFilled(connectingEnd, 4.0F, IM_COL32(255, 208, 76, 200));
    }
    draw->PopClipRect();
    ImGui::EndChild();

    selectedState = selectedState >= 0 && selectedState < static_cast<int>(controller.States.size())
        ? selectedState
        : -1;
    selectedTransition = selectedTransition >= 0 &&
            selectedTransition < static_cast<int>(controller.Transitions.size())
        ? selectedTransition
        : -1;
    if (selectedState >= 0)
    {
        AnimatorState& state = controller.States[static_cast<std::size_t>(selectedState)];
        ImGui::BeginChild("StateInspector", ImVec2{0.0F, 118.0F}, true);
        ImGui::TextUnformatted("STATE");
        ImGui::SameLine();
        char stateName[96]{};
        std::snprintf(stateName, sizeof(stateName), "%s", state.Name.c_str());
        ImGui::SetNextItemWidth(150.0F);
        if (ImGui::InputText("##StateName", stateName, sizeof(stateName)))
        {
            const std::string newName = stateName;
            if (newName != "Any State")
            {
                const std::string oldName = state.Name;
                state.Name = newName;
                if (controller.EntryState == oldName) controller.EntryState = state.Name;
                if (animator.ActiveState == oldName) animator.ActiveState = state.Name;
                for (AnimatorTransition& transition : controller.Transitions)
                {
                    if (transition.From == oldName) transition.From = state.Name;
                    if (transition.To == oldName) transition.To = state.Name;
                }
            }
        }
        TrackControllerEditItem(scene, "Rename Animator State");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0F);
        if (ImGui::BeginCombo("##StateClip", state.ClipName.empty() ? "Select clip" : state.ClipName.c_str()))
        {
            for (const std::string& clipName : clipNames)
            {
                if (ImGui::Selectable(clipName.c_str(), clipName == state.ClipName))
                {
                    CommitControllerEdit(scene, "Set Animator State Clip",
                        [&]() { state.ClipName = clipName; });
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Set Entry"))
        {
            CommitControllerEdit(scene, "Set Animator Entry State",
                [&]() { controller.EntryState = state.Name; });
        }
        ImGui::SameLine();
        if (ImGui::Button(FADIX_ICON_TRASH "  Delete State"))
        {
            CommitControllerEdit(scene, "Delete Animator State", [&]() {
                const std::string removed = state.Name;
                controller.States.erase(controller.States.begin() + selectedState);
                std::erase_if(controller.Transitions, [&](const AnimatorTransition& transition) {
                    return transition.From == removed || transition.To == removed;
                });
                if (controller.EntryState == removed)
                {
                    controller.EntryState = controller.States.empty() ? std::string{}
                                                                     : controller.States.front().Name;
                }
                if (animator.ActiveState == removed) animator.ClearControllerRuntime();
                selectedState = -1;
                selectedTransition = -1;
            });
        }
        if (selectedState >= 0 && controller.States.size() > 1U)
        {
            ImGui::TextUnformatted("Transitions:");
            ImGui::SameLine();
            for (int target = 0; target < static_cast<int>(controller.States.size()); ++target)
            {
                if (target == selectedState) continue;
                ImGui::PushID(target + 20000);
                if (ImGui::SmallButton((std::string{"+ "} + controller.States[static_cast<std::size_t>(target)].Name).c_str()))
                {
                    CommitControllerEdit(scene, "Add Animator Transition", [&]() {
                        AnimatorTransition transition;
                        transition.From = state.Name;
                        transition.To = controller.States[static_cast<std::size_t>(target)].Name;
                        transition.HasExitTime = true;
                        controller.Transitions.push_back(std::move(transition));
                        selectedTransition = static_cast<int>(controller.Transitions.size()) - 1;
                        selectedState = -1;
                    });
                }
                ImGui::SameLine();
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    if (selectedTransition >= 0)
    {
        AnimatorTransition& transition =
            controller.Transitions[static_cast<std::size_t>(selectedTransition)];
        ImGui::BeginChild("TransitionInspector", ImVec2{0.0F, 150.0F}, true);
        ImGui::Text("TRANSITION  %s -> %s", transition.From.c_str(), transition.To.c_str());
        ImGui::SetNextItemWidth(105.0F);
        ImGui::DragFloat("Duration", &transition.Duration, 0.01F, 0.0F, 10.0F, "%.2f s");
        TrackControllerEditItem(scene, "Set Transition Duration");
        ImGui::SameLine();
        bool hasExitTime = transition.HasExitTime;
        if (ImGui::Checkbox("Exit Time", &hasExitTime))
        {
            CommitControllerEdit(scene, "Toggle Transition Exit Time",
                [&]() { transition.HasExitTime = hasExitTime; });
        }
        if (transition.HasExitTime)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.0F);
            ImGui::DragFloat("##Exit", &transition.ExitTime, 0.01F, 0.0F, 1.0F, "%.2f normalized");
            TrackControllerEditItem(scene, "Set Transition Exit Time");
        }
        ImGui::SameLine();
        if (ImGui::Button(FADIX_ICON_PLUS "  Condition") && !controller.Parameters.empty())
        {
            CommitControllerEdit(scene, "Add Animator Condition", [&]() {
                AnimatorCondition condition;
                condition.Parameter = controller.Parameters.front().Name;
                transition.Conditions.push_back(std::move(condition));
            });
        }
        ImGui::SameLine();
        if (ImGui::Button(FADIX_ICON_TRASH "  Delete Transition"))
        {
            CommitControllerEdit(scene, "Delete Animator Transition", [&]() {
                controller.Transitions.erase(controller.Transitions.begin() + selectedTransition);
                selectedTransition = -1;
            });
        }
        if (selectedTransition >= 0)
        {
            int removeCondition = -1;
            for (int conditionIndex = 0;
                 conditionIndex < static_cast<int>(transition.Conditions.size()); ++conditionIndex)
            {
                AnimatorCondition& condition =
                    transition.Conditions[static_cast<std::size_t>(conditionIndex)];
                ImGui::PushID(conditionIndex);
                ImGui::SetNextItemWidth(140.0F);
                if (ImGui::BeginCombo("##Parameter", condition.Parameter.c_str()))
                {
                    for (const AnimatorParameter& parameter : controller.Parameters)
                    {
                        if (ImGui::Selectable(parameter.Name.c_str(), parameter.Name == condition.Parameter))
                        {
                            CommitControllerEdit(scene, "Set Transition Parameter",
                                [&]() { condition.Parameter = parameter.Name; });
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                static constexpr std::array<const char*, 4> comparisonLabels{"==", "!=", ">", "<"};
                int comparison = static_cast<int>(condition.Comparison);
                ImGui::SetNextItemWidth(60.0F);
                if (ImGui::Combo("##Comparison", &comparison, comparisonLabels.data(),
                        static_cast<int>(comparisonLabels.size())))
                {
                    CommitControllerEdit(scene, "Set Transition Comparison", [&]() {
                        condition.Comparison = static_cast<AnimatorComparison>(comparison);
                    });
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(105.0F);
                ImGui::DragFloat("##Threshold", &condition.Threshold, 0.05F);
                TrackControllerEditItem(scene, "Set Transition Threshold");
                ImGui::SameLine();
                if (ImGui::SmallButton(FADIX_ICON_TRASH)) removeCondition = conditionIndex;
                ImGui::PopID();
            }
            if (removeCondition >= 0)
            {
                CommitControllerEdit(scene, "Delete Animator Condition", [&]() {
                    transition.Conditions.erase(transition.Conditions.begin() + removeCondition);
                });
            }
        }
        ImGui::EndChild();
    }
    ImGui::PopID();
}

void DrawTransformSection(SceneEditor& scene, const std::filesystem::path& projectRoot,
    entt::registry& registry, const entt::entity entity,
    TransformAnimatorComponent& anim, bool& previewPlaying, float& previewTime,
    int& selChannel, int& selKey, int& selEvent, int& selectedState,
    int& selectedTransition, glm::vec2& graphPan, int& connectingFrom, ImVec2& connectingEnd,
    int& framesPerSecond,
    bool& snapToFrames, float& pixelsPerSecond, float& startTime, float& endTime, int& dragTimeline,
    int& dragChannel, int& dragKey, float& dragStartTime, float& dragStartMouseX,
    const AnimationEditHooks& edits)
{
    ImGui::Separator();
    ImGui::TextUnformatted(FADIX_ICON_FILM "  Transform Animation");
    TransformComponent* transformPtr = registry.try_get<TransformComponent>(entity);
    if (transformPtr == nullptr)
    {
        ImGui::TextDisabled("Entity has no Transform component.");
        return;
    }
    TransformComponent& transform = *transformPtr;
    anim.Playing = false;

    AnimationClipAsset& initialClip = EnsureTransformClip(anim);
    const auto resetClipView = [&]() {
        previewPlaying = false;
        previewTime = 0.0F;
        selChannel = -1;
        selKey = -1;
        selEvent = -1;
    };
    ImGui::SetNextItemWidth(std::max(150.0F, ImGui::GetContentRegionAvail().x - 250.0F));
    if (ImGui::BeginCombo("Clip##TransformClip", initialClip.Name.c_str()))
    {
        for (AnimationClipAsset& candidate : anim.Clips)
        {
            const bool selected = candidate.Name == anim.ClipName;
            if (ImGui::Selectable(candidate.Name.c_str(), selected))
            {
                edits.Begin();
                anim.ClipName = candidate.Name;
                anim.CurrentTime = 0.0F;
                resetClipView();
                edits.End("Select Transform Clip");
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(FADIX_ICON_PLUS "##NewTransformClip"))
    {
        edits.Begin();
        AnimationClipAsset created;
        created.Name = UniqueTransformClipName(anim, "Animation");
        anim.Clips.push_back(std::move(created));
        anim.ClipName = anim.Clips.back().Name;
        anim.CurrentTime = 0.0F;
        resetClipView();
        edits.End("New Transform Clip");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("New clip");
    ImGui::SameLine();
    if (ImGui::SmallButton("Duplicate##TransformClip"))
    {
        edits.Begin();
        AnimationClipAsset duplicate = *FindTransformClip(anim);
        duplicate.Name = UniqueTransformClipName(anim, duplicate.Name + " Copy");
        anim.Clips.push_back(std::move(duplicate));
        anim.ClipName = anim.Clips.back().Name;
        anim.CurrentTime = 0.0F;
        resetClipView();
        edits.End("Duplicate Transform Clip");
    }
    static char renameBuffer[128]{};
    ImGui::SameLine();
    if (ImGui::SmallButton("Rename##TransformClip"))
    {
        const AnimationClipAsset* active = FindTransformClip(anim);
        std::snprintf(renameBuffer, sizeof(renameBuffer), "%s", active != nullptr ? active->Name.c_str() : "Animation");
        ImGui::OpenPopup("Rename Transform Clip");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(anim.Clips.size() <= 1);
    if (ImGui::SmallButton(FADIX_ICON_TRASH "##DeleteTransformClip"))
    {
        edits.Begin();
        const auto active = std::find_if(anim.Clips.begin(), anim.Clips.end(),
            [&](const AnimationClipAsset& candidate) { return candidate.Name == anim.ClipName; });
        const std::size_t index = active != anim.Clips.end()
            ? static_cast<std::size_t>(std::distance(anim.Clips.begin(), active))
            : 0U;
        const std::string removedName = anim.Clips[index].Name;
        anim.Clips.erase(anim.Clips.begin() + static_cast<std::ptrdiff_t>(index));
        anim.ClipName = anim.Clips[std::min(index, anim.Clips.size() - 1U)].Name;
        for (AnimatorState& state : anim.Controller.States)
        {
            if (state.ClipName == removedName) state.ClipName = anim.ClipName;
        }
        anim.CurrentTime = 0.0F;
        resetClipView();
        edits.End("Delete Transform Clip");
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(anim.Clips.size() <= 1 ? "An animator needs at least one clip" : "Delete clip");

    if (ImGui::BeginPopupModal("Rename Transform Clip", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("Name", renameBuffer, sizeof(renameBuffer));
        const bool valid = renameBuffer[0] != '\0';
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Rename"))
        {
            edits.Begin();
            AnimationClipAsset* active = FindTransformClip(anim);
            if (active != nullptr)
            {
                const std::string oldName = active->Name;
                active->Name = UniqueTransformClipName(anim, renameBuffer, active);
                anim.ClipName = active->Name;
                for (AnimatorState& state : anim.Controller.States)
                {
                    if (state.ClipName == oldName) state.ClipName = active->Name;
                }
            }
            edits.End("Rename Transform Clip");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    std::vector<std::string> transformClipNames;
    transformClipNames.reserve(anim.Clips.size());
    for (const AnimationClipAsset& candidate : anim.Clips)
    {
        transformClipNames.push_back(candidate.Name);
    }
    DrawAnimatorController(scene, "TransformController", anim, transformClipNames,
        projectRoot, selectedState, selectedTransition, graphPan,
        connectingFrom, connectingEnd);

    AnimationClipAsset& clip = EnsureTransformClip(anim);
    AdvancePreview(clip, previewTime, previewPlaying, anim.Loop, anim.Speed);

    ImGui::TextDisabled("Add track / key");
    ImGui::SameLine();
    if (ImGui::SmallButton(FADIX_ICON_PLUS "  Position##TransformKey"))
    {
        edits.Begin();
        KeyTransformProperty(anim, transform, AnimationChannel::Property::Translation,
            SnapTime(previewTime, framesPerSecond, snapToFrames));
        edits.End("Insert Position Key");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(FADIX_ICON_PLUS "  Rotation##TransformKey"))
    {
        edits.Begin();
        KeyTransformProperty(anim, transform, AnimationChannel::Property::Rotation,
            SnapTime(previewTime, framesPerSecond, snapToFrames));
        edits.End("Insert Rotation Key");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(FADIX_ICON_PLUS "  Scale##TransformKey"))
    {
        edits.Begin();
        KeyTransformProperty(anim, transform, AnimationChannel::Property::Scale,
            SnapTime(previewTime, framesPerSecond, snapToFrames));
        edits.End("Insert Scale Key");
    }

    const bool insertSelected = DrawDopeSheet("TransformTimeline", 2, clip, previewTime,
        previewPlaying, anim.Loop, anim.Speed, selChannel, selKey, selEvent,
        framesPerSecond, snapToFrames,
        pixelsPerSecond, startTime, endTime, dragTimeline, dragChannel, dragKey, dragStartTime,
        dragStartMouseX, edits,
        [](const int, const AnimationChannel& channel) {
            return std::string{TransformChannelLabel(channel.Target)} + "   " +
                std::to_string(channel.Keyframes.size()) + " keys";
        },
        []() {});
    if (insertSelected)
    {
        edits.Begin();
        const AnimationChannel::Property property =
            clip.Channels[static_cast<std::size_t>(selChannel)].Target;
        KeyTransformProperty(anim, transform, property,
            SnapTime(previewTime, framesPerSecond, snapToFrames));
        edits.End("Insert Animation Key");
    }

    const bool editingEvent =
        DrawEventInspector(clip, selEvent, 2, framesPerSecond, snapToFrames, edits);
    if (!editingEvent && selChannel >= 0 && selChannel < static_cast<int>(clip.Channels.size()))
    {
        AnimationChannel& channel = clip.Channels[static_cast<std::size_t>(selChannel)];
        if (selKey >= 0 && selKey < static_cast<int>(channel.Keyframes.size()))
        {
            AnimationKeyframe& key = channel.Keyframes[static_cast<std::size_t>(selKey)];
            ImGui::Separator();
            ImGui::BeginChild("TransformKeyInspector", ImVec2{0.0F, 124.0F}, true);
            ImGui::Text(FADIX_ICON_KEY "  %s Key", TransformChannelLabel(channel.Target));
            ImGui::TextDisabled("Interpolation");
            ImGui::SameLine(104.0F);
            AnimationChannel::Interpolation requested = channel.InterpolationMode;
            if (DrawInterpolationCombo(channel, "##TransformInterpolation", requested))
            {
                edits.Begin();
                channel.InterpolationMode = requested;
                edits.End("Change Key Interpolation");
            }
            ImGui::TextDisabled("Time");
            ImGui::SameLine(104.0F);
            ImGui::SetNextItemWidth(-1.0F);
            if (ImGui::DragFloat("##TransformKeyTime", &key.Time,
                    1.0F / static_cast<float>(framesPerSecond), 0.0F, 10000.0F, "%.3f s"))
            {
                key.Time = SnapTime(key.Time, framesPerSecond,
                    snapToFrames && !ImGui::GetIO().KeyAlt);
                previewTime = key.Time;
                RecomputeDuration(clip);
            }
            if (ImGui::IsItemActivated())
            {
                edits.Begin();
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                const float movedTime = key.Time;
                SortKeyframes(channel);
                selKey = -1;
                for (int i = 0; i < static_cast<int>(channel.Keyframes.size()); ++i)
                {
                    if (std::abs(channel.Keyframes[static_cast<std::size_t>(i)].Time - movedTime) < 0.0001F)
                    {
                        selKey = i;
                        break;
                    }
                }
                edits.End("Move Animation Key");
            }
            else if (ImGui::IsItemDeactivated())
            {
                edits.Cancel();
            }
            ImGui::TextDisabled(channel.Target == AnimationChannel::Property::Rotation
                    ? "Quaternion"
                    : "Value");
            ImGui::SameLine(104.0F);
            ImGui::SetNextItemWidth(-1.0F);
            if (channel.Target == AnimationChannel::Property::Rotation)
            {
                if (ImGui::DragFloat4("##TransformKeyValue", &key.Value.x, 0.01F))
                {
                    const glm::quat q = glm::normalize(
                        glm::quat{key.Value.w, key.Value.x, key.Value.y, key.Value.z});
                    key.Value = glm::vec4{q.x, q.y, q.z, q.w};
                }
            }
            else if (ImGui::DragFloat3("##TransformKeyValue", &key.Value.x, 0.01F))
            {
            }
            if (ImGui::IsItemActivated())
            {
                edits.Begin();
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                edits.End("Edit Animation Key Value");
            }
            else if (ImGui::IsItemDeactivated())
            {
                edits.Cancel();
            }
            if (ImGui::Button(FADIX_ICON_TRASH "  Delete Key##TransformValue"))
            {
                edits.Begin();
                channel.Keyframes.erase(channel.Keyframes.begin() + selKey);
                RecomputeDuration(clip);
                selKey = -1;
                edits.End("Delete Animation Key");
            }
            ImGui::EndChild();
        }
    }
}
}

void FdxAnimationPanel::Draw(
    SceneEditor& scene, EditorUiState& ui, const std::filesystem::path& projectRoot,
    ViewportPanel* viewports)
{
    if (!ui.ShowFdxAnimation)
    {
        return;
    }
    bool panelOpen = true;
    const char* title = m_SkeletalDirty
        ? FADIX_ICON_FILM " FDX Animation *###FDX Animation"
        : FADIX_ICON_FILM " FDX Animation###FDX Animation";
    const bool expanded = ImGui::Begin(title, &panelOpen,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (!panelOpen)
    {
        if (m_SkeletalDirty)
        {
            m_CloseRequested = true;
            ImGui::OpenPopup("Unsaved Animation");
        }
        else
        {
            ui.ShowFdxAnimation = false;
        }
    }
    if (!expanded)
    {
        ImGui::End();
        return;
    }

    IWorld& world = scene.World();
    entt::registry& registry = world.Registry();

    const auto clearDirty = [this]() {
        m_SkeletalDirty = false;
        m_DirtyTarget.reset();
        m_DirtyClipName.clear();
        m_DirtyFileName.clear();
        m_SavedSkeletalClip.reset();
    };
    const auto refreshAssets = [&scene]() {
        if (AssetDatabase* assets = scene.Assets())
        {
            const Result<void> refreshed = assets->Refresh();
            if (!refreshed)
            {
                scene.Report("[FDX Animation] saved, but Content Browser refresh failed");
            }
        }
    };
    const auto saveDirty = [&]() {
        if (!m_SkeletalDirty)
        {
            return true;
        }
        if (!m_DirtyTarget || scene.GltfMeshes() == nullptr)
        {
            scene.Report("[FDX Animation] cannot resolve the unsaved animation target");
            return false;
        }
        const auto dirtyEntity = world.Find(*m_DirtyTarget);
        const MeshComponent* dirtyMesh = dirtyEntity
            ? registry.try_get<MeshComponent>(*dirtyEntity)
            : nullptr;
        GltfMeshAsset* dirtyModel = dirtyMesh != nullptr && dirtyMesh->ImportedMesh.IsValid()
            ? scene.GltfMeshes()->GetMutable(dirtyMesh->ImportedMesh)
            : nullptr;
        if (dirtyModel == nullptr)
        {
            scene.Report("[FDX Animation] cannot resolve the unsaved animation model");
            return false;
        }
        for (const AnimationClipAsset& candidate : dirtyModel->Animations)
        {
            if (candidate.Name != m_DirtyClipName)
            {
                continue;
            }
            AnimationClipAsset saved = candidate;
            saved.Name = m_DirtyFileName.empty() ? candidate.Name : m_DirtyFileName;
            if (!SaveClip(projectRoot, saved, *dirtyModel))
            {
                scene.Report("[FDX Animation] save failed; animation remains unsaved");
                return false;
            }
            scene.Report("[FDX Animation] Saved " + saved.Name + ".fdxanim");
            clearDirty();
            refreshAssets();
            return true;
        }
        scene.Report("[FDX Animation] unsaved clip no longer exists");
        return false;
    };
    const auto discardDirty = [&]() {
        if (m_SkeletalDirty && m_SavedSkeletalClip && m_DirtyTarget &&
            scene.GltfMeshes() != nullptr)
        {
            const auto dirtyEntity = world.Find(*m_DirtyTarget);
            const MeshComponent* dirtyMesh = dirtyEntity
                ? registry.try_get<MeshComponent>(*dirtyEntity)
                : nullptr;
            GltfMeshAsset* dirtyModel = dirtyMesh != nullptr && dirtyMesh->ImportedMesh.IsValid()
                ? scene.GltfMeshes()->GetMutable(dirtyMesh->ImportedMesh)
                : nullptr;
            if (dirtyModel != nullptr)
            {
                for (AnimationClipAsset& candidate : dirtyModel->Animations)
                {
                    if (candidate.Name == m_DirtyClipName)
                    {
                        candidate = *m_SavedSkeletalClip;
                        break;
                    }
                }
            }
        }
        clearDirty();
    };

    if (m_CloseRequested)
    {
        ImGui::OpenPopup("Unsaved Animation");
    }
    if (ImGui::BeginPopupModal("Unsaved Animation", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Save animation changes before closing FDX Animation?");
        if (ImGui::Button(FADIX_ICON_SAVE "  Save"))
        {
            if (saveDirty())
            {
                m_CloseRequested = false;
                ui.ShowFdxAnimation = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard"))
        {
            discardDirty();
            m_CloseRequested = false;
            ui.ShowFdxAnimation = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_CloseRequested = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

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
    const std::optional<Uuid> previewTarget = target.IsValid()
        ? std::optional<Uuid>{target}
        : std::nullopt;
    if (m_PreviewTarget != previewTarget)
    {
        if (!saveDirty())
        {
            m_Pinned = m_DirtyTarget;
            ImGui::TextWrapped("Unsaved animation could not be saved. The previous target remains "
                               "pinned so its changes cannot be overwritten.");
            ImGui::End();
            return;
        }
        m_PreviewTarget = previewTarget;
        m_SkeletalEditBefore.reset();
        m_TransformEditBefore.reset();
        m_SaveName[0] = '\0';
        m_SkeletalPreviewPlaying = false;
        m_TransformPreviewPlaying = false;
        m_SkeletalPreviewTime = 0.0F;
        m_TransformPreviewTime = 0.0F;
        m_SelectedChannel = -1;
        m_SelectedKey = -1;
        m_SelectedEvent = -1;
        m_TSelectedChannel = -1;
        m_TSelectedKey = -1;
        m_TSelectedEvent = -1;
    }

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
        ImGui::TextWrapped(
            "Select any entity in the Hierarchy (or pin one). Cubes use Transform Animation; "
            "rigged meshes also show skeletal clips.");
        ImGui::End();
        return;
    }
    if (viewports != nullptr)
    {
        const float previewHeight = std::clamp(ImGui::GetContentRegionAvail().y * 0.20F,
            82.0F, 132.0F);
        viewports->DrawAnimationPreview(world, target, previewHeight,
            m_SkeletalPreviewTime, m_TransformPreviewTime);
        ImGui::Separator();
    }
    TransformAnimatorComponent* transformAnim =
        registry.try_get<TransformAnimatorComponent>(*entity);
    const auto transformEditHooks = [&](TransformAnimatorComponent& edited) {
        TransformAnimatorComponent* editedPtr = &edited;
        return AnimationEditHooks{
            [this, editedPtr]() {
                if (!m_TransformEditBefore)
                {
                    m_TransformEditBefore = *editedPtr;
                }
            },
            [this, &scene, &world, target, editedPtr](const char* name) {
                if (!m_TransformEditBefore)
                {
                    return;
                }
                TransformAnimatorComponent before = std::move(*m_TransformEditBefore);
                m_TransformEditBefore.reset();
                scene.History().Push(std::make_unique<TransformAnimationEditCommand>(
                    world, target, std::move(before), *editedPtr, name));
                scene.MarkChanged();
            },
            [this]() { m_TransformEditBefore.reset(); }};
    };
    if (SkeletonComponent* skeleton = registry.try_get<SkeletonComponent>(*entity))
    {
        skeleton->JointMatrices.clear();
    }
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
                scene.BeginEditTransaction();
                TransformAnimatorComponent created;
                static_cast<void>(EnsureTransformClip(created, entityName));
                created.Playing = false;
                transformAnim =
                    &registry.emplace<TransformAnimatorComponent>(*entity, std::move(created));
                scene.EndEditTransaction("Add Transform Animation");
                transformAnim = registry.try_get<TransformAnimatorComponent>(*entity);
            }
            if (!hasSkinnedModel)
            {
                drawAssignHelp();
            }
        }
        if (transformAnim != nullptr)
        {
            const AnimationEditHooks edits = transformEditHooks(*transformAnim);
            DrawTransformSection(scene, projectRoot, registry, *entity, *transformAnim,
                m_TransformPreviewPlaying, m_TransformPreviewTime,
                m_TSelectedChannel, m_TSelectedKey, m_TSelectedEvent,
                m_TSelectedAnimatorState, m_TSelectedAnimatorTransition,
                m_TransformGraphPan, m_TConnectingFromState, m_TConnectingLineEnd,
                m_FramesPerSecond, m_SnapToFrames,
                m_TimelinePixelsPerSecond, m_TimelineStart, m_TimelineEnd, m_DragTimeline, m_DragChannel,
                m_DragKey, m_DragStartTime, m_DragStartMouseX, edits);
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
                if (!saveDirty())
                {
                    continue;
                }
                animator->ClipName = n;
                std::snprintf(m_SaveName, sizeof(m_SaveName), "%s", n.c_str());
                m_SkeletalPreviewTime = 0.0F;
                m_SkeletalPreviewPlaying = false;
                m_SelectedChannel = -1;
                m_SelectedKey = -1;
                m_SelectedEvent = -1;
            }
        }
        ImGui::EndCombo();
    }
    AnimationClipAsset& clip = gltf->Animations[static_cast<std::size_t>(clipIndex)];
    if (m_SaveName[0] == '\0')
    {
        std::snprintf(m_SaveName, sizeof(m_SaveName), "%s", clip.Name.c_str());
    }
    animator->Playing = false;
    AdvancePreview(clip, m_SkeletalPreviewTime, m_SkeletalPreviewPlaying,
        animator->Loop, animator->Speed);

    const std::string activeClipName = clip.Name;
    const std::string activeFileName =
        m_SaveName[0] != '\0' ? std::string{m_SaveName} : activeClipName;
    const auto markSkeletalDirty =
        [this, target, activeClipName, activeFileName, gltf, clipIndex]() {
            if (!m_SkeletalDirty && !m_SavedSkeletalClip && gltf != nullptr && clipIndex >= 0 &&
                clipIndex < static_cast<int>(gltf->Animations.size()))
            {
                m_SavedSkeletalClip = gltf->Animations[static_cast<std::size_t>(clipIndex)];
            }
            m_SkeletalDirty = true;
            m_DirtyTarget = target;
            m_DirtyClipName = activeClipName;
            m_DirtyFileName = activeFileName;
        };
    const AnimationEditHooks skeletalEdits{
        [this, &clip, clipIndex]() {
            if (!m_SkeletalEditBefore)
            {
                m_SkeletalEditBefore = clip;
                m_SkeletalEditClipIndex = clipIndex;
            }
        },
        [this, &scene, gltf, clipIndex, &clip, markSkeletalDirty](const char* name) {
            if (!m_SkeletalEditBefore || m_SkeletalEditClipIndex != clipIndex)
            {
                m_SkeletalEditBefore.reset();
                m_SkeletalEditClipIndex = -1;
                return;
            }
            AnimationClipAsset before = std::move(*m_SkeletalEditBefore);
            m_SkeletalEditBefore.reset();
            m_SkeletalEditClipIndex = -1;
            if (!m_SkeletalDirty && !m_SavedSkeletalClip)
            {
                m_SavedSkeletalClip = before;
            }
            markSkeletalDirty();
            scene.History().Push(std::make_unique<AnimationClipEditCommand>(*gltf, clipIndex,
                std::move(before), clip, name, markSkeletalDirty));
            scene.MarkChanged();
        },
        [this]() {
            m_SkeletalEditBefore.reset();
            m_SkeletalEditClipIndex = -1;
        }};

    std::vector<std::string> skeletalClipNames;
    skeletalClipNames.reserve(gltf->Animations.size());
    for (const AnimationClipAsset& candidate : gltf->Animations)
    {
        skeletalClipNames.push_back(candidate.Name);
    }
    DrawAnimatorController(scene, "SkeletalController", *animator, skeletalClipNames,
        projectRoot, m_SelectedAnimatorState, m_SelectedAnimatorTransition,
        m_SkeletalGraphPan, m_ConnectingFromState, m_ConnectingLineEnd);

    const bool insertSelected = DrawDopeSheet("SkeletalTimeline", 1, clip,
        m_SkeletalPreviewTime, m_SkeletalPreviewPlaying, animator->Loop, animator->Speed,
        m_SelectedChannel, m_SelectedKey, m_SelectedEvent, m_FramesPerSecond, m_SnapToFrames,
        m_TimelinePixelsPerSecond, m_TimelineStart, m_TimelineEnd, m_DragTimeline, m_DragChannel,
        m_DragKey, m_DragStartTime, m_DragStartMouseX, skeletalEdits,
        [&](const int, const AnimationChannel& channel) {
            return JointName(*gltf, channel.JointIndex) + "   [" + PropertyLabel(channel.Target) +
                "]   " + std::to_string(channel.Keyframes.size()) + " keys";
        },
        []() {});

    const bool editingEvent = DrawEventInspector(
        clip, m_SelectedEvent, 1, m_FramesPerSecond, m_SnapToFrames, skeletalEdits);
    if (!editingEvent && m_SelectedChannel >= 0 &&
        m_SelectedChannel < static_cast<int>(clip.Channels.size()))
    {
        AnimationChannel& ch = clip.Channels[static_cast<std::size_t>(m_SelectedChannel)];
        if (insertSelected)
        {
            skeletalEdits.Begin();
            InsertOrReplaceKey(ch,
                SnapTime(m_SkeletalPreviewTime, m_FramesPerSecond, m_SnapToFrames));
            RecomputeDuration(clip);
            skeletalEdits.End("Insert Animation Key");
        }
        if (m_SelectedKey >= 0 && m_SelectedKey < static_cast<int>(ch.Keyframes.size()))
        {
            AnimationKeyframe& key = ch.Keyframes[static_cast<std::size_t>(m_SelectedKey)];
            ImGui::Separator();
            ImGui::BeginChild("SkeletalKeyInspector", ImVec2{0.0F, 124.0F}, true);
            ImGui::Text(FADIX_ICON_KEY "  %s [%s] Key", JointName(*gltf, ch.JointIndex).c_str(),
                PropertyLabel(ch.Target));
            ImGui::TextDisabled("Interpolation");
            ImGui::SameLine(104.0F);
            AnimationChannel::Interpolation requested = ch.InterpolationMode;
            if (DrawInterpolationCombo(ch, "##SkeletalInterpolation", requested))
            {
                skeletalEdits.Begin();
                ch.InterpolationMode = requested;
                skeletalEdits.End("Change Key Interpolation");
            }
            ImGui::TextDisabled("Time");
            ImGui::SameLine(104.0F);
            ImGui::SetNextItemWidth(-1.0F);
            if (ImGui::DragFloat("##SkeletalKeyTime", &key.Time,
                    1.0F / static_cast<float>(m_FramesPerSecond), 0.0F, 10000.0F, "%.3f s"))
            {
                key.Time = SnapTime(key.Time, m_FramesPerSecond,
                    m_SnapToFrames && !ImGui::GetIO().KeyAlt);
                m_SkeletalPreviewTime = key.Time;
                animator->Playing = false;
                RecomputeDuration(clip);
            }
            if (ImGui::IsItemActivated())
            {
                skeletalEdits.Begin();
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                const float movedTime = key.Time;
                SortKeyframes(ch);
                m_SelectedKey = -1;
                for (int i = 0; i < static_cast<int>(ch.Keyframes.size()); ++i)
                {
                    if (std::abs(ch.Keyframes[static_cast<std::size_t>(i)].Time - movedTime) < 0.0001F)
                    {
                        m_SelectedKey = i;
                        break;
                    }
                }
                skeletalEdits.End("Move Animation Key");
            }
            else if (ImGui::IsItemDeactivated())
            {
                skeletalEdits.Cancel();
            }
            ImGui::TextDisabled(ch.Target == AnimationChannel::Property::Rotation
                    ? "Quaternion"
                    : "Value");
            ImGui::SameLine(104.0F);
            ImGui::SetNextItemWidth(-1.0F);
            if (ch.Target == AnimationChannel::Property::Rotation)
            {
                if (ImGui::DragFloat4("##SkeletalKeyValue", &key.Value.x, 0.01F))
                {
                    const glm::quat q =
                        glm::normalize(glm::quat{key.Value.w, key.Value.x, key.Value.y, key.Value.z});
                    key.Value = glm::vec4{q.x, q.y, q.z, q.w};
                }
            }
            else if (ImGui::DragFloat3("##SkeletalKeyValue", &key.Value.x, 0.01F))
            {
            }
            if (ImGui::IsItemActivated())
            {
                skeletalEdits.Begin();
            }
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                skeletalEdits.End("Edit Animation Key Value");
            }
            else if (ImGui::IsItemDeactivated())
            {
                skeletalEdits.Cancel();
            }
            if (ImGui::Button(FADIX_ICON_TRASH "  Delete Key##SkeletalValue"))
            {
                skeletalEdits.Begin();
                ch.Keyframes.erase(ch.Keyframes.begin() + m_SelectedKey);
                RecomputeDuration(clip);
                m_SelectedKey = -1;
                skeletalEdits.End("Delete Animation Key");
            }
            ImGui::EndChild();
        }
    }
    else if (!editingEvent)
    {
        ImGui::TextDisabled("Select a channel row to insert or edit keys.");
    }
    ImGui::Separator();

    // Persistence: .fdxanim is a first-class asset under Assets/Animations.
    const auto saveActiveClip = [&]() {
        AnimationClipAsset saved = clip;
        saved.Name = m_SaveName[0] != '\0' ? std::string{m_SaveName} : clip.Name;
        if (!SaveClip(projectRoot, saved, *gltf))
        {
            scene.Report("[FDX Animation] Save failed; animation remains unsaved");
            return false;
        }
        scene.Report("[FDX Animation] Saved " + saved.Name + ".fdxanim");
        if (m_DirtyTarget == target && m_DirtyClipName == clip.Name)
        {
            clearDirty();
        }
        refreshAssets();
        return true;
    };
    if (m_SkeletalDirty && m_DirtyTarget == target && m_DirtyClipName == clip.Name)
    {
        ImGui::TextColored(ImVec4{1.0F, 0.72F, 0.22F, 1.0F},
            FADIX_ICON_KEY "  Unsaved animation changes");
    }
    ImGui::SetNextItemWidth(200.0F);
    if (ImGui::InputText("File name", m_SaveName, sizeof(m_SaveName)) &&
        m_SkeletalDirty && m_DirtyTarget == target && m_DirtyClipName == clip.Name)
    {
        m_DirtyFileName = m_SaveName;
    }
    ImGui::SameLine();
    if (ImGui::Button(FADIX_ICON_SAVE "  Save"))
    {
        static_cast<void>(saveActiveClip());
    }
    const bool saveShortcut = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S);
    if (saveShortcut)
    {
        static_cast<void>(saveActiveClip());
    }
    ImGui::SameLine();
    if (ImGui::Button(FADIX_ICON_FOLDER_OPEN "  Load"))
    {
        if (!saveDirty())
        {
            scene.Report("[FDX Animation] resolve unsaved changes before loading another clip");
        }
        else if (auto loaded = LoadClip(projectRoot, m_SaveName, *gltf))
        {
            AnimationClipAsset& merged = MergeClip(*gltf, std::move(*loaded));
            animator->ClipName = merged.Name;
            m_SkeletalPreviewTime = 0.0F;
            m_SelectedChannel = -1;
            m_SelectedKey = -1;
            m_SelectedEvent = -1;
            clearDirty();
            scene.MarkChanged();
            scene.Report("[FDX Animation] Loaded " + merged.Name + ".fdxanim");
        }
        else
        {
            scene.Report("[FDX Animation] Load failed (no such .fdxanim)");
        }
    }

    // Coexistence: a skinned entity may also carry an entity-transform clip.
    if (transformAnim == nullptr)
    {
        ImGui::Separator();
        if (ImGui::Button("Add Transform Animation"))
        {
            scene.BeginEditTransaction();
            TransformAnimatorComponent created;
            static_cast<void>(EnsureTransformClip(created, entityName));
            created.Playing = false;
            transformAnim =
                &registry.emplace<TransformAnimatorComponent>(*entity, std::move(created));
            scene.EndEditTransaction("Add Transform Animation");
            transformAnim = registry.try_get<TransformAnimatorComponent>(*entity);
        }
        else
        {
            ImGui::TextDisabled("Optional: key this entity's own XYZ (in addition to skin clips).");
        }
    }
    if (transformAnim != nullptr)
    {
        if (ImGui::CollapsingHeader("Object Transform Animation"))
        {
            const AnimationEditHooks edits = transformEditHooks(*transformAnim);
            DrawTransformSection(scene, projectRoot, registry, *entity, *transformAnim,
                m_TransformPreviewPlaying, m_TransformPreviewTime,
                m_TSelectedChannel, m_TSelectedKey, m_TSelectedEvent,
                m_TSelectedAnimatorState, m_TSelectedAnimatorTransition,
                m_TransformGraphPan, m_TConnectingFromState, m_TConnectingLineEnd,
                m_FramesPerSecond, m_SnapToFrames, m_TimelinePixelsPerSecond,
                m_TimelineStart, m_TimelineEnd, m_DragTimeline, m_DragChannel,
                m_DragKey, m_DragStartTime, m_DragStartMouseX, edits);
        }
    }

    ImGui::End();
}
}
