#pragma once

#include "engine/app/IApplicationHost.hpp"
#include "engine/project/IProjectService.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Rml
{
class Context;
class ElementDocument;
class Event;
class EventListener;
}

namespace fadix
{
class ProjectService;

/// RmlUi binder for assets/editor/project_manager.rml.
/// Integration wire notes (EditorApplication — do not apply from this module):
///   SetProjectService(project::CreateService());  // or fadix::CreateProjectService()
///   m_ProjectManager.Connect(*this, Projects());
///   // after LoadDocument("project_manager.rml"):
///   m_ProjectManager.Bind(m_Context);
///   m_ProjectManager.OnDocumentLoaded(m_Document);
///   // forward PM clicks to m_ProjectManager.HandleEvent(event); remove NullProjectService stubs
class ProjectManagerController
{
public:
    ProjectManagerController();
    ~ProjectManagerController();

    void Connect(IApplicationHost& host, IProjectService& projects);
    void Bind(Rml::Context* context);
    void OnDocumentLoaded(Rml::ElementDocument* document);
    void HandleEvent(Rml::Event& event);

private:
    class Listener;

    void BindButtons();
    void RefreshRecents();
    void SetStatus(std::string_view text);
    void SetSelectedRecent(std::size_t index);
    void UpdateSelectionChrome();
    void UpdateActionAvailability();
    [[nodiscard]] ProjectService* Service() noexcept;
    [[nodiscard]] std::string ReadInputValue(const char* elementId) const;
    void WriteInputValue(const char* elementId, std::string_view value);
    void OnCreate();
    void OnBrowseParent();
    void OnOpen();
    void OnBrowseOpen();
    void OnOpenSelected();
    void OnRemoveSelected();
    void OnRenameSelected();
    void OnRevealSelected();
    void OnSelectTemplate(ProjectTemplate projectTemplate);
    [[nodiscard]] static std::string EscapeRml(std::string_view text);

    IApplicationHost* m_Host{nullptr};
    IProjectService* m_Projects{nullptr};
    Rml::Context* m_Context{nullptr};
    Rml::ElementDocument* m_Document{nullptr};
    std::unique_ptr<Listener> m_Listener;
    ProjectTemplate m_CreateTemplate{ProjectTemplate::Empty3D};
    std::optional<std::size_t> m_SelectedRecent;
};
}
