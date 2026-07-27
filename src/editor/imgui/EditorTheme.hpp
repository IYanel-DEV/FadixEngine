#pragma once

#include <imgui.h>

#include <filesystem>
#include <string>

struct SDL_GPUDevice;
struct SDL_GPUTexture;

namespace fadix::editor
{
/// Fadix colors/fonts from assets/editor/workspace.rcss (not Laura).
class EditorTheme final
{
public:
    EditorTheme() = default;
    ~EditorTheme();

    EditorTheme(const EditorTheme&) = delete;
    EditorTheme& operator=(const EditorTheme&) = delete;

    void Apply();
    [[nodiscard]] bool LoadFonts(const std::filesystem::path& assetRoot);
    [[nodiscard]] bool LoadLogo(SDL_GPUDevice* device, const std::filesystem::path& assetRoot);
    void ReleaseLogo(SDL_GPUDevice* device);

    [[nodiscard]] ImTextureRef Logo() const;
    [[nodiscard]] bool HasLogo() const noexcept { return m_Logo != nullptr; }

    // workspace.rcss palette
    ImVec4 MainBackground{0.118F, 0.118F, 0.133F, 1.0F};      // #1e1e22
    ImVec4 Panel{0.145F, 0.145F, 0.157F, 1.0F};               // #252528
    ImVec4 Header{0.165F, 0.165F, 0.180F, 1.0F};              // #2a2a2e
    ImVec4 Brand{0.133F, 0.133F, 0.149F, 1.0F};               // #222226
    ImVec4 Accent{0.337F, 0.612F, 0.839F, 1.0F};              // #569cd6
    ImVec4 Text{0.800F, 0.800F, 0.800F, 1.0F};                // #cccccc
    ImVec4 TextMuted{0.502F, 0.502F, 0.525F, 1.0F};           // #808086
    ImVec4 TextBright{0.878F, 0.878F, 0.878F, 1.0F};          // #e0e0e0
    ImVec4 Border{0.094F, 0.094F, 0.110F, 1.0F};              // #18181c
    ImVec4 BorderLight{0.227F, 0.227F, 0.251F, 1.0F};         // #3a3a40
    ImVec4 Hover{0.165F, 0.180F, 0.204F, 1.0F};               // #2a2e34
    ImVec4 Active{0.235F, 0.235F, 0.259F, 1.0F};              // #3c3c42
    ImVec4 Selection{0.149F, 0.310F, 0.471F, 1.0F};           // #264f78
    ImVec4 Tool{0.200F, 0.200F, 0.220F, 1.0F};                // #333338
    ImVec4 Warning{0.863F, 0.863F, 0.667F, 1.0F};             // #dcdcaa
    ImVec4 Error{0.957F, 0.278F, 0.278F, 1.0F};               // #f44747
    ImVec4 Info{0.337F, 0.612F, 0.839F, 1.0F};                // #569cd6
    ImVec4 Play{0.647F, 0.812F, 0.604F, 1.0F};                // #a5cf9a
    ImVec4 PlayRunning{0.290F, 0.478F, 0.227F, 1.0F};         // #4a7a3a
    ImVec4 PlayBorder{0.275F, 0.439F, 0.227F, 1.0F};          // #46703a

private:
    SDL_GPUTexture* m_Logo{nullptr};
    int m_LogoWidth{0};
    int m_LogoHeight{0};
};
}
