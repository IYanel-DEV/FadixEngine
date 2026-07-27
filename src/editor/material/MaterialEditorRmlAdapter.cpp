#include "editor/material/MaterialEditorRmlAdapter.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <array>
#include <cstdio>
#include <string>

namespace fadix::editor
{
namespace
{
std::string FormatValue(const float value)
{
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.4g", value);
    return buffer.data();
}

std::string FormatLabel(const float value)
{
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.2f", value);
    return buffer.data();
}

const char* AlphaModeName(const AlphaMode mode)
{
    switch (mode)
    {
    case AlphaMode::Mask: return "Mask";
    case AlphaMode::Blend: return "Blend";
    case AlphaMode::Opaque: break;
    }
    return "Opaque";
}

Rml::Element* Elem(Rml::ElementDocument* doc, const char* id)
{
    return doc == nullptr ? nullptr : doc->GetElementById(id);
}

void SetInput(Rml::ElementDocument* doc, const char* id, const float value)
{
    if (auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(Elem(doc, id)))
    {
        control->SetValue(FormatValue(value));
    }
}

float ReadInput(Rml::ElementDocument* doc, const char* id, const float fallback)
{
    if (auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(Elem(doc, id)))
    {
        try
        {
            return std::stof(std::string{control->GetValue()});
        }
        catch (...)
        {
        }
    }
    return fallback;
}

void SetLabel(Rml::ElementDocument* doc, const char* id, const float value)
{
    if (Rml::Element* element = Elem(doc, id))
    {
        element->SetInnerRML(FormatLabel(value));
    }
}

void SetToggleLabel(Rml::ElementDocument* doc, const char* id, const char* label)
{
    if (Rml::Element* element = Elem(doc, id))
    {
        element->SetInnerRML(std::string{"<span class=\"button-label\">"} + label + "</span>");
    }
}

void RefreshTextureSlot(
    Rml::ElementDocument* doc,
    MaterialEditorController& editor,
    const char* slot)
{
    const std::string nameId = std::string{"mat-ed-tex-"} + slot + "-name";
    const std::string clearId = std::string{"mat-ed-tex-"} + slot + "-clear";
    const std::string display = editor.TextureSlotDisplayName(slot);
    const bool assigned = display != "None";
    if (Rml::Element* name = Elem(doc, nameId.c_str()))
    {
        name->SetInnerRML(display);
    }
    if (Rml::Element* clear = Elem(doc, clearId.c_str()))
    {
        clear->SetClass("hidden", !assigned);
    }
}
}

void MaterialEditorRmlAdapter::Bind(
    Rml::ElementDocument& document, MaterialEditorController& editor)
{
    m_Document = &document;
    m_Editor = &editor;
    editor.SetUiNotify([this]() { Sync(); });
    Sync();
}

void MaterialEditorRmlAdapter::Sync()
{
    if (m_Document == nullptr || m_Editor == nullptr)
    {
        return;
    }
    if (Rml::Element* backdrop = Elem(m_Document, "material-editor-backdrop"))
    {
        backdrop->SetClass("hidden", !m_Editor->IsOpen());
    }
    if (!m_Editor->IsOpen())
    {
        return;
    }
    const MaterialAsset& mat = m_Editor->Editing();
    SetInput(m_Document, "mat-ed-basecolor-r", mat.BaseColor.r);
    SetInput(m_Document, "mat-ed-basecolor-g", mat.BaseColor.g);
    SetInput(m_Document, "mat-ed-basecolor-b", mat.BaseColor.b);
    SetInput(m_Document, "mat-ed-basecolor-a", mat.BaseColor.a);
    SetInput(m_Document, "mat-ed-metallic", mat.Metallic);
    SetLabel(m_Document, "mat-ed-metallic-val", mat.Metallic);
    SetInput(m_Document, "mat-ed-roughness", mat.Roughness);
    SetLabel(m_Document, "mat-ed-roughness-val", mat.Roughness);
    SetInput(m_Document, "mat-ed-emissive-r", mat.EmissiveColor.r);
    SetInput(m_Document, "mat-ed-emissive-g", mat.EmissiveColor.g);
    SetInput(m_Document, "mat-ed-emissive-b", mat.EmissiveColor.b);
    SetInput(m_Document, "mat-ed-emissive-intensity", mat.EmissiveIntensity);
    SetInput(m_Document, "mat-ed-alpha-cutoff", mat.AlphaCutoff);
    SetLabel(m_Document, "mat-ed-alpha-cutoff-val", mat.AlphaCutoff);
    SetToggleLabel(m_Document, "mat-ed-alpha-mode", AlphaModeName(mat.AlphaMode));
    SetToggleLabel(m_Document, "mat-ed-double-sided", mat.DoubleSided ? "On" : "Off");
    SetInput(m_Document, "mat-ed-uv-scale-x", mat.UVScale.x);
    SetInput(m_Document, "mat-ed-uv-scale-y", mat.UVScale.y);
    SetInput(m_Document, "mat-ed-uv-offset-x", mat.UVOffset.x);
    SetInput(m_Document, "mat-ed-uv-offset-y", mat.UVOffset.y);
    RefreshTextureSlot(m_Document, *m_Editor, "basecolor");
    RefreshTextureSlot(m_Document, *m_Editor, "normal");
    RefreshTextureSlot(m_Document, *m_Editor, "mr");
    RefreshTextureSlot(m_Document, *m_Editor, "emissive");

    const int shape = m_Editor->PreviewShape();
    const std::array<std::pair<const char*, int>, 3> buttons{{
        {"mat-preview-cube", 0}, {"mat-preview-sphere", 1}, {"mat-preview-plane", 2}}};
    for (const auto& [id, index] : buttons)
    {
        if (Rml::Element* button = Elem(m_Document, id))
        {
            button->SetClass("active", index == shape);
        }
    }
}

void MaterialEditorRmlAdapter::HandleInput(Rml::Event& event)
{
    if (m_Editor == nullptr || !m_Editor->IsOpen() || event.GetCurrentElement() == nullptr)
    {
        return;
    }
    const std::string id = event.GetCurrentElement()->GetId();
    m_Editor->SetFloatField(id, ReadInput(m_Document, id.c_str(), 0.0F));
}
}
