#include "editor/imgui/panels/ContentBrowserPanel.hpp"

#include "assets/ScriptDatabase.hpp"
#include "editor/imgui/EditorIcons.hpp"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <glm/vec3.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace fadix::editor
{
namespace
{
[[nodiscard]] std::string LowerExt(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

[[nodiscard]] bool IsModelExt(const std::string& ext)
{
    return ext == ".gltf" || ext == ".glb" || ext == ".fbx" || ext == ".obj" ||
        ext == ".blend" || ext == ".stl" || ext == ".ply" || ext == ".dae";
}

[[nodiscard]] const char* TypeGlyph(const AssetBrowserEntry& entry)
{
    if (entry.Kind == AssetBrowserEntryKind::Folder)
    {
        return entry.FolderHasChildren ? FADIX_ICON_FOLDER_OPEN : FADIX_ICON_FOLDER;
    }
    if (entry.AssetType == "Scene" || entry.AssetType == "Prefab")
    {
        return FADIX_ICON_CUBE;
    }
    if (entry.AssetType == "Mesh")
    {
        return FADIX_ICON_CUBE;
    }
    if (entry.AssetType == "Texture")
    {
        return FADIX_ICON_FILE;
    }
    if (entry.AssetType == "Material")
    {
        return FADIX_ICON_GEAR;
    }
    if (entry.AssetType == "Script")
    {
        return FADIX_ICON_FILE;
    }
    if (entry.AssetType == "Audio")
    {
        return FADIX_ICON_FILE;
    }
    if (entry.AssetType == "Animation")
    {
        return FADIX_ICON_FILM;
    }
    if (entry.AssetType == "AnimatorController")
    {
        return FADIX_ICON_SITEMAP;
    }
    return FADIX_ICON_FILE;
}

[[nodiscard]] std::size_t CountCrumbs(const std::string& display)
{
    std::size_t total = 0;
    for (std::size_t i = 0; i < display.size();)
    {
        const std::size_t end = display.find('/', i);
        const std::string segment =
            display.substr(i, end == std::string::npos ? std::string::npos : end - i);
        if (!segment.empty())
        {
            ++total;
        }
        if (end == std::string::npos)
        {
            break;
        }
        i = end + 1;
    }
    return total;
}
}

void ContentBrowserPanel::Bind(
    AssetBrowserController& browser,
    AssetThumbnailCache& thumbnails,
    ScriptDatabase* scripts,
    EditorLog* log,
    SDL_GPUDevice* device)
{
    Shutdown();
    m_ExternalImports = std::make_shared<ExternalImportQueue>();
    m_Browser = &browser;
    m_Thumbnails = &thumbnails;
    m_Scripts = scripts;
    m_Log = log;
    m_Device = device;
    const std::string_view search = browser.Search();
    std::snprintf(
        m_SearchBuf,
        sizeof(m_SearchBuf),
        "%.*s",
        static_cast<int>(std::min(search.size(), sizeof(m_SearchBuf) - 1)),
        search.data());
    ApplyControllerCallbacks(false);
}

void ContentBrowserPanel::Shutdown() noexcept
{
    const std::shared_ptr<ExternalImportQueue> imports = std::move(m_ExternalImports);
    if (imports)
    {
        std::scoped_lock lock{imports->Mutex};
        imports->Paths.clear();
    }
    ClearGpuThumbs();
    m_Browser = nullptr;
    m_Thumbnails = nullptr;
    m_Scripts = nullptr;
    m_Log = nullptr;
    m_Device = nullptr;
    m_Selected.reset();
    m_PendingDelete.reset();
    m_PendingRename.reset();
    m_PendingScriptLang.reset();
    m_PendingActivate.reset();
    m_PendingNewFolder = false;
    m_PendingNewScene = false;
    m_ClearThumbsPending = false;
    m_ImportBusy = false;
}

void ContentBrowserPanel::SetCallbacks(ContentBrowserCallbacks callbacks)
{
    m_Callbacks = std::move(callbacks);
    ApplyControllerCallbacks(false);
}

void ContentBrowserPanel::ApplyControllerCallbacks(const bool confirmDeletes)
{
    if (m_Browser == nullptr)
    {
        return;
    }
    AssetBrowserCallbacks callbacks;
    callbacks.ConfirmDelete = [confirmDeletes](const std::filesystem::path&) {
        return confirmDeletes;
    };
    callbacks.ReportError = [this](std::string_view message) {
        if (m_Log)
        {
            m_Log->Log(std::string{message}, "error");
        }
        if (m_Callbacks.ReportError)
        {
            m_Callbacks.ReportError(message);
        }
    };
    callbacks.EntriesChanged = [this]() {
        // Defer GPU release — Activate can fire mid-draw and freeing textures
        // while ImGui still references them crashes the device.
        m_ClearThumbsPending = true;
    };
    callbacks.LoadScene = m_Callbacks.LoadScene;
    callbacks.OpenMaterial = m_Callbacks.OpenMaterial;
    callbacks.OpenScript = m_Callbacks.OpenScript;
    callbacks.CreateMeshEntity = m_Callbacks.CreateMeshEntity;
    callbacks.AssetDroppedInScene = m_Callbacks.DropInScene;
    m_Browser->SetCallbacks(callbacks);
}

void ContentBrowserPanel::Poll()
{
    if (m_Browser != nullptr)
    {
        m_Browser->PollImports();
    }
}

void ContentBrowserPanel::Status(const std::string_view text, EditorUiState& ui)
{
    ui.StatusText = std::string{text};
    if (m_Callbacks.ReportStatus)
    {
        m_Callbacks.ReportStatus(text);
    }
}

void ContentBrowserPanel::Error(const std::string_view text, EditorUiState& ui)
{
    ui.StatusText = std::string{text};
    if (m_Log)
    {
        m_Log->Log(std::string{text}, "error");
    }
    if (m_Callbacks.ReportError)
    {
        m_Callbacks.ReportError(text);
    }
}

void ContentBrowserPanel::ClearGpuThumbs()
{
    if (m_Device != nullptr)
    {
        for (auto& [_, thumb] : m_GpuThumbs)
        {
            if (thumb.Texture != nullptr)
            {
                SDL_ReleaseGPUTexture(m_Device, thumb.Texture);
            }
        }
    }
    m_GpuThumbs.clear();
}

bool ContentBrowserPanel::UploadThumb(const std::filesystem::path& path, ThumbGpu& out)
{
    if (m_Device == nullptr)
    {
        return false;
    }
    SDL_Surface* loaded = IMG_Load(path.string().c_str());
    if (loaded == nullptr)
    {
        return false;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (rgba == nullptr)
    {
        return false;
    }

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<Uint32>(rgba->w);
    info.height = static_cast<Uint32>(rgba->h);
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(m_Device, &info);
    if (texture == nullptr)
    {
        SDL_DestroySurface(rgba);
        return false;
    }

    const Uint32 bytes = static_cast<Uint32>(rgba->pitch) * static_cast<Uint32>(rgba->h);
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(m_Device, &transferInfo);
    if (transfer == nullptr)
    {
        SDL_ReleaseGPUTexture(m_Device, texture);
        SDL_DestroySurface(rgba);
        return false;
    }
    void* mapped = SDL_MapGPUTransferBuffer(m_Device, transfer, false);
    if (mapped == nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_Device, transfer);
        SDL_ReleaseGPUTexture(m_Device, texture);
        SDL_DestroySurface(rgba);
        return false;
    }
    std::memcpy(mapped, rgba->pixels, bytes);
    SDL_UnmapGPUTransferBuffer(m_Device, transfer);

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(m_Device);
    if (commands == nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_Device, transfer);
        SDL_ReleaseGPUTexture(m_Device, texture);
        SDL_DestroySurface(rgba);
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = transfer;
    SDL_GPUTextureRegion dest{};
    dest.texture = texture;
    dest.w = info.width;
    dest.h = info.height;
    dest.d = 1;
    SDL_UploadToGPUTexture(copy, &source, &dest, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(commands);
    SDL_WaitForGPUIdle(m_Device);
    SDL_ReleaseGPUTransferBuffer(m_Device, transfer);

    out.Texture = texture;
    out.Width = rgba->w;
    out.Height = rgba->h;
    SDL_DestroySurface(rgba);
    return true;
}

ImTextureRef ContentBrowserPanel::ThumbnailTexture(const AssetBrowserEntry& entry)
{
    if (m_Thumbnails == nullptr || !entry.Handle || entry.Kind != AssetBrowserEntryKind::Asset ||
        entry.AssetType != "Texture")
    {
        return {};
    }
    const auto path = m_Thumbnails->TextureThumbnailPath(*entry.Handle);
    if (!path)
    {
        return {};
    }
    const std::string key = path->generic_string();
    auto it = m_GpuThumbs.find(key);
    if (it == m_GpuThumbs.end())
    {
        ThumbGpu thumb;
        if (!UploadThumb(*path, thumb))
        {
            return {};
        }
        it = m_GpuThumbs.emplace(key, thumb).first;
    }
    return ImTextureRef{static_cast<ImTextureID>(reinterpret_cast<intptr_t>(it->second.Texture))};
}

void ContentBrowserPanel::BeginAssetDrag(const AssetBrowserEntry& entry)
{
    if (m_Browser == nullptr)
    {
        return;
    }
    const auto payload = m_Browser->BeginDrag(entry);
    if (!payload)
    {
        return;
    }
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
    {
        const AssetDragDropBlob blob = MakeAssetDragDropBlob(*payload);
        ImGui::SetDragDropPayload("FADIX_ASSET", &blob, sizeof(blob));
        ImGui::TextUnformatted(entry.DisplayName.c_str());
        ImGui::EndDragDropSource();
    }
}

void ContentBrowserPanel::ApplySceneDrop(const AssetDragPayload& payload, SceneEditor* scene)
{
    if (m_Browser != nullptr)
    {
        m_Browser->DropInScene(payload);
    }
    if (m_Callbacks.DropInScene)
    {
        m_Callbacks.DropInScene(payload);
    }
    if (scene == nullptr)
    {
        return;
    }
    // Extra assigns when controller callbacks only cover mesh create.
    if (payload.AssetType == "Material")
    {
        static_cast<void>(scene->AssignMaterial(payload.Handle));
    }
    else if (payload.AssetType == "Script")
    {
        static_cast<void>(scene->AssignScript(payload.Handle));
    }
    else if (payload.AssetType == "Prefab")
    {
        if (const auto id = scene->InstantiatePrefab(payload.SourcePath, glm::vec3{0.0F}))
        {
            scene->SetSelection(id, true);
        }
    }
}

void ContentBrowserPanel::ImportPath(const std::filesystem::path& path, EditorUiState& ui)
{
    if (m_Browser == nullptr)
    {
        return;
    }
    m_ImportBusy = true;
    Status("Importing " + path.filename().string() + "...", ui);
    if (IsModelExt(LowerExt(path)))
    {
        if (const Result<std::filesystem::path> result = m_Browser->ImportExternalModel(path))
        {
            Status("Imported " + result.Value().filename().string(), ui);
            if (m_Log)
            {
                m_Log->Log("Imported model " + path.generic_string());
            }
        }
        else
        {
            Error(result.ErrorMessage(), ui);
        }
    }
    else
    {
        static_cast<void>(m_Browser->Import(path));
        Status("Queued import " + path.filename().string(), ui);
    }
    m_ImportBusy = false;
    if (m_Thumbnails)
    {
        m_Thumbnails->Clear();
        ClearGpuThumbs();
    }
}

void ContentBrowserPanel::HandleExternalDrop(const SDL_Event& event, EditorUiState& ui)
{
    if (event.type != SDL_EVENT_DROP_FILE || event.drop.data == nullptr || m_Browser == nullptr)
    {
        return;
    }
    const bool overBrowser = event.drop.x >= m_WindowMin.x && event.drop.x < m_WindowMax.x &&
        event.drop.y >= m_WindowMin.y && event.drop.y < m_WindowMax.y;
    if (!overBrowser)
    {
        return;
    }
    ImportPath(
        std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(event.drop.data)}},
        ui);
}

void ContentBrowserPanel::PromptImportModel(EditorUiState& ui, SDL_Window* window)
{
    if (m_Browser == nullptr || window == nullptr)
    {
        Status("Open a project before importing models", ui);
        return;
    }
    static constexpr SDL_DialogFileFilter filters[] = {
        {"3D Models", "glb;gltf;fbx;blend;obj;stl;ply;dae"},
        {"glTF", "glb;gltf"},
        {"All Files", "*"},
    };
    struct Ctx
    {
        std::shared_ptr<ExternalImportQueue> Imports;
    };
    // ponytail: dialog userdata must outlive callback; heap + delete in callback.
    auto* ctx = new Ctx{m_ExternalImports};
    SDL_ShowOpenFileDialog(
        [](void* userData, const char* const* files, int) {
            auto* c = static_cast<Ctx*>(userData);
            std::vector<std::filesystem::path> paths;
            if (files != nullptr)
            {
                for (std::size_t i = 0; files[i] != nullptr; ++i)
                {
                    paths.emplace_back(
                        std::filesystem::path{std::u8string{
                            reinterpret_cast<const char8_t*>(files[i])}});
                }
            }
            if (!paths.empty())
            {
                std::scoped_lock lock{c->Imports->Mutex};
                c->Imports->Paths.insert(
                    c->Imports->Paths.end(),
                    std::make_move_iterator(paths.begin()),
                    std::make_move_iterator(paths.end()));
            }
            delete c;
        },
        ctx,
        window,
        filters,
        static_cast<int>(std::size(filters)),
        nullptr,
        true);
}

void ContentBrowserPanel::DrainPendingExternalImports(EditorUiState& ui)
{
    const std::shared_ptr<ExternalImportQueue> imports = m_ExternalImports;
    if (!imports)
    {
        return;
    }
    std::vector<std::filesystem::path> paths;
    {
        std::scoped_lock lock{imports->Mutex};
        paths.swap(imports->Paths);
    }
    for (const std::filesystem::path& path : paths)
    {
        ImportPath(path, ui);
    }
}

void ContentBrowserPanel::DrawToolbar(EditorUiState& ui)
{
    if (ImGui::Button(FADIX_ICON_FOLDER " Assets"))
    {
        if (const Result<void> opened = m_Browser->NavigateHome(); !opened)
        {
            Error(opened.ErrorMessage(), ui);
        }
        m_Selected.reset();
    }
    ImGui::SameLine();
    if (ImGui::Button(FADIX_ICON_FOLDER " Up"))
    {
        static_cast<void>(m_Browser->NavigateUp());
        m_Selected.reset();
    }
    ImGui::SameLine();
    if (ImGui::Button(FADIX_ICON_REFRESH " Refresh"))
    {
        if (const Result<void> refreshed = m_Browser->Refresh(); !refreshed)
        {
            Error(refreshed.ErrorMessage(), ui);
        }
        if (m_Thumbnails)
        {
            m_Thumbnails->Clear();
            ClearGpuThumbs();
        }
        m_Selected.reset();
    }
    ImGui::SameLine();
    if (ImGui::Button(m_Grid ? FADIX_ICON_LIST " List" : FADIX_ICON_GRID " Grid"))
    {
        m_Grid = !m_Grid;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0F);
    if (ImGui::InputTextWithHint("##search", FADIX_ICON_SEARCH " Search", m_SearchBuf, sizeof(m_SearchBuf)))
    {
        m_Browser->SetSearch(m_SearchBuf);
        m_Selected.reset();
    }
    if (m_ImportBusy)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("Importing...");
    }
    ImGui::SameLine();
    if (ImGui::Button(FADIX_ICON_PLUS " New"))
    {
        ImGui::OpenPopup("##asset_create");
    }
    if (ImGui::BeginPopup("##asset_create"))
    {
        if (ImGui::MenuItem("Folder"))
        {
            std::snprintf(m_NameBuf, sizeof(m_NameBuf), "New Folder");
            m_PendingNewFolder = true;
        }
        if (ImGui::MenuItem("Scene"))
        {
            std::snprintf(m_NameBuf, sizeof(m_NameBuf), "NewScene");
            m_PendingNewScene = true;
        }
        if (ImGui::MenuItem("Material"))
        {
            if (const Result<AssetHandle> created = m_Browser->CreateMaterial("Material"); !created)
            {
                Error(created.ErrorMessage(), ui);
            }
            else
            {
                Status("Created material", ui);
            }
        }
        if (ImGui::MenuItem("Lua Script"))
        {
            m_PendingScriptLang = ScriptLanguage::Lua;
            std::snprintf(m_NameBuf, sizeof(m_NameBuf), "NewScript");
        }
        if (ImGui::MenuItem("C++ Script"))
        {
            m_PendingScriptLang = ScriptLanguage::Cpp;
            std::snprintf(m_NameBuf, sizeof(m_NameBuf), "NewScript");
        }
        ImGui::EndPopup();
    }
}

void ContentBrowserPanel::DrawBreadcrumbs()
{
    const std::string display = m_Browser->CurrentFolderDisplay();
    const std::size_t total = CountCrumbs(display);
    std::size_t crumb = 0;
    for (std::size_t begin = 0; begin < display.size();)
    {
        const std::size_t end = display.find('/', begin);
        const std::string segment =
            display.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!segment.empty())
        {
            if (crumb > 0)
            {
                ImGui::SameLine();
                ImGui::TextDisabled(">");
                ImGui::SameLine();
            }
            ImGui::PushID(static_cast<int>(crumb));
            if (ImGui::SmallButton(segment.c_str()))
            {
                for (std::size_t i = crumb + 1; i < total; ++i)
                {
                    static_cast<void>(m_Browser->NavigateUp());
                }
                m_Selected.reset();
            }
            ImGui::PopID();
            ++crumb;
        }
        if (end == std::string::npos)
        {
            break;
        }
        begin = end + 1;
    }
}

void ContentBrowserPanel::DrawContextMenu(
    const std::size_t index, const AssetBrowserEntry& entry, EditorUiState& ui)
{
    if (!ImGui::BeginPopupContextItem("##asset_ctx"))
    {
        return;
    }
    m_Selected = index;
    if (ImGui::MenuItem("Open"))
    {
        m_PendingActivate = entry;
    }
    if (ImGui::MenuItem("Rename"))
    {
        m_PendingRename = entry.Path;
        std::snprintf(m_NameBuf, sizeof(m_NameBuf), "%s", entry.DisplayName.c_str());
    }
    if (entry.Kind == AssetBrowserEntryKind::Asset && ImGui::MenuItem("Duplicate"))
    {
        if (const Result<AssetHandle> dup = m_Browser->Duplicate(entry.Path); !dup)
        {
            Error(dup.ErrorMessage(), ui);
        }
    }
    if (entry.Kind == AssetBrowserEntryKind::Asset && entry.AssetType == "Texture" && entry.Handle &&
        ImGui::MenuItem("Reimport"))
    {
        static_cast<void>(m_Browser->ReimportTexture(*entry.Handle));
        Status("Reimporting texture...", ui);
        if (m_Thumbnails)
        {
            m_Thumbnails->Clear();
            ClearGpuThumbs();
        }
    }
    if (ImGui::MenuItem("Reveal in Explorer"))
    {
        if (m_Callbacks.RevealPath)
        {
            m_Callbacks.RevealPath(entry.Path);
        }
    }
    if (ImGui::MenuItem("Copy Path"))
    {
        const std::string path = entry.Path.string();
        ImGui::SetClipboardText(path.c_str());
        Status("Copied asset path", ui);
    }
    if (ImGui::MenuItem("Delete"))
    {
        m_PendingDelete = entry.Path;
    }
    ImGui::EndPopup();
}

void ContentBrowserPanel::DrawEntryRow(
    const std::size_t index, const AssetBrowserEntry& entry, EditorUiState& ui)
{
    ImGui::PushID(static_cast<int>(index));
    const bool selected = m_Selected && *m_Selected == index;
    ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowDoubleClick);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        m_PendingActivate = entry;
    }
    else if (ImGui::IsItemClicked())
    {
        m_Selected = index;
    }
    BeginAssetDrag(entry);
    if (ImGui::BeginDragDropTarget() && entry.Kind == AssetBrowserEntryKind::Folder)
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FADIX_ASSET"))
        {
            if (const auto drag = ParseAssetDragDropBlob(payload->Data, payload->DataSize))
            {
                if (const Result<void> moved = m_Browser->Move(drag->SourcePath, entry.Path); !moved)
                {
                    Error(moved.ErrorMessage(), ui);
                }
                else
                {
                    Status("Moved " + drag->SourcePath.filename().string(), ui);
                    m_Selected.reset();
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    DrawContextMenu(index, entry, ui);

    ImGui::SameLine();
    if (const ImTextureRef thumb = ThumbnailTexture(entry); thumb.GetTexID() != 0)
    {
        ImGui::Image(thumb, ImVec2{18.0F, 18.0F});
        ImGui::SameLine();
    }
    else if (entry.AssetType == "Material" && entry.Handle && m_Thumbnails)
    {
        if (const auto rgb = m_Thumbnails->MaterialSwatchRgb(*entry.Handle))
        {
            int r = 128;
            int g = 128;
            int b = 128;
            static_cast<void>(std::sscanf(rgb->c_str(), "%d,%d,%d", &r, &g, &b));
            ImGui::ColorButton(
                "##sw",
                ImVec4{r / 255.0F, g / 255.0F, b / 255.0F, 1.0F},
                ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                ImVec2{18.0F, 18.0F});
            ImGui::SameLine();
        }
        else
        {
            ImGui::TextUnformatted(TypeGlyph(entry));
            ImGui::SameLine();
        }
    }
    else
    {
        ImGui::TextUnformatted(TypeGlyph(entry));
        ImGui::SameLine();
    }
    ImGui::TextUnformatted(entry.DisplayName.c_str());
    if (!entry.AssetType.empty())
    {
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 80.0F);
        ImGui::TextDisabled("%s", entry.AssetType.c_str());
    }
    ImGui::PopID();
}

void ContentBrowserPanel::DrawEntryGrid(
    const std::size_t index, const AssetBrowserEntry& entry, EditorUiState& ui)
{
    ImGui::PushID(static_cast<int>(index));
    const bool selected = m_Selected && *m_Selected == index;
    ImGui::BeginGroup();
    const ImVec2 thumbSize{72.0F, 72.0F};
    ImGui::PushStyleColor(
        ImGuiCol_Button,
        selected ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
                 : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    if (ImGui::Button("##cell", ImVec2{88.0F, 100.0F}))
    {
        m_Selected = index;
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        m_PendingActivate = entry;
    }
    BeginAssetDrag(entry);
    if (ImGui::BeginDragDropTarget() && entry.Kind == AssetBrowserEntryKind::Folder)
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FADIX_ASSET"))
        {
            if (const auto drag = ParseAssetDragDropBlob(payload->Data, payload->DataSize))
            {
                if (const Result<void> moved = m_Browser->Move(drag->SourcePath, entry.Path); !moved)
                {
                    Error(moved.ErrorMessage(), ui);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    DrawContextMenu(index, entry, ui);

    const ImVec2 min = ImGui::GetItemRectMin();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 thumbMin{min.x + 8.0F, min.y + 6.0F};
    if (const ImTextureRef thumb = ThumbnailTexture(entry); thumb.GetTexID() != 0)
    {
        draw->AddImage(thumb, thumbMin, ImVec2{thumbMin.x + thumbSize.x, thumbMin.y + thumbSize.y});
    }
    else if (entry.AssetType == "Material" && entry.Handle && m_Thumbnails)
    {
        if (const auto rgb = m_Thumbnails->MaterialSwatchRgb(*entry.Handle))
        {
            int r = 128;
            int g = 128;
            int b = 128;
            static_cast<void>(std::sscanf(rgb->c_str(), "%d,%d,%d", &r, &g, &b));
            draw->AddRectFilled(
                thumbMin,
                ImVec2{thumbMin.x + thumbSize.x, thumbMin.y + thumbSize.y},
                IM_COL32(r, g, b, 255));
        }
        else
        {
            draw->AddText(
                ImVec2{thumbMin.x + 20.0F, thumbMin.y + 28.0F},
                ImGui::GetColorU32(ImGuiCol_Text),
                TypeGlyph(entry));
        }
    }
    else
    {
        const char* glyph = TypeGlyph(entry);
        const float iconPx = entry.Kind == AssetBrowserEntryKind::Folder ? 40.0F : 28.0F;
        ImFont* font = ImGui::GetFont();
        const ImVec2 size = font->CalcTextSizeA(iconPx, 1000.0F, 0.0F, glyph);
        draw->AddText(
            font,
            iconPx,
            ImVec2{
                thumbMin.x + (thumbSize.x - size.x) * 0.5F,
                thumbMin.y + (thumbSize.y - size.y) * 0.5F},
            ImGui::GetColorU32(ImGuiCol_Text),
            glyph);
    }
    draw->AddText(
        ImVec2{min.x + 4.0F, min.y + 80.0F},
        ImGui::GetColorU32(ImGuiCol_Text),
        entry.DisplayName.c_str());
    ImGui::EndGroup();
    ImGui::PopID();
}

void ContentBrowserPanel::DrawEntries(EditorUiState& ui)
{
    const auto liveEntries = m_Browser->Entries();
    const std::vector<AssetBrowserEntry> entries{liveEntries.begin(), liveEntries.end()};
    if (entries.empty())
    {
        ImGui::TextDisabled("No assets in this folder");
        return;
    }
    if (m_Grid)
    {
        const float cell = 96.0F;
        const int columns =
            std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cell));
        if (ImGui::BeginTable("##asset_grid", columns))
        {
            for (std::size_t i = 0; i < entries.size(); ++i)
            {
                ImGui::TableNextColumn();
                DrawEntryGrid(i, entries[i], ui);
            }
            ImGui::EndTable();
        }
    }
    else
    {
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            DrawEntryRow(i, entries[i], ui);
        }
    }
}

void ContentBrowserPanel::Draw(EditorUiState& ui, SceneEditor* /*scene*/)
{
    if (!ui.ShowContentBrowser || m_Browser == nullptr)
    {
        m_WindowHovered = false;
        return;
    }
    if (!ImGui::Begin(FADIX_ICON_FOLDER " Content Browser###Content Browser", &ui.ShowContentBrowser))
    {
        m_WindowHovered = false;
        ImGui::End();
        return;
    }
    m_WindowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
    m_WindowMin = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    m_WindowMax = {m_WindowMin.x + windowSize.x, m_WindowMin.y + windowSize.y};

    // SDL file-dialog callbacks are not guaranteed to run on the render thread.
    // Importing here keeps the asset database, UI state, thumbnails, and entry
    // storage strictly owned by the editor's main thread.
    DrainPendingExternalImports(ui);

    if (m_ClearThumbsPending)
    {
        m_ClearThumbsPending = false;
        if (m_Thumbnails)
        {
            m_Thumbnails->Clear();
        }
        ClearGpuThumbs();
    }

    DrawToolbar(ui);
    DrawBreadcrumbs();
    ImGui::Separator();
    DrawEntries(ui);

    // Open folder / asset only after the entry list draw finishes — Activate rebuilds
    // m_Entries and must not run while we still hold spans into it.
    if (m_PendingActivate)
    {
        const AssetBrowserEntry toOpen = *m_PendingActivate;
        m_PendingActivate.reset();
        m_Selected.reset();
        m_Browser->Activate(toOpen);
    }

    if (m_PendingNewFolder && !ImGui::IsPopupOpen("##asset_new_folder"))
    {
        ImGui::OpenPopup("##asset_new_folder");
    }
    if (m_PendingNewScene && !ImGui::IsPopupOpen("##asset_new_scene"))
    {
        ImGui::OpenPopup("##asset_new_scene");
    }
    if (m_PendingScriptLang && !ImGui::IsPopupOpen("##asset_new_script"))
    {
        ImGui::OpenPopup("##asset_new_script");
    }
    if (m_PendingRename && !ImGui::IsPopupOpen("##asset_rename"))
    {
        ImGui::OpenPopup("##asset_rename");
    }
    if (m_PendingDelete && !ImGui::IsPopupOpen("##asset_delete"))
    {
        ImGui::OpenPopup("##asset_delete");
    }

    if (ImGui::BeginPopupModal("##asset_new_folder", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("Name", m_NameBuf, sizeof(m_NameBuf));
        if (ImGui::Button("Create"))
        {
            if (const Result<void> created = m_Browser->CreateFolder(m_NameBuf); !created)
            {
                Error(created.ErrorMessage(), ui);
            }
            else
            {
                Status(std::string{"Created folder "} + m_NameBuf, ui);
            }
            m_PendingNewFolder = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_PendingNewFolder = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("##asset_new_scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("Name", m_NameBuf, sizeof(m_NameBuf));
        if (ImGui::Button("Create"))
        {
            std::filesystem::path name{m_NameBuf};
            if (name.empty() || name.filename() != name || name == "." || name == "..")
            {
                Error("Scene name is empty or contains path separators", ui);
            }
            else if (!m_Callbacks.CreateScene)
            {
                Error("Scene creation is not available", ui);
            }
            else
            {
                if (LowerExt(name) != ".scene" && LowerExt(name) != ".fadixscene")
                {
                    name += ".scene";
                }
                const std::filesystem::path path = m_Browser->CurrentFolder() / name;
                std::error_code error;
                if (std::filesystem::exists(path, error))
                {
                    Error("A scene with that name already exists", ui);
                }
                else
                {
                    m_Callbacks.CreateScene(path);
                    m_PendingNewScene = false;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_PendingNewScene = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("##asset_new_script", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("Name", m_NameBuf, sizeof(m_NameBuf));
        if (ImGui::Button("Create"))
        {
            if (m_Scripts == nullptr)
            {
                Error("Script database is not available", ui);
            }
            else if (
                m_Scripts->CreateNew(
                    m_NameBuf,
                    m_PendingScriptLang.value_or(ScriptLanguage::Lua),
                    m_Browser->CurrentFolder()) == nullptr)
            {
                Error("Could not create script", ui);
            }
            else
            {
                static_cast<void>(m_Browser->Refresh());
                Status(std::string{"Created script "} + m_NameBuf, ui);
            }
            m_PendingScriptLang.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_PendingScriptLang.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("##asset_rename", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("Name", m_NameBuf, sizeof(m_NameBuf));
        if (ImGui::Button("Rename") && m_PendingRename)
        {
            if (const Result<void> renamed = m_Browser->Rename(*m_PendingRename, m_NameBuf); !renamed)
            {
                Error(renamed.ErrorMessage(), ui);
            }
            m_PendingRename.reset();
            m_Selected.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_PendingRename.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("##asset_delete", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text(
            "Delete %s?",
            m_PendingDelete ? m_PendingDelete->filename().string().c_str() : "?");
        if (ImGui::Button("Delete") && m_PendingDelete)
        {
            ApplyControllerCallbacks(true);
            if (const Result<void> deleted = m_Browser->Delete(*m_PendingDelete); !deleted)
            {
                Error(deleted.ErrorMessage(), ui);
            }
            else
            {
                Status("Deleted asset", ui);
            }
            ApplyControllerCallbacks(false);
            m_PendingDelete.reset();
            m_Selected.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_PendingDelete.reset();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}
}
