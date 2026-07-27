#include "editor/scene/SceneController.hpp"

#include "assets/AssetDatabase.hpp"
#include "assets/GltfMeshCache.hpp"
#include "editor/assets/AssetBrowserController.hpp"
#include "editor/command/EntityCommands.hpp"
#include "engine/assets/MaterialAsset.hpp"
#include "engine/command/UndoStack.hpp"
#include "engine/scene/IWorld.hpp"
#include "runtime/Components.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace fadix
{
namespace
{
std::string Escape(std::string text)
{
    for (const auto& [from, to] : {
             std::pair{"&", "&amp;"}, std::pair{"<", "&lt;"}, std::pair{">", "&gt;"},
             std::pair{"\"", "&quot;"}})
    {
        std::size_t position = 0;
        while ((position = text.find(from, position)) != std::string::npos)
        {
            text.replace(position, std::char_traits<char>::length(from), to);
            position += std::char_traits<char>::length(to);
        }
    }
    return text;
}

float InputFloat(Rml::Element* element, const float fallback)
{
    const auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(element);
    if (input == nullptr)
    {
        return fallback;
    }
    try
    {
        return std::stof(input->GetValue());
    }
    catch (...)
    {
        return fallback;
    }
}

Rml::Element* NumericInputAt(Rml::Element* element, const float mouseX, const float mouseY)
{
    if (element == nullptr)
    {
        return nullptr;
    }
    if (element->IsClassSet("numeric-input"))
    {
        const Rml::Vector2f position = element->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
        if (mouseX >= position.x && mouseY >= position.y &&
            mouseX < position.x + size.x && mouseY < position.y + size.y)
        {
            return element;
        }
    }
    for (Rml::Element* child = element->GetFirstChild(); child != nullptr;
         child = child->GetNextSibling())
    {
        if (Rml::Element* match = NumericInputAt(child, mouseX, mouseY))
        {
            return match;
        }
    }
    return nullptr;
}

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

void GroupTitle(std::ostringstream& markup, const std::string_view title)
{
    markup << "<div class=\"group-title\"><span class=\"group-label\">"
           << title << "</span></div>";
}

void ScalarProperty(
    std::ostringstream& markup,
    const std::string_view label,
    const std::string_view id,
    const float value,
    const std::string_view attributes = {})
{
    markup << "<div class=\"property property-line\"><span class=\"property-label\">"
           << label << "</span><input class=\"numeric-input\" id=\"" << id << "\" type=\"number\" value=\""
           << value << "\" " << attributes << "/></div>";
}

void VectorProperty(
    std::ostringstream& markup,
    const std::string_view label,
    const std::string_view prefix,
    const float* values,
    const std::string_view axes)
{
    markup << "<div class=\"property\"><span class=\"property-label\">" << label
           << "</span><div class=\"vector vector-" << axes.size() << "\">";
    for (std::size_t index = 0; index < axes.size(); ++index)
    {
        const char axis = axes[index];
        markup << "<div class=\"axis-field\"><span class=\"axis-label axis-" << axis
               << "\">" << static_cast<char>(std::toupper(static_cast<unsigned char>(axis)))
               << "</span><input class=\"numeric-input\" id=\"" << prefix << '-' << axis
               << "\" type=\"number\" value=\"" << values[index] << "\"/></div>";
    }
    markup << "</div></div>";
}

void ToggleProperty(
    std::ostringstream& markup,
    const std::string_view label,
    const std::string_view id,
    const std::string_view value)
{
    markup << "<div class=\"property property-line\"><span class=\"property-label\">"
           << label << "</span><button class=\"inspector-toggle\" id=\"" << id
           << "\"><span class=\"button-label\">" << value << "</span></button></div>";
}

void TextureHandleProperty(
    std::ostringstream& markup,
    const std::string_view label,
    const std::string_view id,
    const AssetHandle& handle)
{
    markup << "<div class=\"property property-line drop-target\" id=\"" << id
           << "-drop\"><span class=\"property-label\">" << label
           << "</span><input class=\"string-input drop-slot\" id=\"" << id
           << "\" type=\"text\" value=\""
           << (handle.IsValid() ? Escape(handle.ToString()) : "") << "\"/></div>";
}

const char* AlphaModeName(const AlphaMode mode) noexcept
{
    switch (mode)
    {
    case AlphaMode::Mask: return "Mask";
    case AlphaMode::Blend: return "Blend";
    case AlphaMode::Opaque: break;
    }
    return "Opaque";
}

const char* EmitterShapeName(const EmitterShape shape) noexcept
{
    switch (shape)
    {
    case EmitterShape::Sphere: return "Sphere";
    case EmitterShape::Box: return "Box";
    case EmitterShape::Cone: break;
    }
    return "Cone";
}

const char* AntiAliasModeName(const AntiAliasMode mode) noexcept
{
    switch (mode)
    {
    case AntiAliasMode::Off: return "Off";
    case AntiAliasMode::Fxaa: return "FXAA";
    case AntiAliasMode::Taa: return "TAA";
    case AntiAliasMode::Auto: break;
    }
    return "Auto";
}
} // namespace

class SceneController::Listener final : public Rml::EventListener
{
public:
    explicit Listener(SceneController& controller) : m_Controller(controller) {}
    void ProcessEvent(Rml::Event& event) override { m_Controller.Handle(event); }

private:
    SceneController& m_Controller;
};

SceneController::SceneController(IWorld& world, UndoStack& history)
    : m_Editor(world, history)
{
}

SceneController::~SceneController() = default;

void SceneController::Bind(Rml::ElementDocument& document)
{
    m_Document = &document;
    m_Listeners.clear();
    for (const char* id : {"scene-delete", "scene-duplicate", "replicated-toggle"})
    {
        Listen(id);
    }
    Refresh();
}

void SceneController::Listen(const char* id, const bool changeEvent)
{
    if (m_Document == nullptr)
    {
        return;
    }
    if (Rml::Element* element = m_Document->GetElementById(id))
    {
        auto listener = std::make_unique<Listener>(*this);
        element->AddEventListener(changeEvent ? Rml::EventId::Change : Rml::EventId::Click, listener.get());
        m_Listeners.push_back(std::move(listener));
    }
}

void SceneController::Refresh()
{
    if (m_Document)
    {
        RebuildTree();
        RebuildInspector();
    }
}

std::optional<Uuid> SceneController::Selection() const noexcept { return m_Editor.Selection(); }

void SceneController::SetSelection(std::optional<Uuid> selection)
{
    m_Editor.SetSelection(std::move(selection));
    Refresh();
}

std::optional<Uuid> SceneController::SceneRootId() const { return m_Editor.SceneRootId(); }
bool SceneController::IsSceneRoot(const Uuid& id) const { return m_Editor.IsSceneRoot(id); }

void SceneController::SetFilter(std::string filter)
{
    m_Editor.SetFilter(std::move(filter));
    RebuildTree();
}

void SceneController::SetChangedCallback(std::function<void()> callback)
{
    m_Editor.SetChangedCallback(std::move(callback));
}

void SceneController::SetAssetDatabase(AssetDatabase* database)
{
    m_Editor.SetAssetDatabase(database);
}

void SceneController::SetGltfMeshCache(GltfMeshCache* cache)
{
    m_Editor.SetGltfMeshCache(cache);
}

void SceneController::SetSelectedMaterialProvider(
    std::function<std::optional<AssetHandle>()> provider)
{
    m_Editor.SetSelectedMaterialProvider(std::move(provider));
}

void SceneController::SetSelectedMeshProvider(
    std::function<std::optional<AssetHandle>()> provider)
{
    m_Editor.SetSelectedMeshProvider(std::move(provider));
}

void SceneController::SetStatusReporter(std::function<void(std::string_view)> reporter)
{
    m_Editor.SetStatusReporter(std::move(reporter));
}

void SceneController::RebuildTree()
{
    Rml::Element* tree = m_Document->GetElementById("scene-tree");
    if (tree == nullptr)
    {
        return;
    }

    m_Editor.EnsureOrphansAdopted();
    const std::optional<Uuid> rootId = m_Editor.SceneRootId();
    const std::string& filter = m_Editor.Filter();
    IWorld& world = m_Editor.World();

    struct Row
    {
        Uuid Id;
        std::string Name;
        bool IsRoot{false};
        const char* Icon{"entity"};
    };
    std::vector<Row> rows;
    for (const auto [entity, id] : world.Registry().view<const UuidComponent>().each())
    {
        const NameComponent* name = world.Registry().try_get<NameComponent>(entity);
        const std::string displayName = name ? name->Name : "Entity";
        if (!filter.empty() && Lower(displayName).find(filter) == std::string::npos)
        {
            continue;
        }
        const bool isRoot = rootId && id.Id == *rootId;
        const bool camera = world.Registry().all_of<CameraComponent>(entity);
        const bool light = world.Registry().all_of<DirectionalLightComponent>(entity);
        const bool pointLight = world.Registry().all_of<PointLightComponent>(entity);
        const bool spotLight = world.Registry().all_of<SpotLightComponent>(entity);
        const bool environment = world.Registry().all_of<EnvironmentComponent>(entity);
        const bool mesh = world.Registry().all_of<MeshComponent>(entity);
        const bool body3d = world.Registry().all_of<JoltBodyComponent>(entity);
        const bool body2d = world.Registry().all_of<Box2DBodyComponent>(entity);
        const bool network = world.Registry().all_of<NetworkIdentityComponent>(entity);
        const char* iconClass = camera ? "camera" : light ? "light"
            : pointLight ? "point-light" : spotLight ? "spot-light"
            : environment ? "environment" : mesh ? "mesh"
            : body3d ? "physics3d" : body2d ? "physics2d" : network ? "network"
            : isRoot ? "scene" : "entity";
        rows.push_back(Row{id.Id, displayName, isRoot, iconClass});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.IsRoot != b.IsRoot)
        {
            return a.IsRoot;
        }
        return Lower(a.Name) < Lower(b.Name);
    });

    const std::optional<Uuid> sel = m_Editor.Selection();
    std::ostringstream markup;
    for (const Row& row : rows)
    {
        const bool selected = sel && *sel == row.Id;
        const bool child = !row.IsRoot;
        markup << "<button id=\"entity-" << row.Id.ToString() << "\" class=\"row"
               << (child ? " child" : "") << (selected ? " selected" : "")
               << "\" data-uuid=\"" << row.Id.ToString() << "\">"
               << "<span class=\"expander\">" << (child ? "-" : "") << "</span>"
               << "<span class=\"icon " << row.Icon << "\"></span>"
               << "<span class=\"label\">" << Escape(row.Name) << "</span></button>";
    }
    tree->SetInnerRML(markup.str());
    for (const Row& row : rows)
    {
        Listen(("entity-" + row.Id.ToString()).c_str());
    }
}

void SceneController::RebuildInspector()
{
    CloseComponentPicker();
    Rml::Element* inspector = m_Document->GetElementById("inspector-content");
    if (inspector == nullptr)
    {
        return;
    }
    const std::optional<Uuid> sel = m_Editor.Selection();
    if (!sel)
    {
        inspector->SetInnerRML("<div class=\"inspector-empty\">Select an entity</div>");
        return;
    }
    IWorld& world = m_Editor.World();
    const auto entity = world.Find(*sel);
    if (!entity)
    {
        m_Editor.SetSelection(std::nullopt);
        inspector->SetInnerRML("<div class=\"inspector-empty\">Select an entity</div>");
        return;
    }
    const NameComponent* name = world.Registry().try_get<NameComponent>(*entity);
    const TransformComponent* transform = world.Registry().try_get<TransformComponent>(*entity);
    std::ostringstream markup;
    markup << std::setprecision(6)
           << "<div class=\"inspector-head\"><input id=\"entity-name\" type=\"text\" value=\""
           << Escape(name ? name->Name : "Entity") << "\"/>"
           << "<div class=\"inspector-id\"><span class=\"inspector-id-label\">Entity ID</span>"
           << "<span class=\"inspector-id-value\">" << sel->ToString()
           << "</span></div></div>";
    if (transform)
    {
        GroupTitle(markup, "Transform");
        const char* labels[] = {"Position", "Rotation", "Scale"};
        const char* prefixes[] = {"position", "rotation", "scale"};
        const glm::vec3 values[] = {transform->Position, glm::degrees(glm::eulerAngles(transform->Rotation)),
                                    transform->Scale};
        for (int row = 0; row < 3; ++row)
        {
            VectorProperty(markup, labels[row], prefixes[row], &values[row][0], "xyz");
        }
    }
    AssetDatabase* assets = m_Editor.Assets();
    GltfMeshCache* gltfMeshes = m_Editor.GltfMeshes();
    if (const MeshComponent* mesh = world.Registry().try_get<MeshComponent>(*entity))
    {
        GroupTitle(markup, "Mesh");
        ToggleProperty(markup, "Primitive", "mesh-kind", MeshKindName(mesh->Kind));

        GroupTitle(markup, "Imported Mesh");
        markup << "<div class=\"property property-line drop-target\" id=\"mesh-imported-drop\">"
               << "<span class=\"property-label\">Asset</span>"
               << "<input class=\"string-input drop-slot\" id=\"mesh-imported\" type=\"text\" value=\""
               << (mesh->ImportedMesh.IsValid() ? Escape(mesh->ImportedMesh.ToString()) : "") << "\"/>"
               << "</div>";
        markup << "<div class=\"property property-line\"><span class=\"property-label\">Actions</span>"
               << "<button class=\"inspector-toggle\" id=\"mesh-imported-assign\">"
               << "<span class=\"button-label\">Assign</span></button>"
               << "<button class=\"inspector-toggle\" id=\"mesh-imported-clear\">"
               << "<span class=\"button-label\">Clear</span></button></div>";

        const bool hasMaterial = mesh->Material.IsValid();
        std::optional<MaterialAsset> materialAsset;
        if (hasMaterial && assets != nullptr)
        {
            if (Result<MaterialAsset> loaded = assets->LoadMaterial(mesh->Material))
            {
                materialAsset = std::move(loaded).Value();
            }
        }

        GroupTitle(markup, "Material");
        markup << "<div class=\"property property-line drop-target\" id=\"mesh-material-drop\">"
               << "<span class=\"property-label\">Asset</span>"
               << "<input class=\"string-input drop-slot\" id=\"mesh-material\" type=\"text\" value=\""
               << (hasMaterial ? Escape(mesh->Material.ToString()) : "") << "\"/>"
               << "</div>";
        if (materialAsset)
        {
            markup << "<div class=\"property property-line\"><span class=\"property-label\">Name</span>"
                   << "<input class=\"string-input\" id=\"mat-name\" type=\"text\" value=\""
                   << Escape(materialAsset->Name) << "\"/></div>";
        }
        else if (hasMaterial)
        {
            markup << "<div class=\"property property-line\"><span class=\"property-label\">Name</span>"
                   << "<span class=\"property-value\">(missing / invalid)</span></div>";
        }
        markup << "<div class=\"property property-line\"><span class=\"property-label\">Actions</span>"
               << "<button class=\"inspector-toggle\" id=\"mesh-material-assign\">"
               << "<span class=\"button-label\">Assign</span></button>"
               << "<button class=\"inspector-toggle\" id=\"mesh-material-clear\">"
               << "<span class=\"button-label\">Clear</span></button></div>";

        if (materialAsset)
        {
            VectorProperty(markup, "Base Color", "mat-basecolor", &materialAsset->BaseColor[0], "rgba");
            TextureHandleProperty(
                markup, "Base Color Texture", "mat-tex-basecolor", materialAsset->BaseColorTexture);
            TextureHandleProperty(markup, "Normal Texture", "mat-tex-normal", materialAsset->NormalTexture);
            ScalarProperty(
                markup, "Metallic", "mat-metallic", materialAsset->Metallic,
                "min=\"0\" max=\"1\" step=\"0.01\"");
            ScalarProperty(
                markup, "Roughness", "mat-roughness", materialAsset->Roughness,
                "min=\"0\" max=\"1\" step=\"0.01\"");
            TextureHandleProperty(
                markup, "Metallic/Roughness Texture", "mat-tex-mr",
                materialAsset->MetallicRoughnessTexture);
            VectorProperty(
                markup, "Emissive Color", "mat-emissive", &materialAsset->EmissiveColor[0], "rgb");
            ScalarProperty(
                markup, "Emissive Intensity", "mat-emissive-intensity",
                materialAsset->EmissiveIntensity, "min=\"0\" step=\"0.1\"");
            TextureHandleProperty(
                markup, "Emissive Texture", "mat-tex-emissive", materialAsset->EmissiveTexture);
            ToggleProperty(
                markup, "Alpha Mode", "mat-alpha-mode", AlphaModeName(materialAsset->AlphaMode));
            ScalarProperty(
                markup, "Alpha Cutoff", "mat-alpha-cutoff", materialAsset->AlphaCutoff,
                "min=\"0\" max=\"1\" step=\"0.01\"");
            ToggleProperty(
                markup, "Double Sided", "mat-double-sided",
                materialAsset->DoubleSided ? "On" : "Off");
            VectorProperty(markup, "UV Scale", "mat-uv-scale", &materialAsset->UVScale[0], "xy");
            VectorProperty(markup, "UV Offset", "mat-uv-offset", &materialAsset->UVOffset[0], "xy");
        }
        else
        {
            VectorProperty(markup, "Base Color", "mesh-color", &mesh->BaseColor[0], "rgb");
            ScalarProperty(
                markup, "Metallic", "mesh-metallic", mesh->Metallic,
                "min=\"0\" max=\"1\" step=\"0.01\"");
            ScalarProperty(
                markup, "Roughness", "mesh-roughness", mesh->Roughness,
                "min=\"0\" max=\"1\" step=\"0.01\"");
        }
        ToggleProperty(markup, "Component", "remove-mesh", "Remove");
    }
    if (const DirectionalLightComponent* light =
            world.Registry().try_get<DirectionalLightComponent>(*entity))
    {
        GroupTitle(markup, "Directional Light");
        VectorProperty(markup, "Color", "light-color", &light->Color[0], "rgb");
        ScalarProperty(markup, "Intensity", "light-intensity", light->Intensity, "min=\"0\" step=\"0.1\"");
        ToggleProperty(markup, "Cast Shadows", "light-cast-shadows", light->CastShadows ? "On" : "Off");
        ScalarProperty(
            markup, "Shadow Bias", "light-shadow-bias", light->ShadowBias,
            "min=\"0\" max=\"0.05\" step=\"0.001\"");
        ScalarProperty(
            markup, "Shadow Strength", "light-shadow-strength", light->ShadowStrength,
            "min=\"0\" max=\"1\" step=\"0.01\"");
        ToggleProperty(
            markup, "Shadow Resolution", "light-shadow-res",
            std::to_string(light->ShadowMapResolution > 0 ? light->ShadowMapResolution : 2048));
        ToggleProperty(markup, "Component", "remove-dirlight", "Remove");
    }
    if (const PointLightComponent* light =
            world.Registry().try_get<PointLightComponent>(*entity))
    {
        GroupTitle(markup, "Point Light");
        ToggleProperty(markup, "Enabled", "pointlight-enabled", light->Enabled ? "On" : "Off");
        VectorProperty(markup, "Color", "pointlight-color", &light->Color[0], "rgb");
        ScalarProperty(markup, "Intensity", "pointlight-intensity", light->Intensity, "min=\"0\" step=\"0.5\"");
        ScalarProperty(markup, "Range", "pointlight-range", light->Range, "min=\"0.01\" step=\"0.5\"");
        ScalarProperty(markup, "Falloff Exponent", "pointlight-falloff", light->FalloffExponent, "min=\"0.1\" max=\"16\" step=\"0.1\"");
        ToggleProperty(markup, "Component", "remove-pointlight", "Remove");
    }
    if (const SpotLightComponent* light =
            world.Registry().try_get<SpotLightComponent>(*entity))
    {
        GroupTitle(markup, "Spot Light");
        ToggleProperty(markup, "Enabled", "spotlight-enabled", light->Enabled ? "On" : "Off");
        VectorProperty(markup, "Color", "spotlight-color", &light->Color[0], "rgb");
        ScalarProperty(markup, "Intensity", "spotlight-intensity", light->Intensity, "min=\"0\" step=\"0.5\"");
        ScalarProperty(markup, "Range", "spotlight-range", light->Range, "min=\"0.01\" step=\"0.5\"");
        ScalarProperty(markup, "Inner Cone", "spotlight-inner", light->InnerConeDegrees, "min=\"0\" max=\"89\" step=\"0.5\"");
        ScalarProperty(markup, "Outer Cone", "spotlight-outer", light->OuterConeDegrees, "min=\"0.2\" max=\"89.9\" step=\"0.5\"");
        ScalarProperty(markup, "Falloff Exponent", "spotlight-falloff", light->FalloffExponent, "min=\"0.1\" max=\"16\" step=\"0.1\"");
        ToggleProperty(markup, "Component", "remove-spotlight", "Remove");
    }
    if (const EnvironmentComponent* env =
            world.Registry().try_get<EnvironmentComponent>(*entity))
    {
        GroupTitle(markup, "Environment");
        ToggleProperty(markup, "Primary", "env-primary", env->Primary ? "Primary" : "Make Primary");
        ScalarProperty(markup, "Priority", "env-priority", static_cast<float>(env->Priority), "step=\"1\"");
        VectorProperty(markup, "Sky Zenith", "env-zenith", &env->SkyZenithColor[0], "rgb");
        VectorProperty(markup, "Sky Horizon", "env-horizon", &env->SkyHorizonColor[0], "rgb");
        VectorProperty(markup, "Ground Color", "env-ground", &env->GroundColor[0], "rgb");
        VectorProperty(markup, "Ambient Color", "env-ambient", &env->AmbientColor[0], "rgb");
        ScalarProperty(markup, "Ambient Intensity", "env-ambient-intensity", env->AmbientIntensity, "min=\"0\" max=\"16\" step=\"0.05\"");
        ScalarProperty(markup, "Exposure", "env-exposure", env->Exposure, "min=\"0.05\" max=\"16\" step=\"0.05\"");
        ToggleProperty(markup, "Fog Enabled", "env-fog-enabled", env->FogEnabled ? "On" : "Off");
        VectorProperty(markup, "Fog Color", "env-fog-color", &env->FogColor[0], "rgb");
        ScalarProperty(markup, "Fog Density", "env-fog-density", env->FogDensity, "min=\"0\" max=\"1\" step=\"0.001\"");
        ScalarProperty(markup, "Fog Start", "env-fog-start", env->FogStart, "min=\"0\" step=\"1\"");
        ScalarProperty(markup, "Fog End", "env-fog-end", env->FogEnd, "min=\"0.1\" step=\"5\"");
        GroupTitle(markup, "Post Processing");
        ToggleProperty(markup, "Bloom", "env-bloom", env->BloomEnabled ? "On" : "Off");
        ScalarProperty(markup, "Bloom Threshold", "env-bloom-threshold", env->BloomThreshold, "min=\"0\" max=\"10\" step=\"0.05\"");
        ScalarProperty(markup, "Bloom Intensity", "env-bloom-intensity", env->BloomIntensity, "min=\"0\" max=\"4\" step=\"0.05\"");
        ScalarProperty(markup, "Bloom Passes", "env-bloom-passes", static_cast<float>(env->BloomPasses), "min=\"1\" max=\"8\" step=\"1\"");
        ToggleProperty(markup, "Tonemapping", "env-tonemap", env->TonemapEnabled ? "On" : "Off");
        ToggleProperty(markup, "Color Grading", "env-colorgrading", env->ColorGradingEnabled ? "On" : "Off");
        ScalarProperty(markup, "Lift R", "env-lift-r", env->Lift.x, "min=\"-1\" max=\"1\" step=\"0.01\"");
        ScalarProperty(markup, "Lift G", "env-lift-g", env->Lift.y, "min=\"-1\" max=\"1\" step=\"0.01\"");
        ScalarProperty(markup, "Lift B", "env-lift-b", env->Lift.z, "min=\"-1\" max=\"1\" step=\"0.01\"");
        ScalarProperty(markup, "Gamma R", "env-gamma-r", env->Gamma.x, "min=\"0.1\" max=\"3\" step=\"0.05\"");
        ScalarProperty(markup, "Gamma G", "env-gamma-g", env->Gamma.y, "min=\"0.1\" max=\"3\" step=\"0.05\"");
        ScalarProperty(markup, "Gamma B", "env-gamma-b", env->Gamma.z, "min=\"0.1\" max=\"3\" step=\"0.05\"");
        ScalarProperty(markup, "Gain R", "env-gain-r", env->Gain.x, "min=\"0\" max=\"3\" step=\"0.05\"");
        ScalarProperty(markup, "Gain G", "env-gain-g", env->Gain.y, "min=\"0\" max=\"3\" step=\"0.05\"");
        ScalarProperty(markup, "Gain B", "env-gain-b", env->Gain.z, "min=\"0\" max=\"3\" step=\"0.05\"");
        ToggleProperty(
            markup, "Anti-Alias", "env-antialias", AntiAliasModeName(env->AntiAlias));
        ScalarProperty(markup, "FXAA Strength", "env-fxaa-strength", env->FxaaStrength, "min=\"0\" max=\"2\" step=\"0.05\"");
        ToggleProperty(markup, "Component", "remove-environment", "Remove");
    }
    if (const VisibilityComponent* visibility =
            world.Registry().try_get<VisibilityComponent>(*entity))
    {
        GroupTitle(markup, "Visibility");
        ToggleProperty(markup, "Visible In Editor", "vis-editor", visibility->VisibleInEditor ? "On" : "Off");
        ToggleProperty(markup, "Visible In Game", "vis-game", visibility->VisibleInGame ? "On" : "Off");
        ToggleProperty(markup, "Selectable", "vis-selectable", visibility->Selectable ? "On" : "Off");
        ToggleProperty(markup, "Component", "remove-visibility", "Remove");
    }
    if (const CameraComponent* camera = world.Registry().try_get<CameraComponent>(*entity))
    {
        GroupTitle(markup, "Camera");
        ScalarProperty(markup, "Field of View", "camera-fov", camera->FieldOfView, "min=\"1\" max=\"179\" step=\"1\"");
        ScalarProperty(markup, "Near Clip", "camera-near", camera->NearPlane, "min=\"0.001\" step=\"0.01\"");
        ScalarProperty(markup, "Far Clip", "camera-far", camera->FarPlane, "min=\"0.01\" step=\"10\"");
        ToggleProperty(markup, "Primary Camera", "camera-primary", camera->Primary ? "On" : "Off");
        ToggleProperty(markup, "Component", "remove-camera", "Remove");
    }
    if (const NetworkIdentityComponent* network =
            world.Registry().try_get<NetworkIdentityComponent>(*entity))
    {
        GroupTitle(markup, "Network Identity (Local)");
        markup << "<div class=\"property property-line\"><span class=\"property-label\">Network ID</span>"
               << "<input class=\"numeric-input\" id=\"network-id\" type=\"number\" min=\"0\" value=\""
               << network->NetworkId << "\"/></div>";
        ToggleProperty(markup, "Authority", "network-authority",
            network->Authority == NetworkAuthority::Server ? "Server"
                : network->Authority == NetworkAuthority::Owner ? "Owner" : "Shared");
        ToggleProperty(markup, "Component", "replicated-toggle", "Remove");
    }
    if (const JoltBodyComponent* body = world.Registry().try_get<JoltBodyComponent>(*entity))
    {
        GroupTitle(markup, "Jolt Body 3D");
        VectorProperty(markup, "Half Extents", "jolt-extent", &body->HalfExtent[0], "xyz");
        ToggleProperty(markup, "Body Type", "jolt-dynamic", body->Dynamic ? "Dynamic" : "Static");
        ScalarProperty(markup, "Mass (kg)", "jolt-mass", body->Mass,
            "min=\"0.001\" max=\"1000000\" step=\"0.1\"");
        ScalarProperty(markup, "Friction", "jolt-friction", body->Friction,
            "min=\"0\" max=\"2\" step=\"0.01\"");
        ToggleProperty(markup, "Component", "remove-jolt", "Remove");
    }
    if (const Box2DBodyComponent* body = world.Registry().try_get<Box2DBodyComponent>(*entity))
    {
        GroupTitle(markup, "Box2D Body");
        VectorProperty(markup, "Half Extents", "box2d-extent", &body->HalfExtent[0], "xy");
        ToggleProperty(markup, "Body Type", "box2d-dynamic", body->Dynamic ? "Dynamic" : "Static");
        ToggleProperty(markup, "Component", "remove-box2d", "Remove");
    }
    if (const ScriptComponent* scripts = world.Registry().try_get<ScriptComponent>(*entity))
    {
        GroupTitle(markup, "Scripts");
        ToggleProperty(markup, "Enabled", "script-enabled", scripts->Enabled ? "On" : "Off");
        if (scripts->ScriptNames.empty())
        {
            markup << "<div class=\"property property-line\"><span class=\"property-label\">"
                   << "Attached</span><span class=\"property-value\">Drop a script here</span></div>";
        }
        else
        {
            for (std::size_t i = 0; i < scripts->ScriptNames.size(); ++i)
            {
                markup << "<div class=\"property property-line\"><span class=\"property-label\">Script "
                       << (i + 1) << "</span><span class=\"property-value\">"
                       << Escape(scripts->ScriptNames[i]) << "</span></div>";
            }
        }
        std::string targetLabel = "Drop an entity here";
        if (scripts->Target.IsValid())
        {
            if (const auto targetEntity = world.Find(scripts->Target))
            {
                if (const NameComponent* targetName =
                        world.Registry().try_get<NameComponent>(*targetEntity))
                {
                    targetLabel = targetName->Name;
                }
                else
                {
                    targetLabel = scripts->Target.ToString();
                }
            }
            else
            {
                targetLabel = "(missing)";
            }
        }
        markup << "<div class=\"property property-line drop-target\" id=\"script-target-drop\">"
               << "<span class=\"property-label\">Target</span>"
               << "<input class=\"string-input drop-slot\" id=\"script-target\" type=\"text\" value=\""
               << Escape(targetLabel) << "\"/>"
               << "<button class=\"inspector-toggle\" id=\"script-target-clear\">"
               << "<span class=\"button-label\">Clear</span></button></div>";
        ToggleProperty(markup, "Component", "remove-script", "Remove");
    }
    if (const ParticleEmitterComponent* emitter =
            world.Registry().try_get<ParticleEmitterComponent>(*entity))
    {
        GroupTitle(markup, "Particle Emitter");
        ToggleProperty(markup, "Enabled", "particle-enabled", emitter->Enabled ? "On" : "Off");
        ScalarProperty(
            markup, "Max Particles", "particle-max", static_cast<float>(emitter->MaxParticles),
            "min=\"1\" max=\"4096\" step=\"1\"");
        ScalarProperty(
            markup, "Emit Rate", "particle-emit-rate", emitter->EmitRate, "min=\"0\" step=\"1\"");
        ScalarProperty(
            markup, "Lifetime Min", "particle-life-min", emitter->LifetimeMin, "min=\"0.01\" step=\"0.1\"");
        ScalarProperty(
            markup, "Lifetime Max", "particle-life-max", emitter->LifetimeMax, "min=\"0.01\" step=\"0.1\"");
        ScalarProperty(
            markup, "Speed Min", "particle-speed-min", emitter->SpeedMin, "min=\"0\" step=\"0.1\"");
        ScalarProperty(
            markup, "Speed Max", "particle-speed-max", emitter->SpeedMax, "min=\"0\" step=\"0.1\"");
        ToggleProperty(markup, "Shape", "particle-shape", EmitterShapeName(emitter->Shape));
        ScalarProperty(
            markup, "Shape Radius", "particle-shape-radius", emitter->ShapeRadius,
            "min=\"0\" step=\"0.05\"");
        ScalarProperty(
            markup, "Cone Angle", "particle-cone-angle", emitter->ConeAngle,
            "min=\"0\" max=\"90\" step=\"1\"");
        VectorProperty(markup, "Direction", "particle-dir", &emitter->Direction[0], "xyz");
        VectorProperty(markup, "Gravity", "particle-gravity", &emitter->Gravity[0], "xyz");
        VectorProperty(markup, "Color Start", "particle-color-start", &emitter->ColorStart[0], "rgba");
        VectorProperty(markup, "Color End", "particle-color-end", &emitter->ColorEnd[0], "rgba");
        ScalarProperty(
            markup, "Size Start", "particle-size-start", emitter->SizeStart, "min=\"0\" step=\"0.01\"");
        ScalarProperty(
            markup, "Size End", "particle-size-end", emitter->SizeEnd, "min=\"0\" step=\"0.01\"");
        ToggleProperty(
            markup, "Play On Start", "particle-play-on-start", emitter->PlayOnStart ? "On" : "Off");
        ToggleProperty(markup, "Loop", "particle-loop", emitter->Loop ? "On" : "Off");
        ToggleProperty(markup, "Component", "remove-particle", "Remove");
    }
    if (const AudioSourceComponent* audio =
            world.Registry().try_get<AudioSourceComponent>(*entity))
    {
        GroupTitle(markup, "Audio Source");
        markup << "<div class=\"property property-line drop-target\" id=\"audio-sound-drop\">"
               << "<span class=\"property-label\">Sound</span>"
               << "<input class=\"string-input drop-slot\" id=\"audio-sound\" type=\"text\" value=\""
               << (audio->Sound.IsValid() ? Escape(audio->Sound.ToString()) : "None") << "\"/>"
               << "</div>";
        ToggleProperty(
            markup, "Play On Start", "audio-play-on-start", audio->PlayOnStart ? "On" : "Off");
        ToggleProperty(markup, "Loop", "audio-loop", audio->Loop ? "On" : "Off");
        ToggleProperty(markup, "Spatial", "audio-spatial", audio->Spatial ? "On" : "Off");
        ScalarProperty(
            markup, "Volume", "audio-volume", audio->Volume, "min=\"0\" max=\"1\" step=\"0.01\"");
        ScalarProperty(
            markup, "Min Distance", "audio-min-distance", audio->MinDistance,
            "min=\"0\" step=\"0.1\"");
        ScalarProperty(
            markup, "Max Distance", "audio-max-distance", audio->MaxDistance,
            "min=\"0.01\" step=\"0.5\"");
        ToggleProperty(markup, "Component", "remove-audio-source", "Remove");
    }
    if (const AudioListenerComponent* listener =
            world.Registry().try_get<AudioListenerComponent>(*entity))
    {
        GroupTitle(markup, "Audio Listener");
        ToggleProperty(markup, "Active", "audio-listener-active", listener->Active ? "On" : "Off");
        ToggleProperty(markup, "Component", "remove-audio-listener", "Remove");
    }
    if (const UICanvasComponent* ui = world.Registry().try_get<UICanvasComponent>(*entity))
    {
        GroupTitle(markup, "UI Canvas");
        markup << "<div class=\"property property-line drop-target\" id=\"ui-asset-drop\">"
               << "<span class=\"property-label\">UI</span>"
               << "<input class=\"string-input drop-slot\" id=\"ui-asset\" type=\"text\" value=\""
               << (ui->UIAsset.IsValid() ? Escape(ui->UIAsset.ToString()) : "None") << "\"/>"
               << "</div>";
        markup << "<div class=\"property property-line drop-target\" id=\"ui-style-drop\">"
               << "<span class=\"property-label\">Style</span>"
               << "<input class=\"string-input drop-slot\" id=\"ui-style\" type=\"text\" value=\""
               << (ui->StyleAsset.IsValid() ? Escape(ui->StyleAsset.ToString()) : "None") << "\"/>"
               << "</div>";
        ToggleProperty(
            markup, "Editor Preview", "ui-render-editor", ui->RenderInEditor ? "On" : "Off");
        ToggleProperty(markup, "Game Render", "ui-render-game", ui->RenderInGame ? "On" : "Off");
        ScalarProperty(
            markup, "Order", "ui-order", static_cast<float>(ui->Order), "min=\"-10\" max=\"10\" step=\"1\"");
        ScalarProperty(markup, "Scale", "ui-scale", ui->Scale, "min=\"0.1\" max=\"5\" step=\"0.1\"");
        ToggleProperty(markup, "Component", "remove-ui-canvas", "Remove");
    }
    if (const TerrainComponent* terrain = world.Registry().try_get<TerrainComponent>(*entity))
    {
        const int layerCount = std::clamp(terrain->LayerCount, 1, 4);
        GroupTitle(markup, "Terrain");
        markup << "<div class=\"property property-line drop-target\" id=\"terrain-heightmap-drop\">"
               << "<span class=\"property-label\">Heightmap</span>"
               << "<input class=\"string-input drop-slot\" id=\"terrain-heightmap\" type=\"text\" value=\""
               << (terrain->Heightmap.IsValid() ? Escape(terrain->Heightmap.ToString()) : "None")
               << "\"/>"
               << "</div>";
        ScalarProperty(
            markup, "Width", "terrain-width", terrain->Width, "min=\"1\" max=\"10000\" step=\"1\"");
        ScalarProperty(
            markup, "Depth", "terrain-depth", terrain->Depth, "min=\"1\" max=\"10000\" step=\"1\"");
        ScalarProperty(
            markup, "Height", "terrain-height", terrain->HeightScale,
            "min=\"0.1\" max=\"1000\" step=\"0.5\"");
        ScalarProperty(
            markup, "Res X", "terrain-res-x", static_cast<float>(terrain->ResolutionX),
            "min=\"2\" max=\"513\" step=\"1\"");
        ScalarProperty(
            markup, "Res Z", "terrain-res-z", static_cast<float>(terrain->ResolutionZ),
            "min=\"2\" max=\"513\" step=\"1\"");
        ScalarProperty(
            markup, "Layers", "terrain-layer-count", static_cast<float>(layerCount),
            "min=\"1\" max=\"4\" step=\"1\"");
        for (int i = 0; i < layerCount; ++i)
        {
            const std::string prefix = "terrain-layer-" + std::to_string(i);
            GroupTitle(markup, "Layer " + std::to_string(i + 1));
            markup << "<div class=\"property property-line drop-target\" id=\"" << prefix
                   << "-tex-drop\">"
                   << "<span class=\"property-label\">Texture</span>"
                   << "<input class=\"string-input drop-slot\" id=\"" << prefix
                   << "-tex\" type=\"text\" value=\""
                   << (terrain->Layers[i].Texture.IsValid()
                           ? Escape(terrain->Layers[i].Texture.ToString())
                           : "None")
                   << "\"/>"
                   << "</div>";
            ScalarProperty(
                markup, "Tiling", (prefix + "-tiling").c_str(), terrain->Layers[i].Tiling,
                "min=\"0.1\" max=\"100\" step=\"0.5\"");
            ScalarProperty(
                markup, "Min Height", (prefix + "-min-h").c_str(), terrain->Layers[i].MinHeight,
                "min=\"0\" max=\"1\" step=\"0.01\"");
            ScalarProperty(
                markup, "Max Height", (prefix + "-max-h").c_str(), terrain->Layers[i].MaxHeight,
                "min=\"0\" max=\"1\" step=\"0.01\"");
            ScalarProperty(
                markup, "Min Slope", (prefix + "-min-s").c_str(), terrain->Layers[i].MinSlope,
                "min=\"0\" max=\"1\" step=\"0.01\"");
            ScalarProperty(
                markup, "Max Slope", (prefix + "-max-s").c_str(), terrain->Layers[i].MaxSlope,
                "min=\"0\" max=\"1\" step=\"0.01\"");
        }
        ToggleProperty(
            markup, "Cast Shadows", "terrain-shadows", terrain->CastShadows ? "On" : "Off");
        ToggleProperty(markup, "Component", "remove-terrain", "Remove");
    }
    if (const SkeletonComponent* skeleton =
            world.Registry().try_get<SkeletonComponent>(*entity))
    {
        GroupTitle(markup, "Skeleton");
        markup << "<div class=\"property property-line\"><span class=\"property-label\">Joints</span>"
               << "<span class=\"property-value\">" << skeleton->JointCount << "</span></div>";
        ToggleProperty(markup, "Component", "remove-skeleton", "Remove");
    }
    if (const AnimatorComponent* animator =
            world.Registry().try_get<AnimatorComponent>(*entity))
    {
        GroupTitle(markup, "Animator");
        std::string clipLabel = animator->ClipName.empty() ? "(none)" : animator->ClipName;
        if (gltfMeshes != nullptr)
        {
            if (const MeshComponent* mesh = world.Registry().try_get<MeshComponent>(*entity);
                mesh != nullptr && mesh->ImportedMesh.IsValid())
            {
                if (const GltfMeshAsset* asset = gltfMeshes->Get(mesh->ImportedMesh))
                {
                    if (asset->Animations.empty())
                    {
                        clipLabel = "No animations";
                    }
                    else
                    {
                        clipLabel = animator->ClipName.empty()
                            ? asset->Animations.front().Name
                            : animator->ClipName;
                        clipLabel +=
                            " (" + std::to_string(asset->Animations.size()) + " clips)";
                    }
                }
            }
        }
        markup << "<div class=\"property property-line\"><span class=\"property-label\">Clip</span>"
               << "<span class=\"property-value\">" << Escape(clipLabel) << "</span></div>";
        ToggleProperty(markup, "Next Clip", "animator-next-clip", "Cycle");
        ScalarProperty(
            markup, "Speed", "animator-speed", animator->Speed, "min=\"0\" max=\"4\" step=\"0.1\"");
        ToggleProperty(markup, "Loop", "animator-loop", animator->Loop ? "On" : "Off");
        ToggleProperty(markup, "Playing", "animator-playing", animator->Playing ? "On" : "Off");
        markup << "<div class=\"property property-line\"><span class=\"property-label\">Time</span>"
               << "<span class=\"property-value\">" << animator->CurrentTime << "</span></div>";
        ToggleProperty(markup, "Component", "remove-animator", "Remove");
    }
    markup << "<div class=\"add-component\"><button id=\"add-component\">"
           << "<span class=\"add-symbol\">+</span>"
           << "<span class=\"button-label\">Add Component</span></button>";
    markup << "</div>";
    inspector->SetInnerRML(markup.str());
    Listen("entity-name", true);
    for (const char* prefix : {"position", "rotation", "scale"})
    {
        for (const char axis : {'x', 'y', 'z'})
        {
            const std::string id = std::string{prefix} + '-' + axis;
            Listen(id.c_str(), true);
        }
    }
    for (const char* id : {"camera-fov", "camera-near", "camera-far", "mesh-metallic",
             "mesh-roughness", "mesh-material", "mesh-imported", "mat-name", "mat-metallic", "mat-roughness",
             "mat-emissive-intensity", "mat-alpha-cutoff",
             "mat-tex-basecolor", "mat-tex-normal", "mat-tex-mr", "mat-tex-emissive",
             "light-intensity", "light-shadow-bias", "light-shadow-strength",
             "pointlight-intensity", "pointlight-range",
             "pointlight-falloff", "spotlight-intensity", "spotlight-range", "spotlight-inner",
             "spotlight-outer", "spotlight-falloff", "env-priority", "env-ambient-intensity",
             "env-exposure", "env-fog-density", "env-fog-start", "env-fog-end",
             "env-bloom-threshold", "env-bloom-intensity", "env-bloom-passes",
             "env-lift-r", "env-lift-g", "env-lift-b",
             "env-gamma-r", "env-gamma-g", "env-gamma-b",
             "env-gain-r", "env-gain-g", "env-gain-b", "env-fxaa-strength",
             "particle-max", "particle-emit-rate", "particle-life-min", "particle-life-max",
             "particle-speed-min", "particle-speed-max", "particle-shape-radius",
             "particle-cone-angle", "particle-size-start", "particle-size-end",
             "audio-volume", "audio-min-distance", "audio-max-distance", "audio-sound",
             "ui-asset", "ui-style", "ui-order", "ui-scale",
             "terrain-heightmap", "terrain-width", "terrain-depth", "terrain-height",
             "terrain-res-x", "terrain-res-z", "terrain-layer-count", "animator-speed"})
    {
        Listen(id, true);
    }
    if (const TerrainComponent* terrain = world.Registry().try_get<TerrainComponent>(*entity))
    {
        const int layerCount = std::clamp(terrain->LayerCount, 1, 4);
        for (int i = 0; i < layerCount; ++i)
        {
            const std::string prefix = "terrain-layer-" + std::to_string(i);
            for (const char* suffix : {"-tex", "-tiling", "-min-h", "-max-h", "-min-s", "-max-s"})
            {
                Listen((prefix + suffix).c_str(), true);
            }
        }
    }
    Listen("camera-primary");
    for (const char* prefix : {"mesh-color", "mat-basecolor", "mat-emissive", "light-color",
             "pointlight-color", "spotlight-color", "env-zenith", "env-horizon", "env-ground",
             "env-ambient", "env-fog-color", "particle-color-start", "particle-color-end"})
    {
        for (const char axis : {'r', 'g', 'b'})
        {
            Listen((std::string{prefix} + '-' + axis).c_str(), true);
        }
    }
    for (const char* prefix : {"particle-color-start", "particle-color-end"})
    {
        Listen((std::string{prefix} + "-a").c_str(), true);
    }
    Listen("mat-basecolor-a", true);
    for (const char* prefix : {"mat-uv-scale", "mat-uv-offset"})
    {
        for (const char axis : {'x', 'y'})
        {
            Listen((std::string{prefix} + '-' + axis).c_str(), true);
        }
    }
    for (const char* prefix : {"particle-dir", "particle-gravity"})
    {
        for (const char axis : {'x', 'y', 'z'})
        {
            Listen((std::string{prefix} + '-' + axis).c_str(), true);
        }
    }
    Listen("network-id", true);
    Listen("network-authority");
    Listen("jolt-mass", true);
    Listen("jolt-friction", true);
    for (const char axis : {'x', 'y', 'z'})
    {
        Listen((std::string{"jolt-extent-"} + axis).c_str(), true);
    }
    for (const char axis : {'x', 'y'})
    {
        Listen((std::string{"box2d-extent-"} + axis).c_str(), true);
    }
    for (const char* id : {"jolt-dynamic", "box2d-dynamic", "script-enabled",
             "replicated-toggle", "script-target-clear",
             "mesh-kind", "mesh-material-assign", "mesh-material-clear",
             "mesh-imported-assign", "mesh-imported-clear",
             "mat-alpha-mode", "mat-double-sided",
             "pointlight-enabled", "spotlight-enabled", "env-primary",
             "env-fog-enabled", "env-bloom", "env-tonemap", "env-colorgrading", "env-antialias",
             "vis-editor", "vis-game", "vis-selectable", "remove-mesh",
             "remove-dirlight", "remove-pointlight", "remove-spotlight", "remove-environment",
             "remove-visibility", "remove-camera", "remove-jolt", "remove-box2d", "remove-script",
             "remove-particle", "remove-audio-source", "remove-audio-listener", "remove-ui-canvas",
             "remove-terrain",
             "light-cast-shadows", "light-shadow-res",
             "particle-enabled", "particle-shape", "particle-play-on-start", "particle-loop",
             "audio-play-on-start", "audio-loop", "audio-spatial", "audio-listener-active",
             "ui-render-editor", "ui-render-game", "terrain-shadows",
             "animator-next-clip", "animator-loop", "animator-playing",
             "remove-skeleton", "remove-animator"})
    {
        Listen(id);
    }
}

void SceneController::ToggleComponentPicker(Rml::Element& anchor)
{
    if (m_Document == nullptr || !m_Editor.Selection())
    {
        return;
    }
    if (m_AddComponentOpen)
    {
        CloseComponentPicker();
        return;
    }
    Rml::Element* popup = m_Document->GetElementById("component-popup");
    if (popup == nullptr || !m_Editor.World().Find(*m_Editor.Selection()))
    {
        return;
    }

    m_ComponentFilter.clear();
    popup->SetInnerRML(
        "<input id=\"component-search\" class=\"component-search\" type=\"text\" "
        "placeholder=\"Search components...\"/>"
        "<div id=\"component-list\" class=\"component-list\"></div>");
    RebuildComponentPickerList();

    constexpr float pickerWidth = 300.0F;
    constexpr float pickerHeight = 240.0F;
    const Rml::Vector2f anchorPosition = anchor.GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f anchorSize = anchor.GetBox().GetSize(Rml::BoxArea::Border);
    const Rml::Vector2f documentSize = m_Document->GetBox().GetSize(Rml::BoxArea::Content);
    const float left = std::clamp(
        anchorPosition.x + anchorSize.x - pickerWidth, 4.0F,
        std::max(documentSize.x - pickerWidth - 4.0F, 4.0F));
    const float below = anchorPosition.y + anchorSize.y + 3.0F;
    float top = below;
    if (below + pickerHeight > documentSize.y - 4.0F)
    {
        const float above = anchorPosition.y - pickerHeight - 3.0F;
        top = above >= 31.0F ? above : std::max(4.0F, documentSize.y - pickerHeight - 4.0F);
    }
    top = std::clamp(top, 4.0F, std::max(4.0F, documentSize.y - pickerHeight - 4.0F));
    popup->SetProperty("width", "300px");
    popup->SetProperty("height", "240px");
    popup->SetProperty("max-height", "240px");
    popup->SetProperty("left", std::to_string(static_cast<int>(left)) + "px");
    popup->SetProperty("top", std::to_string(static_cast<int>(top)) + "px");
    popup->SetClass("hidden", false);
    m_AddComponentOpen = true;

    Listen("component-search", true);
    if (Rml::Element* search = m_Document->GetElementById("component-search"))
    {
        search->Focus();
    }
}

void SceneController::RebuildComponentPickerList()
{
    if (m_Document == nullptr || !m_Editor.Selection())
    {
        return;
    }
    Rml::Element* list = m_Document->GetElementById("component-list");
    const auto entity = m_Editor.World().Find(*m_Editor.Selection());
    if (list == nullptr || !entity)
    {
        return;
    }

    m_ComponentPickIds.clear();
    std::ostringstream markup;
    std::size_t optionCount = 0;
    std::ostringstream category;
    std::size_t categoryOptions = 0;
    std::vector<std::string> pendingIds;

    const auto matches = [this](const char* label) {
        if (m_ComponentFilter.empty())
        {
            return true;
        }
        return Lower(label).find(m_ComponentFilter) != std::string::npos;
    };
    const auto componentOption = [&](const char* id, const char* icon, const char* label) {
        if (!matches(label))
        {
            return;
        }
        category << "<button class=\"component-option\" id=\"" << id << "\">"
                 << "<span class=\"component-icon " << icon << "\"></span>"
                 << "<span class=\"component-label\">" << label << "</span></button>";
        pendingIds.emplace_back(id);
        ++categoryOptions;
    };
    const auto beginCategory = [&]() {
        category.str({});
        category.clear();
        categoryOptions = 0;
        pendingIds.clear();
    };
    const auto endCategory = [&](const char* title) {
        if (categoryOptions == 0)
        {
            return;
        }
        markup << "<div class=\"component-menu-title\">" << title << "</div>" << category.str();
        for (std::string& id : pendingIds)
        {
            m_ComponentPickIds.push_back(std::move(id));
        }
        optionCount += categoryOptions;
        pendingIds.clear();
    };
    entt::registry& registry = m_Editor.World().Registry();

    beginCategory();
    if (!registry.all_of<MeshComponent>(*entity)) componentOption("add-component-mesh", "mesh", "Mesh");
    if (!registry.all_of<VisibilityComponent>(*entity))
        componentOption("add-component-visibility", "visibility", "Visibility");
    if (!registry.all_of<DirectionalLightComponent>(*entity))
        componentOption("add-component-light", "light", "Directional Light");
    if (!registry.all_of<PointLightComponent>(*entity))
        componentOption("add-component-pointlight", "point-light", "Point Light");
    if (!registry.all_of<SpotLightComponent>(*entity))
        componentOption("add-component-spotlight", "spot-light", "Spot Light");
    if (!registry.all_of<EnvironmentComponent>(*entity))
        componentOption("add-component-environment", "environment", "Environment");
    if (!registry.all_of<CameraComponent>(*entity))
        componentOption("add-component-camera", "camera", "Camera");
    if (!registry.all_of<ParticleEmitterComponent>(*entity))
        componentOption("add-component-particle", "particles", "Particle Emitter");
    endCategory("Rendering");

    beginCategory();
    if (!registry.all_of<AudioSourceComponent>(*entity))
        componentOption("add-component-audio-source", "audio", "Audio Source");
    if (!registry.all_of<AudioListenerComponent>(*entity))
        componentOption("add-component-audio-listener", "audio", "Audio Listener");
    endCategory("Audio");

    beginCategory();
    if (!registry.all_of<UICanvasComponent>(*entity))
        componentOption("add-component-ui-canvas", "ui", "UI Canvas");
    endCategory("UI");

    beginCategory();
    if (!registry.all_of<TerrainComponent>(*entity))
        componentOption("add-component-terrain", "terrain", "Terrain");
    if (!registry.all_of<SkeletonComponent>(*entity))
        componentOption("add-component-skeleton", "mesh", "Skeleton");
    if (!registry.all_of<AnimatorComponent>(*entity))
        componentOption("add-component-animator", "mesh", "Animator");
    endCategory("World");

    beginCategory();
    if (!registry.all_of<JoltBodyComponent>(*entity))
        componentOption("add-component-jolt", "physics3d", "Jolt Body 3D");
    if (!registry.all_of<Box2DBodyComponent>(*entity))
        componentOption("add-component-box2d", "physics2d", "Box2D Body");
    endCategory("Physics");

    beginCategory();
    if (!registry.all_of<NetworkIdentityComponent>(*entity))
        componentOption("add-component-network", "network", "Network Identity");
    endCategory("Networking");

    beginCategory();
    if (!registry.all_of<ScriptComponent>(*entity))
        componentOption("add-component-script", "script", "Script");
    endCategory("Scripting");

    if (optionCount == 0)
    {
        markup << "<div class=\"component-empty\">"
               << (m_ComponentFilter.empty() ? "No components available" : "No matching components")
               << "</div>";
        m_ComponentPickIds.clear();
    }
    list->SetInnerRML(markup.str());
    list->SetScrollTop(0.0F);
}

bool SceneController::TryPickComponentOption(const float mouseX, const float mouseY)
{
    if (!m_AddComponentOpen || m_Document == nullptr || m_ComponentPickIds.empty())
    {
        return false;
    }
    Rml::Element* list = m_Document->GetElementById("component-list");
    if (list == nullptr)
    {
        return false;
    }
    const Rml::Vector2f listPos = list->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f listSize = list->GetBox().GetSize(Rml::BoxArea::Border);
    if (mouseX < listPos.x || mouseY < listPos.y || mouseX >= listPos.x + listSize.x ||
        mouseY >= listPos.y + listSize.y)
    {
        return false;
    }

    for (const std::string& id : m_ComponentPickIds)
    {
        Rml::Element* option = m_Document->GetElementById(id);
        if (option == nullptr)
        {
            continue;
        }
        const Rml::Vector2f pos = option->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f size = option->GetBox().GetSize(Rml::BoxArea::Border);
        if (size.x <= 0.0F || size.y <= 0.0F)
        {
            continue;
        }
        if (mouseX >= pos.x && mouseY >= pos.y && mouseX < pos.x + size.x &&
            mouseY < pos.y + size.y)
        {
            if (m_Editor.AddComponent(id))
            {
                CloseComponentPicker();
                Refresh();
                return true;
            }
            return false;
        }
    }
    return false;
}

bool SceneController::TryToggleComponentPicker(const float mouseX, const float mouseY)
{
    if (m_Document == nullptr || !m_Editor.Selection())
    {
        return false;
    }
    Rml::Element* anchor = m_Document->GetElementById("add-component");
    if (anchor == nullptr)
    {
        return false;
    }
    const Rml::Vector2f pos = anchor->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = anchor->GetBox().GetSize(Rml::BoxArea::Border);
    if (mouseX < pos.x || mouseY < pos.y || mouseX >= pos.x + size.x || mouseY >= pos.y + size.y)
    {
        return false;
    }
    ToggleComponentPicker(*anchor);
    return true;
}

void SceneController::CloseComponentPicker()
{
    if (m_Document != nullptr)
    {
        if (Rml::Element* popup = m_Document->GetElementById("component-popup"))
        {
            popup->SetInnerRML("");
            popup->SetClass("hidden", true);
            popup->RemoveProperty("height");
            popup->RemoveProperty("max-height");
            popup->RemoveProperty("overflow-y");
        }
    }
    m_ComponentFilter.clear();
    m_ComponentPickIds.clear();
    m_AddComponentOpen = false;
}

bool SceneController::BeginNumericScrub(const float mouseX, const float mouseY)
{
    m_NumericDrag.reset();
    if (m_AddComponentOpen || m_Document == nullptr || !m_Editor.Selection())
    {
        return false;
    }
    Rml::Element* inspector = m_Document->GetElementById("inspector-content");
    Rml::Element* input = NumericInputAt(inspector, mouseX, mouseY);
    if (input == nullptr)
    {
        return false;
    }
    const auto before = EntitySnapshot::Capture(m_Editor.World(), *m_Editor.Selection());
    if (!before)
    {
        return false;
    }
    m_NumericDrag = NumericDragState{
        input->GetId(), mouseX, InputFloat(input, 0.0F), *before, false, false};
    return true;
}

bool SceneController::UpdateNumericScrub(const float mouseX)
{
    if (!m_NumericDrag)
    {
        return false;
    }
    const float deltaX = mouseX - m_NumericDrag->StartMouseX;
    if (!m_NumericDrag->Dragging && std::abs(deltaX) < 3.0F)
    {
        return false;
    }
    m_NumericDrag->Dragging = true;
    const float value = m_NumericDrag->StartValue + deltaX *
        SceneEditor::NumericDragSensitivity(m_NumericDrag->Id, m_NumericDrag->StartValue);
    if (!m_Editor.ApplyNumericField(m_NumericDrag->Id, value))
    {
        return true;
    }
    if (const std::optional<float> displayed = m_Editor.NumericFieldValue(m_NumericDrag->Id))
    {
        if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(
                m_Document->GetElementById(m_NumericDrag->Id)))
        {
            std::ostringstream text;
            text << std::setprecision(6) << *displayed;
            input->SetValue(text.str());
        }
    }
    m_NumericDrag->Changed = true;
    return true;
}

bool SceneController::EndNumericScrub()
{
    if (!m_NumericDrag)
    {
        return false;
    }
    const bool wasDragging = m_NumericDrag->Dragging;
    if (m_NumericDrag->Changed && m_Editor.Selection())
    {
        if (auto after = EntitySnapshot::Capture(m_Editor.World(), *m_Editor.Selection()))
        {
            m_Editor.History().Push(std::make_unique<SnapshotEntityCommand>(
                m_Editor.World(), std::move(m_NumericDrag->Before), std::move(*after),
                "Scrub Property"));
            m_Editor.MarkChanged();
        }
    }
    m_NumericDrag.reset();
    return wasDragging;
}

bool SceneController::NumericScrubActive() const noexcept
{
    return m_NumericDrag.has_value();
}

void SceneController::Handle(Rml::Event& event)
{
    Rml::Element* source = event.GetCurrentElement();
    if (source != nullptr && source->GetId().empty())
    {
        for (Rml::Element* parent = source->GetParentNode(); parent != nullptr;
             parent = parent->GetParentNode())
        {
            if (!parent->GetId().empty())
            {
                source = parent;
                break;
            }
        }
    }
    const std::string id = source ? std::string{source->GetId()} : std::string{};
    if (id == "add")
    {
        m_Editor.CreateEntity("Entity");
        Refresh();
        return;
    }
    if (id.rfind("entity-", 0) == 0)
    {
        const auto selected = Uuid::Parse(id.substr(7));
        if (selected)
        {
            m_Editor.SetSelection(*selected, true);
            Refresh();
        }
        return;
    }
    if (!m_Editor.Selection())
    {
        return;
    }
    if (id == "scene-delete")
    {
        m_Editor.DeleteSelection();
        Refresh();
        return;
    }
    if (id == "scene-duplicate")
    {
        m_Editor.DuplicateSelection();
        Refresh();
        return;
    }
    const auto entity = m_Editor.World().Find(*m_Editor.Selection());
    if (!entity)
    {
        return;
    }
    IWorld& world = m_Editor.World();
    entt::registry& registry = world.Registry();

    if (id == "component-search")
    {
        std::string value;
        if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(source))
        {
            value = input->GetValue();
        }
        m_ComponentFilter = Lower(std::move(value));
        RebuildComponentPickerList();
        return;
    }
    if (id.rfind("remove-", 0) == 0)
    {
        if (m_Editor.RemoveComponent(id))
        {
            Refresh();
        }
        return;
    }
    if (id == "script-enabled")
    {
        if (ScriptComponent* scripts = registry.try_get<ScriptComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            scripts->Enabled = !scripts->Enabled;
            m_Editor.EndEditTransaction("Toggle Scripts");
            RebuildInspector();
        }
        return;
    }
    if (id == "script-target-clear")
    {
        if (m_Editor.ClearScriptTarget())
        {
            RebuildInspector();
        }
        return;
    }
    if (id == "mesh-kind")
    {
        if (MeshComponent* mesh = registry.try_get<MeshComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            mesh->Kind = static_cast<MeshKind>(
                (static_cast<unsigned>(mesh->Kind) + 1U) % MeshKindCount);
            m_Editor.EndEditTransaction("Change Mesh Primitive");
            RebuildInspector();
        }
        return;
    }
    if (id == "pointlight-enabled")
    {
        if (PointLightComponent* light = registry.try_get<PointLightComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            light->Enabled = !light->Enabled;
            m_Editor.EndEditTransaction("Toggle Point Light");
            RebuildInspector();
        }
        return;
    }
    if (id == "light-cast-shadows")
    {
        if (DirectionalLightComponent* light =
                registry.try_get<DirectionalLightComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            light->CastShadows = !light->CastShadows;
            m_Editor.EndEditTransaction("Toggle Cast Shadows");
            RebuildInspector();
        }
        return;
    }
    if (id == "light-shadow-res")
    {
        if (DirectionalLightComponent* light =
                registry.try_get<DirectionalLightComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            const int current = light->ShadowMapResolution > 0 ? light->ShadowMapResolution : 2048;
            light->ShadowMapResolution =
                current == 1024 ? 2048 : current == 2048 ? 4096 : 1024;
            m_Editor.EndEditTransaction("Change Shadow Resolution");
            RebuildInspector();
        }
        return;
    }
    if (id == "particle-enabled" || id == "particle-play-on-start" || id == "particle-loop" ||
        id == "particle-shape")
    {
        if (ParticleEmitterComponent* emitter =
                registry.try_get<ParticleEmitterComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            if (id == "particle-enabled") emitter->Enabled = !emitter->Enabled;
            else if (id == "particle-play-on-start") emitter->PlayOnStart = !emitter->PlayOnStart;
            else if (id == "particle-loop") emitter->Loop = !emitter->Loop;
            else
            {
                emitter->Shape = static_cast<EmitterShape>(
                    (static_cast<unsigned>(emitter->Shape) + 1U) % 3U);
            }
            m_Editor.EndEditTransaction("Edit Particle Emitter");
            RebuildInspector();
        }
        return;
    }
    if (id == "audio-play-on-start" || id == "audio-loop" || id == "audio-spatial")
    {
        if (AudioSourceComponent* audio = registry.try_get<AudioSourceComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            if (id == "audio-play-on-start") audio->PlayOnStart = !audio->PlayOnStart;
            else if (id == "audio-loop") audio->Loop = !audio->Loop;
            else audio->Spatial = !audio->Spatial;
            m_Editor.EndEditTransaction("Edit Audio Source");
            RebuildInspector();
        }
        return;
    }
    if (id == "audio-listener-active")
    {
        if (AudioListenerComponent* listener = registry.try_get<AudioListenerComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            listener->Active = !listener->Active;
            m_Editor.EndEditTransaction("Edit Audio Listener");
            RebuildInspector();
        }
        return;
    }
    if (id == "ui-render-editor" || id == "ui-render-game")
    {
        if (UICanvasComponent* ui = registry.try_get<UICanvasComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            if (id == "ui-render-editor") ui->RenderInEditor = !ui->RenderInEditor;
            else ui->RenderInGame = !ui->RenderInGame;
            m_Editor.EndEditTransaction("Edit UI Canvas");
            RebuildInspector();
        }
        return;
    }
    if (id == "terrain-shadows")
    {
        if (TerrainComponent* terrain = registry.try_get<TerrainComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            terrain->CastShadows = !terrain->CastShadows;
            m_Editor.EndEditTransaction("Edit Terrain");
            RebuildInspector();
        }
        return;
    }
    if (id == "animator-loop" || id == "animator-playing" || id == "animator-next-clip")
    {
        if (AnimatorComponent* animator = registry.try_get<AnimatorComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            if (id == "animator-loop")
            {
                animator->Loop = !animator->Loop;
            }
            else if (id == "animator-playing")
            {
                animator->Playing = !animator->Playing;
            }
            else if (m_Editor.GltfMeshes() != nullptr)
            {
                if (const MeshComponent* mesh = registry.try_get<MeshComponent>(*entity);
                    mesh != nullptr && mesh->ImportedMesh.IsValid())
                {
                    if (const GltfMeshAsset* asset =
                            m_Editor.GltfMeshes()->Get(mesh->ImportedMesh);
                        asset != nullptr && !asset->Animations.empty())
                    {
                        const int count = static_cast<int>(asset->Animations.size());
                        animator->ClipIndex = (std::max(animator->ClipIndex, 0) + 1) % count;
                        animator->ClipName =
                            asset->Animations[static_cast<std::size_t>(animator->ClipIndex)].Name;
                        animator->CurrentTime = 0.0F;
                    }
                }
            }
            m_Editor.EndEditTransaction("Edit Animator");
            RebuildInspector();
        }
        return;
    }
    if (id.rfind("animator-", 0) == 0)
    {
        const std::optional<float> current = m_Editor.NumericFieldValue(id);
        if (current)
        {
            m_Editor.BeginEditTransaction();
            if (m_Editor.ApplyNumericField(id, InputFloat(source, *current)))
            {
                m_Editor.EndEditTransaction("Edit Animator");
            }
            else
            {
                m_Editor.CancelEditTransaction();
            }
        }
        return;
    }
    if (id.rfind("ui-", 0) == 0)
    {
        const std::optional<float> current = m_Editor.NumericFieldValue(id);
        if (current)
        {
            m_Editor.BeginEditTransaction();
            if (m_Editor.ApplyNumericField(id, InputFloat(source, *current)))
            {
                m_Editor.EndEditTransaction("Edit UI Canvas");
            }
            else
            {
                m_Editor.CancelEditTransaction();
            }
        }
        return;
    }
    if (id.rfind("terrain-", 0) == 0)
    {
        const std::optional<float> current = m_Editor.NumericFieldValue(id);
        if (current)
        {
            m_Editor.BeginEditTransaction();
            if (m_Editor.ApplyNumericField(id, InputFloat(source, *current)))
            {
                m_Editor.EndEditTransaction("Edit Terrain");
                if (id == "terrain-layer-count")
                {
                    RebuildInspector();
                }
            }
            else
            {
                m_Editor.CancelEditTransaction();
            }
        }
        return;
    }
    if (id.rfind("audio-", 0) == 0)
    {
        const std::optional<float> current = m_Editor.NumericFieldValue(id);
        if (current)
        {
            m_Editor.BeginEditTransaction();
            if (m_Editor.ApplyNumericField(id, InputFloat(source, *current)))
            {
                m_Editor.EndEditTransaction("Edit Audio Source");
            }
            else
            {
                m_Editor.CancelEditTransaction();
            }
        }
        return;
    }
    if (id.rfind("particle-", 0) == 0)
    {
        const std::optional<float> current = m_Editor.NumericFieldValue(id);
        if (current)
        {
            m_Editor.BeginEditTransaction();
            if (m_Editor.ApplyNumericField(id, InputFloat(source, *current)))
            {
                m_Editor.EndEditTransaction("Edit Particle Emitter");
            }
            else
            {
                m_Editor.CancelEditTransaction();
            }
        }
        return;
    }
    if (id == "spotlight-enabled")
    {
        if (SpotLightComponent* light = registry.try_get<SpotLightComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            light->Enabled = !light->Enabled;
            m_Editor.EndEditTransaction("Toggle Spot Light");
            RebuildInspector();
        }
        return;
    }
    if (id == "env-fog-enabled")
    {
        if (EnvironmentComponent* env = registry.try_get<EnvironmentComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            env->FogEnabled = !env->FogEnabled;
            m_Editor.EndEditTransaction("Toggle Fog");
            RebuildInspector();
        }
        return;
    }
    if (id == "env-bloom" || id == "env-tonemap" || id == "env-colorgrading")
    {
        if (EnvironmentComponent* env = registry.try_get<EnvironmentComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            if (id == "env-bloom") env->BloomEnabled = !env->BloomEnabled;
            else if (id == "env-tonemap") env->TonemapEnabled = !env->TonemapEnabled;
            else env->ColorGradingEnabled = !env->ColorGradingEnabled;
            m_Editor.EndEditTransaction("Toggle Post Processing");
            RebuildInspector();
        }
        return;
    }
    if (id == "env-antialias")
    {
        if (EnvironmentComponent* env = registry.try_get<EnvironmentComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            env->AntiAlias = static_cast<AntiAliasMode>(
                (static_cast<unsigned>(env->AntiAlias) + 1U) % AntiAliasModeCount);
            env->FxaaEnabled = env->AntiAlias != AntiAliasMode::Off;
            m_Editor.EndEditTransaction("Change Anti-Alias Mode");
            RebuildInspector();
        }
        return;
    }
    if (id == "env-primary")
    {
        if (registry.all_of<EnvironmentComponent>(*entity))
        {
            std::vector<EntitySnapshot> before;
            std::vector<Uuid> affected;
            for (const auto [other, environment, uuid] :
                 registry.view<EnvironmentComponent, const UuidComponent>().each())
            {
                if (environment.Primary || other == *entity)
                {
                    if (auto snapshot = EntitySnapshot::Capture(world, uuid.Id))
                    {
                        before.push_back(std::move(*snapshot));
                        affected.push_back(uuid.Id);
                    }
                }
            }
            for (const auto [other, environment] : registry.view<EnvironmentComponent>().each())
            {
                environment.Primary = other == *entity;
            }
            std::vector<EntitySnapshot> after;
            for (const Uuid& uuid : affected)
            {
                if (auto snapshot = EntitySnapshot::Capture(world, uuid))
                {
                    after.push_back(std::move(*snapshot));
                }
            }
            m_Editor.History().Push(std::make_unique<MultiSnapshotCommand>(
                world, std::move(before), std::move(after), "Make Environment Primary"));
            m_Editor.MarkChanged();
            RebuildInspector();
        }
        return;
    }
    if (id.rfind("vis-", 0) == 0)
    {
        if (VisibilityComponent* visibility = registry.try_get<VisibilityComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            if (id == "vis-editor") visibility->VisibleInEditor = !visibility->VisibleInEditor;
            else if (id == "vis-game") visibility->VisibleInGame = !visibility->VisibleInGame;
            else if (id == "vis-selectable") visibility->Selectable = !visibility->Selectable;
            else { m_Editor.CancelEditTransaction(); return; }
            m_Editor.EndEditTransaction("Edit Visibility");
            RebuildInspector();
        }
        return;
    }
    if (id.rfind("pointlight-", 0) == 0 || id.rfind("spotlight-", 0) == 0 ||
        id.rfind("env-", 0) == 0)
    {
        const std::optional<float> current = m_Editor.NumericFieldValue(id);
        if (current)
        {
            m_Editor.BeginEditTransaction();
            if (m_Editor.ApplyNumericField(id, InputFloat(source, *current)))
            {
                m_Editor.EndEditTransaction("Edit Component");
            }
            else
            {
                m_Editor.CancelEditTransaction();
            }
        }
        return;
    }
    if (id == "entity-name")
    {
        if (const auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(source))
        {
            m_Editor.RenameSelection(input->GetValue());
            RebuildTree();
        }
        return;
    }
    if (id == "replicated-toggle")
    {
        m_Editor.BeginEditTransaction();
        if (registry.all_of<NetworkIdentityComponent>(*entity))
        {
            registry.remove<NetworkIdentityComponent>(*entity);
        }
        else
        {
            registry.emplace<NetworkIdentityComponent>(*entity);
        }
        m_Editor.EndEditTransaction("Toggle Replication");
        RebuildInspector();
        return;
    }
    if (id == "mesh-material-assign")
    {
        if (m_Editor.AssignMaterialFromSelection())
        {
            RebuildInspector();
        }
        return;
    }
    if (id == "mesh-material-clear")
    {
        if (m_Editor.ClearMaterial())
        {
            RebuildInspector();
        }
        return;
    }
    if (id == "mesh-imported-assign")
    {
        if (m_Editor.AssignMeshFromSelection())
        {
            RebuildInspector();
        }
        return;
    }
    if (id == "mesh-imported-clear")
    {
        if (m_Editor.ClearImportedMesh())
        {
            RebuildInspector();
        }
        return;
    }
    if (id == "mat-alpha-mode" || id == "mat-double-sided")
    {
        if (ApplyMaterialFieldEdit(id, source))
        {
            RebuildInspector();
        }
        return;
    }
    if (MeshComponent* mesh = registry.try_get<MeshComponent>(*entity))
    {
        if (id.rfind("mesh-color-", 0) == 0 || id == "mesh-metallic" || id == "mesh-roughness" ||
            id == "mesh-material" || id == "mesh-imported")
        {
            m_Editor.BeginEditTransaction();
            if (id.rfind("mesh-color-", 0) == 0)
            {
                const int channel = id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : 2;
                mesh->BaseColor[channel] =
                    std::clamp(InputFloat(source, mesh->BaseColor[channel]), 0.0F, 1.0F);
            }
            else if (id == "mesh-metallic")
            {
                mesh->Metallic = std::clamp(InputFloat(source, mesh->Metallic), 0.0F, 1.0F);
            }
            else if (id == "mesh-roughness")
            {
                mesh->Roughness = std::clamp(InputFloat(source, mesh->Roughness), 0.0F, 1.0F);
            }
            else if (id == "mesh-material")
            {
                if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(source))
                {
                    if (auto parsed = Uuid::Parse(input->GetValue()))
                    {
                        mesh->Material = *parsed;
                    }
                    else if (input->GetValue().empty())
                    {
                        mesh->Material = AssetHandle{};
                    }
                    else
                    {
                        m_Editor.Report("Invalid material handle");
                        m_Editor.CancelEditTransaction();
                        return;
                    }
                }
            }
            else if (id == "mesh-imported")
            {
                if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(source))
                {
                    if (auto parsed = Uuid::Parse(input->GetValue()))
                    {
                        mesh->ImportedMesh = *parsed;
                    }
                    else if (input->GetValue().empty())
                    {
                        mesh->ImportedMesh = AssetHandle{};
                    }
                    else
                    {
                        m_Editor.Report("Invalid mesh handle");
                        m_Editor.CancelEditTransaction();
                        return;
                    }
                }
            }
            m_Editor.EndEditTransaction(
                id == "mesh-imported" ? "Edit Imported Mesh" : "Edit Mesh Material");
            if (id == "mesh-material" || id == "mesh-imported")
            {
                RebuildInspector();
            }
            return;
        }
        if (id.rfind("mat-", 0) == 0)
        {
            if (ApplyMaterialFieldEdit(id, source))
            {
                if (id == "mat-name" || id.rfind("mat-tex-", 0) == 0)
                {
                    RebuildInspector();
                }
            }
            return;
        }
        static_cast<void>(mesh);
    }
    if (DirectionalLightComponent* light = registry.try_get<DirectionalLightComponent>(*entity))
    {
        if (id.rfind("light-color-", 0) == 0 || id == "light-intensity" ||
            id == "light-shadow-bias" || id == "light-shadow-strength")
        {
            m_Editor.BeginEditTransaction();
            if (id.rfind("light-color-", 0) == 0)
            {
                const int channel = id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : 2;
                light->Color[channel] = std::clamp(InputFloat(source, light->Color[channel]), 0.0F, 1.0F);
            }
            else if (id == "light-intensity")
            {
                light->Intensity = std::max(InputFloat(source, light->Intensity), 0.0F);
            }
            else if (id == "light-shadow-bias")
            {
                light->ShadowBias =
                    std::clamp(InputFloat(source, light->ShadowBias), 0.0F, 0.05F);
            }
            else
            {
                light->ShadowStrength =
                    std::clamp(InputFloat(source, light->ShadowStrength), 0.0F, 1.0F);
            }
            m_Editor.EndEditTransaction("Edit Directional Light");
            return;
        }
    }
    if (NetworkIdentityComponent* network = registry.try_get<NetworkIdentityComponent>(*entity))
    {
        if (id == "network-id")
        {
            m_Editor.BeginEditTransaction();
            network->NetworkId = static_cast<std::uint64_t>(
                std::max(InputFloat(source, static_cast<float>(network->NetworkId)), 0.0F));
            m_Editor.EndEditTransaction("Edit Network Identity");
            return;
        }
        if (id == "network-authority")
        {
            m_Editor.BeginEditTransaction();
            const unsigned next = (static_cast<unsigned>(network->Authority) + 1U) % 3U;
            network->Authority = static_cast<NetworkAuthority>(next);
            m_Editor.EndEditTransaction("Edit Network Authority");
            RebuildInspector();
            return;
        }
    }
    if (JoltBodyComponent* body = registry.try_get<JoltBodyComponent>(*entity))
    {
        if (id == "jolt-mass")
        {
            m_Editor.BeginEditTransaction();
            body->Mass = std::clamp(InputFloat(source, body->Mass), 0.001F, 1000000.0F);
            m_Editor.EndEditTransaction("Edit Jolt Body Mass");
            return;
        }
        if (id == "jolt-friction")
        {
            m_Editor.BeginEditTransaction();
            body->Friction = std::clamp(InputFloat(source, body->Friction), 0.0F, 2.0F);
            m_Editor.EndEditTransaction("Edit Jolt Body Friction");
            return;
        }
        if (id.rfind("jolt-extent-", 0) == 0)
        {
            m_Editor.BeginEditTransaction();
            const int axis = id.back() == 'x' ? 0 : id.back() == 'y' ? 1 : 2;
            body->HalfExtent[axis] = std::max(InputFloat(source, body->HalfExtent[axis]), 0.001F);
            m_Editor.EndEditTransaction("Edit Jolt Body");
            return;
        }
        if (id == "jolt-dynamic")
        {
            m_Editor.BeginEditTransaction();
            body->Dynamic = !body->Dynamic;
            m_Editor.EndEditTransaction("Toggle Jolt Body Mode");
            RebuildInspector();
            return;
        }
    }
    if (Box2DBodyComponent* body = registry.try_get<Box2DBodyComponent>(*entity))
    {
        if (id.rfind("box2d-extent-", 0) == 0)
        {
            m_Editor.BeginEditTransaction();
            const int axis = id.back() == 'x' ? 0 : 1;
            body->HalfExtent[axis] = std::max(InputFloat(source, body->HalfExtent[axis]), 0.001F);
            m_Editor.EndEditTransaction("Edit Box2D Body");
            return;
        }
        if (id == "box2d-dynamic")
        {
            m_Editor.BeginEditTransaction();
            body->Dynamic = !body->Dynamic;
            m_Editor.EndEditTransaction("Toggle Box2D Body Mode");
            RebuildInspector();
            return;
        }
    }
    if (TransformComponent* transform = registry.try_get<TransformComponent>(*entity))
    {
        const TransformComponent before = *transform;
        TransformComponent after = before;
        glm::vec3* target = nullptr;
        if (id.rfind("position-", 0) == 0) target = &after.Position;
        if (id.rfind("scale-", 0) == 0) target = &after.Scale;
        if (target != nullptr)
        {
            const int axis = id.back() == 'x' ? 0 : id.back() == 'y' ? 1 : 2;
            (*target)[axis] = InputFloat(source, (*target)[axis]);
            m_Editor.History().Push(std::make_unique<TransformEntityCommand>(
                world, *m_Editor.Selection(), before, after));
            m_Editor.MarkChanged();
            return;
        }
        if (id.rfind("rotation-", 0) == 0)
        {
            glm::vec3 euler = glm::degrees(glm::eulerAngles(after.Rotation));
            const int axis = id.back() == 'x' ? 0 : id.back() == 'y' ? 1 : 2;
            euler[axis] = InputFloat(source, euler[axis]);
            after.Rotation = glm::quat{glm::radians(euler)};
            m_Editor.History().Push(std::make_unique<TransformEntityCommand>(
                world, *m_Editor.Selection(), before, after));
            m_Editor.MarkChanged();
            return;
        }
    }
    if (id.rfind("camera-", 0) == 0)
    {
        if (CameraComponent* camera = registry.try_get<CameraComponent>(*entity))
        {
            m_Editor.BeginEditTransaction();
            if (id == "camera-fov")
                camera->FieldOfView = std::clamp(InputFloat(source, camera->FieldOfView), 1.0F, 179.0F);
            else if (id == "camera-near")
                camera->NearPlane = std::max(InputFloat(source, camera->NearPlane), 0.001F);
            else if (id == "camera-far")
                camera->FarPlane = std::max(InputFloat(source, camera->FarPlane), camera->NearPlane + 0.001F);
            else if (id == "camera-primary")
                camera->Primary = !camera->Primary;
            else
            {
                m_Editor.CancelEditTransaction();
                return;
            }
            m_Editor.EndEditTransaction("Edit Camera");
            if (id == "camera-primary")
            {
                RebuildInspector();
            }
            return;
        }
    }
}

bool SceneController::ApplyMaterialFieldEdit(const std::string_view id, Rml::Element* source)
{
    if (!m_Editor.Selection() || m_Editor.Assets() == nullptr)
    {
        return false;
    }
    const auto entity = m_Editor.World().Find(*m_Editor.Selection());
    if (!entity)
    {
        return false;
    }
    const MeshComponent* mesh = m_Editor.World().Registry().try_get<MeshComponent>(*entity);
    if (mesh == nullptr || !mesh->Material.IsValid())
    {
        return false;
    }
    Result<MaterialAsset> loaded = m_Editor.Assets()->LoadMaterial(mesh->Material);
    if (!loaded)
    {
        m_Editor.Report(loaded.ErrorMessage());
        return false;
    }
    MaterialAsset asset = std::move(loaded).Value();

    auto parseHandle = [](Rml::Element* element) -> std::optional<AssetHandle> {
        auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(element);
        if (input == nullptr)
        {
            return std::nullopt;
        }
        if (input->GetValue().empty())
        {
            return AssetHandle{};
        }
        return Uuid::Parse(input->GetValue());
    };

    if (id == "mat-name")
    {
        if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(source))
        {
            asset.Name = std::string{input->GetValue()};
        }
    }
    else if (id.rfind("mat-basecolor-", 0) == 0)
    {
        const int channel = id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : id.back() == 'b' ? 2 : 3;
        asset.BaseColor[channel] =
            std::clamp(InputFloat(source, asset.BaseColor[channel]), 0.0F, 1.0F);
    }
    else if (id == "mat-metallic")
    {
        asset.Metallic = std::clamp(InputFloat(source, asset.Metallic), 0.0F, 1.0F);
    }
    else if (id == "mat-roughness")
    {
        asset.Roughness = std::clamp(InputFloat(source, asset.Roughness), 0.0F, 1.0F);
    }
    else if (id.rfind("mat-emissive-", 0) == 0 && id != "mat-emissive-intensity")
    {
        const int channel = id.back() == 'r' ? 0 : id.back() == 'g' ? 1 : 2;
        asset.EmissiveColor[channel] =
            std::clamp(InputFloat(source, asset.EmissiveColor[channel]), 0.0F, 1.0F);
    }
    else if (id == "mat-emissive-intensity")
    {
        asset.EmissiveIntensity = std::max(InputFloat(source, asset.EmissiveIntensity), 0.0F);
    }
    else if (id == "mat-alpha-cutoff")
    {
        asset.AlphaCutoff = std::clamp(InputFloat(source, asset.AlphaCutoff), 0.0F, 1.0F);
    }
    else if (id == "mat-alpha-mode")
    {
        const unsigned next = (static_cast<unsigned>(asset.AlphaMode) + 1U) % 3U;
        asset.AlphaMode = static_cast<AlphaMode>(next);
    }
    else if (id == "mat-double-sided")
    {
        asset.DoubleSided = !asset.DoubleSided;
    }
    else if (id.rfind("mat-uv-scale-", 0) == 0)
    {
        const int axis = id.back() == 'x' ? 0 : 1;
        asset.UVScale[axis] = InputFloat(source, asset.UVScale[axis]);
    }
    else if (id.rfind("mat-uv-offset-", 0) == 0)
    {
        const int axis = id.back() == 'x' ? 0 : 1;
        asset.UVOffset[axis] = InputFloat(source, asset.UVOffset[axis]);
    }
    else if (id == "mat-tex-basecolor" || id == "mat-tex-normal" || id == "mat-tex-mr" ||
             id == "mat-tex-emissive")
    {
        const auto parsed = parseHandle(source);
        if (!parsed)
        {
            m_Editor.Report("Invalid texture handle");
            return false;
        }
        if (parsed->IsValid() && m_Editor.Assets()->Meta(*parsed) != nullptr &&
            m_Editor.Assets()->Meta(*parsed)->Type != "Texture")
        {
            m_Editor.Report("Handle is not a Texture asset");
            return false;
        }
        if (id == "mat-tex-basecolor")
        {
            asset.BaseColorTexture = *parsed;
        }
        else if (id == "mat-tex-normal")
        {
            asset.NormalTexture = *parsed;
        }
        else if (id == "mat-tex-mr")
        {
            asset.MetallicRoughnessTexture = *parsed;
        }
        else
        {
            asset.EmissiveTexture = *parsed;
        }
    }
    else
    {
        return false;
    }

    if (Result<void> saved = m_Editor.Assets()->SaveMaterial(asset); !saved)
    {
        m_Editor.Report(saved.ErrorMessage());
        return false;
    }
    return true;
}

bool SceneController::TryApplyInspectorDrop(
    const AssetDragPayload& payload,
    const float mouseX,
    const float mouseY)
{
    if (m_Document == nullptr || !m_Editor.Selection())
    {
        return false;
    }

    const auto hitDropTarget = [this, mouseX, mouseY](const char* id) -> bool {
        Rml::Element* element = m_Document->GetElementById(id);
        if (element == nullptr)
        {
            return false;
        }
        const Rml::Vector2f position = element->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
        return mouseX >= position.x && mouseY >= position.y &&
            mouseX < position.x + size.x && mouseY < position.y + size.y;
    };

    if (payload.AssetType == "Material" &&
        (hitDropTarget("mesh-material-drop") || hitDropTarget("mesh-material")))
    {
        if (!m_Editor.World().Find(*m_Editor.Selection()))
        {
            return false;
        }
        if (m_Editor.AssignMaterial(payload.Handle))
        {
            RebuildInspector();
        }
        return true;
    }

    if (payload.AssetType == "Audio" &&
        (hitDropTarget("audio-sound-drop") || hitDropTarget("audio-sound")))
    {
        if (!m_Editor.World().Find(*m_Editor.Selection()))
        {
            return false;
        }
        if (m_Editor.AssignSound(payload.Handle))
        {
            RebuildInspector();
        }
        return true;
    }

    if (payload.AssetType == "UI" &&
        (hitDropTarget("ui-asset-drop") || hitDropTarget("ui-asset")))
    {
        if (!m_Editor.World().Find(*m_Editor.Selection()))
        {
            return false;
        }
        if (m_Editor.AssignUIAsset(payload.Handle))
        {
            RebuildInspector();
        }
        return true;
    }

    if (payload.AssetType == "UIStyle" &&
        (hitDropTarget("ui-style-drop") || hitDropTarget("ui-style")))
    {
        if (!m_Editor.World().Find(*m_Editor.Selection()))
        {
            return false;
        }
        if (m_Editor.AssignUIStyle(payload.Handle))
        {
            RebuildInspector();
        }
        return true;
    }

    if (payload.AssetType == "Texture")
    {
        const auto entity = m_Editor.World().Find(*m_Editor.Selection());
        if (entity)
        {
            if (TerrainComponent* terrain =
                    m_Editor.World().Registry().try_get<TerrainComponent>(*entity))
            {
                if (hitDropTarget("terrain-heightmap-drop") || hitDropTarget("terrain-heightmap"))
                {
                    if (m_Editor.AssignTerrainHeightmap(payload.Handle))
                    {
                        RebuildInspector();
                    }
                    return true;
                }
                const int layerCount = std::clamp(terrain->LayerCount, 1, 4);
                for (int i = 0; i < layerCount; ++i)
                {
                    const std::string dropId = "terrain-layer-" + std::to_string(i) + "-tex-drop";
                    const std::string fieldId = "terrain-layer-" + std::to_string(i) + "-tex";
                    if (hitDropTarget(dropId.c_str()) || hitDropTarget(fieldId.c_str()))
                    {
                        auto before = EntitySnapshot::Capture(m_Editor.World(), *m_Editor.Selection());
                        terrain->Layers[i].Texture = payload.Handle;
                        if (auto after = EntitySnapshot::Capture(m_Editor.World(), *m_Editor.Selection());
                            before && after)
                        {
                            m_Editor.History().Push(std::make_unique<SnapshotEntityCommand>(
                                m_Editor.World(), std::move(*before), std::move(*after),
                                "Assign Terrain Layer"));
                        }
                        m_Editor.MarkChanged();
                        RebuildInspector();
                        m_Editor.Report("Assigned terrain layer texture");
                        return true;
                    }
                }
            }
        }
    }

    if (payload.AssetType == "Mesh" &&
        (hitDropTarget("mesh-imported-drop") || hitDropTarget("mesh-imported")))
    {
        if (!m_Editor.World().Find(*m_Editor.Selection()))
        {
            return false;
        }
        if (m_Editor.AssignImportedMesh(payload.Handle))
        {
            RebuildInspector();
        }
        return true;
    }

    if (payload.AssetType == "Texture")
    {
        struct Slot
        {
            const char* DropId;
            const char* FieldId;
            AssetHandle MaterialAsset::* Member;
        };
        const Slot slots[] = {
            {"mat-tex-basecolor-drop", "mat-tex-basecolor", &MaterialAsset::BaseColorTexture},
            {"mat-tex-normal-drop", "mat-tex-normal", &MaterialAsset::NormalTexture},
            {"mat-tex-mr-drop", "mat-tex-mr", &MaterialAsset::MetallicRoughnessTexture},
            {"mat-tex-emissive-drop", "mat-tex-emissive", &MaterialAsset::EmissiveTexture},
        };

        const Slot* hit = nullptr;
        for (const Slot& slot : slots)
        {
            if (hitDropTarget(slot.DropId) || hitDropTarget(slot.FieldId))
            {
                hit = &slot;
                break;
            }
        }
        if (hit == nullptr)
        {
            if (hitDropTarget("mesh-material-drop") || hitDropTarget("mesh-material"))
            {
                m_Editor.Report("Textures cannot be assigned to the Material field");
                return true;
            }
            return false;
        }

        if (m_Editor.Assets() == nullptr)
        {
            m_Editor.Report("No asset database available");
            return true;
        }
        const auto entity = m_Editor.World().Find(*m_Editor.Selection());
        if (!entity)
        {
            return true;
        }
        const MeshComponent* mesh =
            m_Editor.World().Registry().try_get<MeshComponent>(*entity);
        if (mesh == nullptr || !mesh->Material.IsValid())
        {
            m_Editor.Report("Assign a Material before dropping textures");
            return true;
        }
        Result<MaterialAsset> loaded = m_Editor.Assets()->LoadMaterial(mesh->Material);
        if (!loaded)
        {
            m_Editor.Report(loaded.ErrorMessage());
            return true;
        }
        MaterialAsset asset = std::move(loaded).Value();
        asset.*(hit->Member) = payload.Handle;
        if (Result<void> saved = m_Editor.Assets()->SaveMaterial(asset); !saved)
        {
            m_Editor.Report(saved.ErrorMessage());
            return true;
        }
        RebuildInspector();
        m_Editor.Report("Assigned texture to material slot");
        return true;
    }

    if (payload.AssetType == "Material" &&
        (hitDropTarget("mat-tex-basecolor-drop") || hitDropTarget("mat-tex-normal-drop") ||
            hitDropTarget("mat-tex-mr-drop") || hitDropTarget("mat-tex-emissive-drop")))
    {
        m_Editor.Report("Materials cannot be dropped onto texture slots");
        return true;
    }

    return false;
}

bool SceneController::TryApplyEntityDrop(
    const Uuid& entityId, const float mouseX, const float mouseY)
{
    if (m_Document == nullptr || !m_Editor.Selection() || !entityId.IsValid())
    {
        return false;
    }

    const auto hitDropTarget = [this, mouseX, mouseY](const char* id) -> bool {
        Rml::Element* element = m_Document->GetElementById(id);
        if (element == nullptr)
        {
            return false;
        }
        const Rml::Vector2f position = element->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
        return mouseX >= position.x && mouseY >= position.y &&
            mouseX < position.x + size.x && mouseY < position.y + size.y;
    };
    if (!hitDropTarget("script-target-drop") && !hitDropTarget("script-target"))
    {
        return false;
    }

    if (!m_Editor.World().Find(*m_Editor.Selection()))
    {
        return false;
    }
    if (m_Editor.AssignScriptTarget(entityId))
    {
        RebuildInspector();
    }
    return true;
}
} // namespace fadix
