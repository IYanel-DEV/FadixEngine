#pragma once

#include "editor/material/MaterialEditorController.hpp"

namespace Rml
{
class ElementDocument;
class Event;
}

namespace fadix::editor
{
/// Temporary RmlUi DOM binder for MaterialEditorController.
class MaterialEditorRmlAdapter final
{
public:
    void Bind(Rml::ElementDocument& document, MaterialEditorController& editor);
    void Sync();
    void HandleInput(Rml::Event& event);

private:
    Rml::ElementDocument* m_Document{nullptr};
    MaterialEditorController* m_Editor{nullptr};
};
}
