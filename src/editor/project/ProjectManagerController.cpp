#include "editor/project/ProjectManagerController.hpp"

#include "engine/app/WorkbenchLayoutIds.hpp"
#include "project/ProjectService.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <sstream>

namespace fadix
{
namespace
{
constexpr char CreateNameId[] = "create-name";
constexpr char CreatePathId[] = "create-path";
constexpr char OpenPathId[] = "open-path";
constexpr char RenameNameId[] = "rename-name";
constexpr char Template2dId[] = "template-2d";
constexpr char Template3dId[] = "template-3d";
constexpr char BtnBrowseParent[] = "btn-browse-parent";
constexpr char BtnBrowseOpen[] = "btn-browse-open";
constexpr char BtnOpenSelected[] = "btn-open-selected";
constexpr char BtnRemove[] = "btn-remove";
constexpr char BtnRename[] = "btn-rename";
constexpr char BtnReveal[] = "btn-reveal";

[[nodiscard]] Rml::String ResolveCommandId(Rml::Event& event)
{
    // Prefer a recent-* ancestor so nested name/meta hits still select the row.
    for (Rml::Element* element = event.GetTargetElement(); element != nullptr;
         element = element->GetParentNode())
    {
        const Rml::String id = element->GetId();
        if (id.size() >= 7 && id.compare(0, 7, "recent-") == 0)
        {
            return id;
        }
        if (element == event.GetCurrentElement())
        {
            break;
        }
    }
    return event.GetCurrentElement()->GetId();
}
}

class ProjectManagerController::Listener final : public Rml::EventListener
{
public:
    explicit Listener(ProjectManagerController& controller) : m_Controller(controller) {}
    void ProcessEvent(Rml::Event& event) override { m_Controller.HandleEvent(event); }

private:
    ProjectManagerController& m_Controller;
};

ProjectManagerController::ProjectManagerController()
    : m_Listener(std::make_unique<Listener>(*this))
{
}

ProjectManagerController::~ProjectManagerController() = default;

void ProjectManagerController::Connect(IApplicationHost& host, IProjectService& projects)
{
    m_Host = &host;
    m_Projects = &projects;
}

void ProjectManagerController::Bind(Rml::Context* context)
{
    m_Context = context;
}

void ProjectManagerController::OnDocumentLoaded(Rml::ElementDocument* document)
{
    m_Document = document;
    if (m_Document == nullptr)
    {
        return;
    }
    if (Service() != nullptr)
    {
        WriteInputValue(CreatePathId, Service()->DefaultProjectsDirectory().string());
    }
    BindButtons();
    OnSelectTemplate(m_CreateTemplate);
    RefreshRecents();
    UpdateActionAvailability();
    SetStatus("Select or create a project.");
}

void ProjectManagerController::BindButtons()
{
    if (m_Document == nullptr || m_Listener == nullptr)
    {
        return;
    }
    for (const char* id : {
             layout::NewProjectButton,
             layout::OpenProjectButton,
             BtnBrowseParent,
             BtnBrowseOpen,
             BtnOpenSelected,
             BtnRemove,
             BtnRename,
             BtnReveal,
             Template2dId,
             Template3dId,
             layout::RecentList})
    {
        if (Rml::Element* element = m_Document->GetElementById(id))
        {
            element->AddEventListener(Rml::EventId::Click, m_Listener.get());
            if (id == layout::RecentList)
            {
                element->AddEventListener(Rml::EventId::Dblclick, m_Listener.get());
            }
        }
    }
}

void ProjectManagerController::HandleEvent(Rml::Event& event)
{
    if (m_Document == nullptr || m_Projects == nullptr)
    {
        return;
    }
    const Rml::String id = ResolveCommandId(event);
    if (id == layout::NewProjectButton)
    {
        OnCreate();
    }
    else if (id == layout::OpenProjectButton)
    {
        OnOpen();
    }
    else if (id == BtnBrowseParent)
    {
        OnBrowseParent();
    }
    else if (id == BtnBrowseOpen)
    {
        OnBrowseOpen();
    }
    else if (id == BtnOpenSelected)
    {
        OnOpenSelected();
    }
    else if (id == BtnRemove)
    {
        OnRemoveSelected();
    }
    else if (id == BtnRename)
    {
        OnRenameSelected();
    }
    else if (id == BtnReveal)
    {
        OnRevealSelected();
    }
    else if (id == Template2dId)
    {
        OnSelectTemplate(ProjectTemplate::Empty2D);
    }
    else if (id == Template3dId)
    {
        OnSelectTemplate(ProjectTemplate::Empty3D);
    }
    else if (id.size() >= 7 && id.compare(0, 7, "recent-") == 0)
    {
        try
        {
            const std::size_t index = static_cast<std::size_t>(std::stoul(std::string{id.substr(7)}));
            SetSelectedRecent(index);
            if (event.GetId() == Rml::EventId::Dblclick)
            {
                OnOpenSelected();
            }
        }
        catch (...)
        {
            SetStatus("Invalid recent project selection");
        }
    }
}

ProjectService* ProjectManagerController::Service() noexcept
{
    return dynamic_cast<ProjectService*>(m_Projects);
}

std::string ProjectManagerController::EscapeRml(const std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char ch : text)
    {
        switch (ch)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out.push_back(ch);
            break;
        }
    }
    return out;
}

std::string ProjectManagerController::ReadInputValue(const char* elementId) const
{
    if (m_Document == nullptr)
    {
        return {};
    }
    Rml::Element* element = m_Document->GetElementById(elementId);
    if (element == nullptr)
    {
        return {};
    }
    if (auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(element))
    {
        return std::string{control->GetValue()};
    }
    return std::string{element->GetAttribute("value", Rml::String{})};
}

void ProjectManagerController::WriteInputValue(const char* elementId, const std::string_view value)
{
    if (m_Document == nullptr)
    {
        return;
    }
    if (Rml::Element* element = m_Document->GetElementById(elementId))
    {
        if (auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(element))
        {
            control->SetValue(Rml::String{value});
        }
        element->SetAttribute("value", Rml::String{value});
    }
}

void ProjectManagerController::SetStatus(const std::string_view text)
{
    if (m_Document == nullptr)
    {
        return;
    }
    if (Rml::Element* status = m_Document->GetElementById(layout::Status))
    {
        status->SetInnerRML(EscapeRml(text));
    }
}

void ProjectManagerController::SetSelectedRecent(const std::size_t index)
{
    if (m_Projects == nullptr || index >= m_Projects->Recents().size())
    {
        m_SelectedRecent.reset();
    }
    else
    {
        m_SelectedRecent = index;
        WriteInputValue(RenameNameId, m_Projects->Recents()[index].Project.Name);
        WriteInputValue(OpenPathId, m_Projects->Recents()[index].Project.ProjectFile.string());
    }
    UpdateSelectionChrome();
    UpdateActionAvailability();
}

void ProjectManagerController::UpdateSelectionChrome()
{
    if (m_Document == nullptr || m_Projects == nullptr)
    {
        return;
    }
    Rml::Element* list = m_Document->GetElementById(layout::RecentList);
    if (list == nullptr)
    {
        return;
    }
    for (int i = 0; i < list->GetNumChildren(); ++i)
    {
        Rml::Element* child = list->GetChild(i);
        if (child == nullptr)
        {
            continue;
        }
        const Rml::String id = child->GetId();
        const bool selected =
            m_SelectedRecent.has_value() && id == ("recent-" + std::to_string(*m_SelectedRecent));
        if (selected)
        {
            child->SetClass("selected", true);
        }
        else
        {
            child->SetClass("selected", false);
        }
    }
}

void ProjectManagerController::UpdateActionAvailability()
{
    if (m_Document == nullptr)
    {
        return;
    }
    const bool hasSelection = m_SelectedRecent.has_value();
    const bool hasService = Service() != nullptr;
    struct Item
    {
        const char* Id;
        bool Enabled;
        const char* DisabledReason;
    };
    const Item items[] = {
        {BtnOpenSelected, hasSelection, "Select a recent project first"},
        {BtnRemove, hasSelection && hasService, hasService ? "Select a recent project first"
                                                           : "Project service extras unavailable"},
        {BtnRename, hasSelection && hasService, hasService ? "Select a recent project first"
                                                          : "Project service extras unavailable"},
        {BtnReveal, hasSelection && hasService, hasService ? "Select a recent project first"
                                                          : "Project service extras unavailable"},
        {BtnBrowseParent, hasService, "Folder browsing requires ProjectService"},
        {BtnBrowseOpen, hasService, "File browsing requires ProjectService"},
    };
    for (const Item& item : items)
    {
        if (Rml::Element* element = m_Document->GetElementById(item.Id))
        {
            if (item.Enabled)
            {
                element->RemoveAttribute("disabled");
                element->SetAttribute("title", "");
                element->SetProperty("opacity", "1");
            }
            else
            {
                element->SetAttribute("disabled", "true");
                element->SetAttribute("title", item.DisabledReason);
                element->SetProperty("opacity", "0.45");
            }
        }
    }
}

void ProjectManagerController::RefreshRecents()
{
    if (m_Document == nullptr || m_Projects == nullptr)
    {
        return;
    }
    Rml::Element* list = m_Document->GetElementById(layout::RecentList);
    if (list == nullptr)
    {
        return;
    }
    list->SetInnerRML("");

    const auto recents = m_Projects->Recents();
    if (recents.empty())
    {
        list->SetInnerRML(
            "<div class=\"empty-state\" id=\"recent-empty\">"
            "<div class=\"empty-glyph\">+</div>"
            "<div class=\"empty-title\">No recent projects</div>"
            "<div class=\"empty-copy\">Create a project or open an existing project.fadix file.</div>"
            "</div>");
        m_SelectedRecent.reset();
        UpdateActionAvailability();
        return;
    }

    std::ostringstream html;
    for (std::size_t index = 0; index < recents.size(); ++index)
    {
        const RecentProject& entry = recents[index];
        const char* templ =
            entry.Project.Template == ProjectTemplate::Empty2D ? "Empty 2D" : "Empty 3D";
        html << "<button type=\"button\" class=\"recent-item\" id=\"recent-" << index << "\">"
             << EscapeRml(entry.Project.Name) << "  /  " << EscapeRml(templ) << "  /  "
             << EscapeRml(entry.Project.RootPath.string()) << "</button>";
    }
    list->SetInnerRML(html.str());

    // Click/dblclick are bound once on #recent-list (BindButtons); rows resolve via
    // ResolveCommandId walking from the event target.

    if (m_SelectedRecent && *m_SelectedRecent >= recents.size())
    {
        m_SelectedRecent.reset();
    }
    UpdateSelectionChrome();
    UpdateActionAvailability();
}

void ProjectManagerController::OnSelectTemplate(const ProjectTemplate projectTemplate)
{
    m_CreateTemplate = projectTemplate;
    if (m_Document == nullptr)
    {
        return;
    }
    if (Rml::Element* two = m_Document->GetElementById(Template2dId))
    {
        two->SetClass("active", projectTemplate == ProjectTemplate::Empty2D);
    }
    if (Rml::Element* three = m_Document->GetElementById(Template3dId))
    {
        three->SetClass("active", projectTemplate == ProjectTemplate::Empty3D);
    }
}

void ProjectManagerController::OnCreate()
{
    if (m_Projects == nullptr || m_Host == nullptr)
    {
        SetStatus("Project service is not connected");
        return;
    }
    const std::string name = ReadInputValue(CreateNameId);
    const std::string parent = ReadInputValue(CreatePathId);
    auto result = m_Projects->Create(name, parent, m_CreateTemplate);
    if (!result)
    {
        SetStatus(result.ErrorMessage());
        return;
    }
    RefreshRecents();
    SetStatus("Created " + result.Value().Name);
    m_Host->EnterWorkbench(result.Value());
}

void ProjectManagerController::OnBrowseParent()
{
    ProjectService* service = Service();
    if (service == nullptr)
    {
        SetStatus("Folder browsing unavailable");
        return;
    }
    auto result = service->BrowseForFolder(ReadInputValue(CreatePathId));
    if (!result)
    {
        SetStatus(result.ErrorMessage());
        return;
    }
    WriteInputValue(CreatePathId, result.Value().string());
    SetStatus("Parent folder selected");
}

void ProjectManagerController::OnOpen()
{
    if (m_Projects == nullptr || m_Host == nullptr)
    {
        SetStatus("Project service is not connected");
        return;
    }
    std::string path = ReadInputValue(OpenPathId);
    if (path.empty())
    {
        OnBrowseOpen();
        path = ReadInputValue(OpenPathId);
        if (path.empty())
        {
            return;
        }
    }
    auto result = m_Projects->Open(path);
    if (!result)
    {
        SetStatus(result.ErrorMessage());
        return;
    }
    RefreshRecents();
    SetStatus("Opened " + result.Value().Name);
    m_Host->EnterWorkbench(result.Value());
}

void ProjectManagerController::OnBrowseOpen()
{
    ProjectService* service = Service();
    if (service == nullptr)
    {
        SetStatus("File browsing unavailable");
        return;
    }
    auto result = service->BrowseForProjectFile(ProjectService::DefaultProjectsDirectory());
    if (!result)
    {
        SetStatus(result.ErrorMessage());
        return;
    }
    WriteInputValue(OpenPathId, result.Value().string());
    SetStatus("Project file selected");
}

void ProjectManagerController::OnOpenSelected()
{
    if (!m_SelectedRecent || m_Projects == nullptr)
    {
        SetStatus("Select a recent project first");
        return;
    }
    WriteInputValue(OpenPathId, m_Projects->Recents()[*m_SelectedRecent].Project.ProjectFile.string());
    OnOpen();
}

void ProjectManagerController::OnRemoveSelected()
{
    ProjectService* service = Service();
    if (!m_SelectedRecent || service == nullptr || m_Projects == nullptr)
    {
        SetStatus("Select a recent project first");
        return;
    }
    const auto path = m_Projects->Recents()[*m_SelectedRecent].Project.ProjectFile;
    auto result = service->RemoveRecent(path);
    if (!result)
    {
        SetStatus(result.ErrorMessage());
        return;
    }
    m_SelectedRecent.reset();
    RefreshRecents();
    SetStatus("Removed from recent list");
}

void ProjectManagerController::OnRenameSelected()
{
    ProjectService* service = Service();
    if (!m_SelectedRecent || service == nullptr || m_Projects == nullptr)
    {
        SetStatus("Select a recent project first");
        return;
    }
    const auto path = m_Projects->Recents()[*m_SelectedRecent].Project.ProjectFile;
    const std::string newName = ReadInputValue(RenameNameId);
    auto result = service->Rename(path, newName);
    if (!result)
    {
        SetStatus(result.ErrorMessage());
        return;
    }
    RefreshRecents();
    SetStatus("Renamed to " + result.Value().Name);
}

void ProjectManagerController::OnRevealSelected()
{
    ProjectService* service = Service();
    if (!m_SelectedRecent || service == nullptr || m_Projects == nullptr)
    {
        SetStatus("Select a recent project first");
        return;
    }
    const auto& project = m_Projects->Recents()[*m_SelectedRecent].Project;
    auto result = service->RevealInFileManager(project.ProjectFile);
    if (!result)
    {
        SetStatus(result.ErrorMessage());
        return;
    }
    SetStatus("Revealed in file manager");
}
}
