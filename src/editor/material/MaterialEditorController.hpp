#pragma once

#include "editor/ui/SceneViewSource.hpp"
#include "engine/assets/MaterialAsset.hpp"

#include <entt/entity/entity.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace fadix
{
class AssetDatabase;
class IAssetDatabase;
class IWorld;
class ViewportRenderer;

namespace rhi
{
class Device;
}

namespace editor
{
/// UI-neutral material editor. Rml DOM binding is MaterialEditorRmlAdapter.
class MaterialEditorController
{
public:
    MaterialEditorController();
    ~MaterialEditorController();

    void Bind(IAssetDatabase& assets, rhi::Device& device);
    void Open(const AssetHandle& materialHandle);
    void Close();
    [[nodiscard]] bool IsOpen() const { return m_Open; }

    [[nodiscard]] MaterialAsset& Editing() noexcept { return m_Editing; }
    [[nodiscard]] const MaterialAsset& Editing() const noexcept { return m_Editing; }
    [[nodiscard]] const std::optional<AssetHandle>& Current() const noexcept { return m_Current; }
    [[nodiscard]] int PreviewShape() const noexcept { return m_PreviewShape; }

    void SetFloatField(std::string_view id, float value);
    void CycleAlphaMode();
    void ToggleDoubleSided();
    void SetPreviewShape(int shape); // 0 cube, 1 sphere, 2 plane
    bool SetTextureSlot(std::string_view slot, const AssetHandle& texture);
    bool ClearTextureSlot(std::string_view slot);
    void Save();

    /// Legacy Rml id helpers (also used by ImGui with the same ids).
    bool HandleButton(const std::string& id);
    bool TryDropTexture(const std::string& slotElementId, const AssetHandle& texture);

    void RenderPreview(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] SceneViewSource PreviewSource() const;

    [[nodiscard]] std::string TextureSlotDisplayName(std::string_view slot) const;

    using UiNotify = std::function<void()>;
    void SetUiNotify(UiNotify notify);

private:
    void NotifyUi();

    IAssetDatabase* m_Assets{nullptr};
    AssetDatabase* m_AssetDb{nullptr};
    rhi::Device* m_Device{nullptr};

    std::optional<AssetHandle> m_Current;
    MaterialAsset m_Editing;
    bool m_Open{false};
    int m_PreviewShape{1};
    UiNotify m_UiNotify;

    std::unique_ptr<ViewportRenderer> m_Preview;
    std::unique_ptr<IWorld> m_PreviewWorld;
    entt::entity m_PreviewMesh{entt::null};
    std::uint32_t m_PreviewWidth{0};
    std::uint32_t m_PreviewHeight{0};
    float m_Orbit{0.7F};
};
}
}
