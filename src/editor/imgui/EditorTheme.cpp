#include "editor/imgui/EditorTheme.hpp"

#include "editor/imgui/EditorIcons.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3_image/SDL_image.h>

#include <cstdint>
#include <cstring>

namespace fadix::editor
{

EditorTheme::~EditorTheme() = default;

void EditorTheme::Apply()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0F;
    style.ChildRounding = 3.0F;
    style.FrameRounding = 3.0F;
    style.PopupRounding = 3.0F;
    style.ScrollbarRounding = 3.0F;
    style.GrabRounding = 2.0F;
    style.TabRounding = 0.0F;
    style.WindowBorderSize = 1.0F;
    style.FrameBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.WindowPadding = ImVec2{8.0F, 8.0F};
    style.FramePadding = ImVec2{8.0F, 4.0F};
    style.ItemSpacing = ImVec2{8.0F, 4.0F};
    style.ScrollbarSize = 12.0F;
    style.GrabMinSize = 10.0F;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = Text;
    colors[ImGuiCol_TextDisabled] = TextMuted;
    colors[ImGuiCol_WindowBg] = MainBackground;
    colors[ImGuiCol_ChildBg] = Panel;
    colors[ImGuiCol_PopupBg] = Panel;
    colors[ImGuiCol_Border] = BorderLight;
    colors[ImGuiCol_BorderShadow] = ImVec4{0.0F, 0.0F, 0.0F, 0.0F};
    colors[ImGuiCol_FrameBg] = MainBackground;
    colors[ImGuiCol_FrameBgHovered] = Hover;
    colors[ImGuiCol_FrameBgActive] = Active;
    colors[ImGuiCol_TitleBg] = Header;
    colors[ImGuiCol_TitleBgActive] = Header;
    colors[ImGuiCol_TitleBgCollapsed] = Header;
    colors[ImGuiCol_MenuBarBg] = Header;
    colors[ImGuiCol_ScrollbarBg] = MainBackground;
    colors[ImGuiCol_ScrollbarGrab] = Tool;
    colors[ImGuiCol_ScrollbarGrabHovered] = Active;
    colors[ImGuiCol_ScrollbarGrabActive] = Accent;
    colors[ImGuiCol_CheckMark] = Accent;
    colors[ImGuiCol_SliderGrab] = Accent;
    colors[ImGuiCol_SliderGrabActive] = Accent;
    colors[ImGuiCol_Button] = Tool;
    colors[ImGuiCol_ButtonHovered] = Hover;
    colors[ImGuiCol_ButtonActive] = Active;
    colors[ImGuiCol_Header] = Header;
    colors[ImGuiCol_HeaderHovered] = Hover;
    colors[ImGuiCol_HeaderActive] = Selection;
    colors[ImGuiCol_Separator] = Border;
    colors[ImGuiCol_SeparatorHovered] = Accent;
    colors[ImGuiCol_SeparatorActive] = Accent;
    colors[ImGuiCol_ResizeGrip] = Tool;
    colors[ImGuiCol_ResizeGripHovered] = Accent;
    colors[ImGuiCol_ResizeGripActive] = Accent;
    colors[ImGuiCol_Tab] = Header;
    colors[ImGuiCol_TabHovered] = Hover;
    colors[ImGuiCol_TabSelected] = Panel;
    colors[ImGuiCol_TabDimmed] = Header;
    colors[ImGuiCol_TabDimmedSelected] = Panel;
    colors[ImGuiCol_DockingPreview] = ImVec4{Accent.x, Accent.y, Accent.z, 0.55F};
    colors[ImGuiCol_DockingEmptyBg] = MainBackground;
    colors[ImGuiCol_PlotLines] = Accent;
    colors[ImGuiCol_PlotHistogram] = Accent;
    colors[ImGuiCol_TableHeaderBg] = Header;
    colors[ImGuiCol_TableBorderStrong] = Border;
    colors[ImGuiCol_TableBorderLight] = BorderLight;
    colors[ImGuiCol_TableRowBg] = Panel;
    colors[ImGuiCol_TableRowBgAlt] = ImVec4{0.141F, 0.141F, 0.153F, 1.0F}; // #242427
    colors[ImGuiCol_TextSelectedBg] = Selection;
    colors[ImGuiCol_DragDropTarget] = Accent;
    colors[ImGuiCol_NavCursor] = Accent;
    colors[ImGuiCol_NavWindowingHighlight] = Accent;
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4{0.0F, 0.0F, 0.0F, 0.55F};
}

bool EditorTheme::LoadFonts(const std::filesystem::path& assetRoot)
{
    ImGuiIO& io = ImGui::GetIO();
    const std::filesystem::path fontPath = assetRoot / "fonts" / "LatoLatin-Regular.ttf";
    ImFontConfig config{};
    config.OversampleH = 2;
    config.OversampleV = 2;
    config.PixelSnapH = true;
    ImFont* font = io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), 15.0F, &config);
    if (font == nullptr)
    {
        io.Fonts->AddFontDefault();
        return false;
    }

    // Official Font Awesome Free Solid (FortAwesome release) merged for editor icons.
    const std::filesystem::path iconPath =
        assetRoot / "fonts" / "FontAwesome7Free-Solid-900.otf";
    static const ImWchar iconRanges[] = {FADIX_ICON_MIN_FA, FADIX_ICON_MAX_FA, 0};
    ImFontConfig icons{};
    icons.MergeMode = true;
    icons.PixelSnapH = true;
    icons.GlyphMinAdvanceX = 15.0F;
    icons.OversampleH = 2;
    icons.OversampleV = 2;
    if (io.Fonts->AddFontFromFileTTF(iconPath.string().c_str(), 15.0F, &icons, iconRanges) ==
        nullptr)
    {
        return false;
    }
    return true;
}

bool EditorTheme::LoadLogo(SDL_GPUDevice* device, const std::filesystem::path& assetRoot)
{
    ReleaseLogo(device);
    if (device == nullptr)
    {
        return false;
    }

    const std::filesystem::path logoPath = assetRoot / "editor" / "icons" / "fadix-logo.png";
    SDL_Surface* loaded = IMG_Load(logoPath.string().c_str());
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
    m_Logo = SDL_CreateGPUTexture(device, &info);
    if (m_Logo == nullptr)
    {
        SDL_DestroySurface(rgba);
        return false;
    }

    const std::size_t bytes = static_cast<std::size_t>(rgba->pitch) * static_cast<std::size_t>(rgba->h);
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(bytes);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
    if (transfer == nullptr)
    {
        SDL_ReleaseGPUTexture(device, m_Logo);
        m_Logo = nullptr;
        SDL_DestroySurface(rgba);
        return false;
    }

    void* mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (mapped == nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUTexture(device, m_Logo);
        m_Logo = nullptr;
        SDL_DestroySurface(rgba);
        return false;
    }
    std::memcpy(mapped, rgba->pixels, bytes);
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
    if (commands == nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        SDL_ReleaseGPUTexture(device, m_Logo);
        m_Logo = nullptr;
        SDL_DestroySurface(rgba);
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = transfer;
    SDL_GPUTextureRegion dest{};
    dest.texture = m_Logo;
    dest.w = info.width;
    dest.h = info.height;
    dest.d = 1;
    SDL_UploadToGPUTexture(copy, &source, &dest, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(commands);
    SDL_WaitForGPUIdle(device);

    SDL_ReleaseGPUTransferBuffer(device, transfer);
    m_LogoWidth = rgba->w;
    m_LogoHeight = rgba->h;
    SDL_DestroySurface(rgba);
    return true;
}

void EditorTheme::ReleaseLogo(SDL_GPUDevice* device)
{
    if (m_Logo != nullptr && device != nullptr)
    {
        SDL_ReleaseGPUTexture(device, m_Logo);
    }
    m_Logo = nullptr;
    m_LogoWidth = 0;
    m_LogoHeight = 0;
}

ImTextureRef EditorTheme::Logo() const
{
    return ImTextureRef{static_cast<ImTextureID>(reinterpret_cast<intptr_t>(m_Logo))};
}
}
