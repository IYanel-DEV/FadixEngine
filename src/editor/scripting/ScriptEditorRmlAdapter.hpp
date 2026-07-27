#pragma once

#include "editor/scripting/ScriptEditorController.hpp"

namespace Rml
{
class ElementDocument;
class Event;
}

namespace fadix::editor
{
/// Temporary RmlUi DOM binder for ScriptEditorController. Remove when Rml shell dies.
class ScriptEditorRmlAdapter final
{
public:
    void Bind(Rml::ElementDocument& document, ScriptEditorController& editor);
    void Sync();
    void HandleListClick(Rml::Event& event);

private:
    Rml::ElementDocument* m_Document{nullptr};
    ScriptEditorController* m_Editor{nullptr};
};
}
