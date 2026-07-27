#include "editor/imgui/panels/MaterialEditorPanel.hpp"

#include "editor/imgui/EditorIcons.hpp"
#include "editor/assets/AssetBrowserController.hpp"
#include "engine/assets/MaterialAsset.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdint>

namespace fadix::editor
{
namespace
{
[[nodiscard]] ImTextureRef PreviewTex(const SceneViewSource& source)
{
    if (source.Texture == nullptr)
    {
        return {};
    }
    return ImTextureRef{static_cast<ImTextureID>(reinterpret_cast<intptr_t>(source.Texture))};
}

const char* AlphaLabel(const AlphaMode mode)
{
    switch (mode)
    {
    case AlphaMode::Mask: return "Mask";
    case AlphaMode::Blend: return "Blend";
    case AlphaMode::Opaque: break;
    }
    return "Opaque";
}
}

void MaterialEditorPanel::Bind(MaterialEditorController& editor)
{
    m_Editor = &editor;
}

void MaterialEditorPanel::DrawTextureSlot(
    const char* label, const char* slot, EditorUiState& ui)
{
    ImGui::PushID(slot);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_Editor->TextureSlotDisplayName(slot).c_str());
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FADIX_ASSET"))
        {
            if (const auto asset = ParseAssetDragDropBlob(
                    payload->Data, static_cast<std::size_t>(payload->DataSize)))
            {
                if (asset->AssetType == "Texture" &&
                    m_Editor->SetTextureSlot(slot, asset->Handle))
                {
                    ui.StatusText = std::string{"Assigned "} + label;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear") && m_Editor->ClearTextureSlot(slot))
    {
        ui.StatusText = std::string{"Cleared "} + label;
    }
    ImGui::PopID();
}

void MaterialEditorPanel::DrawPreview()
{
    ImGui::BeginChild("##mat_preview", ImVec2{280.0F, 0.0F}, ImGuiChildFlags_Borders);
    if (ImGui::Button("Cube"))
    {
        m_Editor->SetPreviewShape(0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Sphere"))
    {
        m_Editor->SetPreviewShape(1);
    }
    ImGui::SameLine();
    if (ImGui::Button("Plane"))
    {
        m_Editor->SetPreviewShape(2);
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const auto width = static_cast<std::uint32_t>(std::clamp(avail.x, 8.0F, 2048.0F));
    const auto height = static_cast<std::uint32_t>(std::clamp(avail.y, 8.0F, 2048.0F));
    m_Editor->RenderPreview(width, height);
    const SceneViewSource source = m_Editor->PreviewSource();
    if (const ImTextureRef tex = PreviewTex(source); tex.GetTexID() != 0)
    {
        ImGui::Image(tex, avail, ImVec2{0.0F, 0.0F}, ImVec2{1.0F, 1.0F});
    }
    else
    {
        ImGui::Dummy(avail);
        ImGui::TextDisabled("Preview");
    }
    ImGui::EndChild();
}

void MaterialEditorPanel::DrawFields(EditorUiState& ui)
{
    MaterialAsset& mat = m_Editor->Editing();
    auto drag = [&](const char* label, const char* id, float* value, float speed = 0.01F) {
        if (ImGui::DragFloat(label, value, speed, 0.0F, 0.0F, "%.4g"))
        {
            m_Editor->SetFloatField(id, *value);
        }
    };

    ImGui::SeparatorText("Base Color");
    drag("R", "mat-ed-basecolor-r", &mat.BaseColor.r);
    drag("G", "mat-ed-basecolor-g", &mat.BaseColor.g);
    drag("B", "mat-ed-basecolor-b", &mat.BaseColor.b);
    drag("A", "mat-ed-basecolor-a", &mat.BaseColor.a);
    DrawTextureSlot("Base Color Tex", "basecolor", ui);

    ImGui::SeparatorText("PBR");
    drag("Metallic", "mat-ed-metallic", &mat.Metallic);
    drag("Roughness", "mat-ed-roughness", &mat.Roughness);
    DrawTextureSlot("Metallic-Roughness", "mr", ui);
    DrawTextureSlot("Normal", "normal", ui);

    ImGui::SeparatorText("Emissive");
    drag("Er", "mat-ed-emissive-r", &mat.EmissiveColor.r);
    drag("Eg", "mat-ed-emissive-g", &mat.EmissiveColor.g);
    drag("Eb", "mat-ed-emissive-b", &mat.EmissiveColor.b);
    drag("Intensity", "mat-ed-emissive-intensity", &mat.EmissiveIntensity, 0.05F);
    DrawTextureSlot("Emissive Tex", "emissive", ui);

    ImGui::SeparatorText("Alpha / Sides");
    drag("Cutoff", "mat-ed-alpha-cutoff", &mat.AlphaCutoff);
    if (ImGui::Button(AlphaLabel(mat.AlphaMode)))
    {
        m_Editor->CycleAlphaMode();
    }
    ImGui::SameLine();
    if (ImGui::Button(mat.DoubleSided ? "Double Sided: On" : "Double Sided: Off"))
    {
        m_Editor->ToggleDoubleSided();
    }

    ImGui::SeparatorText("UV");
    drag("Scale X", "mat-ed-uv-scale-x", &mat.UVScale.x);
    drag("Scale Y", "mat-ed-uv-scale-y", &mat.UVScale.y);
    drag("Offset X", "mat-ed-uv-offset-x", &mat.UVOffset.x);
    drag("Offset Y", "mat-ed-uv-offset-y", &mat.UVOffset.y);
}

void MaterialEditorPanel::Draw(EditorUiState& ui)
{
    if (!ui.ShowMaterialEditor || m_Editor == nullptr)
    {
        return;
    }

    if (!ImGui::Begin(FADIX_ICON_PALETTE " Material Editor###Material Editor", &ui.ShowMaterialEditor))
    {
        ImGui::End();
        return;
    }
    if (!ui.ShowMaterialEditor)
    {
        m_Editor->Close();
        ImGui::End();
        return;
    }

    if (!m_Editor->IsOpen())
    {
        ImGui::TextDisabled("Open a Material asset from the Content Browser, or drop one here");
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FADIX_ASSET"))
            {
                if (const auto asset = ParseAssetDragDropBlob(
                        payload->Data, static_cast<std::size_t>(payload->DataSize)))
                {
                    if (asset->AssetType == "Material")
                    {
                        m_Editor->Open(asset->Handle);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::End();
        return;
    }

    if (ImGui::Button("Close"))
    {
        m_Editor->Close();
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", m_Editor->Current() ? m_Editor->Current()->ToString().c_str() : "");

    DrawPreview();
    ImGui::SameLine();
    if (ImGui::BeginChild("##mat_fields", ImVec2{0.0F, 0.0F}, ImGuiChildFlags_Borders))
    {
        DrawFields(ui);
    }
    ImGui::EndChild();
    ImGui::End();
}
}
