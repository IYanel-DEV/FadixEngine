#pragma once

#include "assets/ScriptAsset.hpp"
#include "editor/EditorLog.hpp"
#include "editor/assets/AssetBrowserController.hpp"
#include "editor/assets/AssetThumbnailCache.hpp"
#include "editor/imgui/EditorUiState.hpp"
#include "editor/scene/SceneEditor.hpp"
#include "engine/assets/AssetHandle.hpp"

#include <imgui.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fadix
{
class ScriptDatabase;
}

namespace fadix::editor
{
struct ContentBrowserCallbacks
{
    std::function<void(const AssetHandle&, const std::filesystem::path&)> LoadScene;
    std::function<void(const std::filesystem::path&)> CreateScene;
    std::function<void(const AssetHandle&, const std::filesystem::path&)> OpenMaterial;
    std::function<void(const std::string& scriptName)> OpenScript;
    std::function<void(const AssetDragPayload&)> DropInScene;
    std::function<void(const AssetHandle&, const std::filesystem::path&)> CreateMeshEntity;
    std::function<void(std::string_view)> ReportStatus;
    std::function<void(std::string_view)> ReportError;
    std::function<void(const std::filesystem::path&)> RevealPath;
};

/// ImGui Content Browser — thin UI over AssetBrowserController + AssetThumbnailCache.
class ContentBrowserPanel final
{
public:
    void Bind(
        AssetBrowserController& browser,
        AssetThumbnailCache& thumbnails,
        ScriptDatabase* scripts,
        EditorLog* log,
        SDL_GPUDevice* device);
    void Shutdown() noexcept;
    void SetCallbacks(ContentBrowserCallbacks callbacks);

    void Poll();
    void Draw(EditorUiState& ui, SceneEditor* scene);
    void HandleExternalDrop(const SDL_Event& event, EditorUiState& ui);
    void PromptImportModel(EditorUiState& ui, SDL_Window* window);
    void ImportPath(const std::filesystem::path& path, EditorUiState& ui);

    [[nodiscard]] bool WindowHovered() const noexcept { return m_WindowHovered; }
    void ApplySceneDrop(const AssetDragPayload& payload, SceneEditor* scene);

private:
    struct ExternalImportQueue
    {
        std::mutex Mutex;
        std::vector<std::filesystem::path> Paths;
    };

    struct ThumbGpu
    {
        SDL_GPUTexture* Texture{nullptr};
        int Width{0};
        int Height{0};
    };

    void ApplyControllerCallbacks(bool confirmDeletes);
    void DrawToolbar(EditorUiState& ui);
    void DrawBreadcrumbs();
    void DrawEntries(EditorUiState& ui);
    void DrawEntryRow(std::size_t index, const AssetBrowserEntry& entry, EditorUiState& ui);
    void DrawEntryGrid(std::size_t index, const AssetBrowserEntry& entry, EditorUiState& ui);
    void DrawContextMenu(std::size_t index, const AssetBrowserEntry& entry, EditorUiState& ui);
    void BeginAssetDrag(const AssetBrowserEntry& entry);
    void DrainPendingExternalImports(EditorUiState& ui);
    [[nodiscard]] ImTextureRef ThumbnailTexture(const AssetBrowserEntry& entry);
    void ClearGpuThumbs();
    [[nodiscard]] bool UploadThumb(const std::filesystem::path& path, ThumbGpu& out);
    void Status(std::string_view text, EditorUiState& ui);
    void Error(std::string_view text, EditorUiState& ui);

    AssetBrowserController* m_Browser{nullptr};
    AssetThumbnailCache* m_Thumbnails{nullptr};
    ScriptDatabase* m_Scripts{nullptr};
    EditorLog* m_Log{nullptr};
    SDL_GPUDevice* m_Device{nullptr};
    ContentBrowserCallbacks m_Callbacks;

    char m_SearchBuf[128]{};
    char m_NameBuf[128]{};
    bool m_Grid{true};
    bool m_WindowHovered{false};
    ImVec2 m_WindowMin{};
    ImVec2 m_WindowMax{};
    bool m_ImportBusy{false};
    bool m_PendingNewFolder{false};
    bool m_PendingNewScene{false};
    bool m_ClearThumbsPending{false};
    std::optional<std::size_t> m_Selected;
    std::optional<std::filesystem::path> m_PendingDelete;
    std::optional<std::filesystem::path> m_PendingRename;
    std::optional<ScriptLanguage> m_PendingScriptLang;
    std::optional<AssetBrowserEntry> m_PendingActivate;
    std::shared_ptr<ExternalImportQueue> m_ExternalImports;
    std::unordered_map<std::string, ThumbGpu> m_GpuThumbs;
};
}
