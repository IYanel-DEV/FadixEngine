#pragma once

#include "editor/imgui/EditorUiState.hpp"
#include "editor/scene/SceneEditor.hpp"
#include "engine/Uuid.hpp"
#include "engine/animation/AnimationClip.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace fadix::editor
{
class ViewportPanel;
// FDX Animation: dockable timeline/keyframe editor for skeletal clips imported on
// a skinned glTF model. Edits clip data in place (the AnimationPlayer evaluates it)
// and round-trips authored clips to project Animations/*.fdxanim.
class FdxAnimationPanel final
{
public:
    void Draw(SceneEditor& scene, EditorUiState& ui, const std::filesystem::path& projectRoot,
        ViewportPanel* viewports);

private:
    std::optional<Uuid> m_Pinned;
    std::optional<Uuid> m_PreviewTarget;
    int m_SelectedChannel{-1};
    int m_SelectedKey{-1};
    int m_SelectedEvent{-1};
    int m_TSelectedChannel{-1}; // transform-clip channel/key selection (separate pool)
    int m_TSelectedKey{-1};
    int m_TSelectedEvent{-1};
    int m_SelectedAnimatorState{-1};
    int m_SelectedAnimatorTransition{-1};
    int m_TSelectedAnimatorState{-1};
    int m_TSelectedAnimatorTransition{-1};
    glm::vec2 m_SkeletalGraphPan{20.0F, 20.0F};
    glm::vec2 m_TransformGraphPan{20.0F, 20.0F};
    int m_FramesPerSecond{30};
    float m_TimelinePixelsPerSecond{120.0F};
    float m_TimelineStart{0.0F};
    float m_TimelineEnd{5.0F};
    bool m_SnapToFrames{true};
    bool m_SkeletalPreviewPlaying{false};
    bool m_TransformPreviewPlaying{false};
    float m_SkeletalPreviewTime{0.0F};
    float m_TransformPreviewTime{0.0F};
    bool m_SkeletalDirty{false};
    bool m_CloseRequested{false};
    std::optional<Uuid> m_DirtyTarget;
    std::string m_DirtyClipName;
    std::string m_DirtyFileName;
    std::optional<AnimationClipAsset> m_SavedSkeletalClip;
    std::optional<AnimationClipAsset> m_SkeletalEditBefore;
    std::optional<TransformAnimatorComponent> m_TransformEditBefore;
    int m_SkeletalEditClipIndex{-1};
    int m_DragChannel{-1};
    int m_DragKey{-1};
    int m_DragTimeline{0};
    float m_DragStartTime{0.0F};
    float m_DragStartMouseX{0.0F};
    char m_SaveName[128]{};
};
}
