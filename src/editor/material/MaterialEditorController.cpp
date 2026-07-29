#include "editor/material/MaterialEditorController.hpp"

#include "assets/AssetDatabase.hpp"
#include "engine/app/ModuleRegistration.hpp"
#include "engine/assets/IAssetDatabase.hpp"
#include "engine/render/ViewportRenderer.hpp"
#include "engine/scene/IWorld.hpp"
#include "render/ViewportRendererFactory.hpp"
#include "rhi/sdl/SdlRhi.hpp"
#include "runtime/Components.hpp"

#include <entt/entity/registry.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace fadix::editor
{
namespace
{
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
}

MaterialEditorController::MaterialEditorController() = default;
MaterialEditorController::~MaterialEditorController() = default;

void MaterialEditorController::SetUiNotify(UiNotify notify)
{
    m_UiNotify = std::move(notify);
}

void MaterialEditorController::NotifyUi()
{
    if (m_UiNotify)
    {
        m_UiNotify();
    }
}

void MaterialEditorController::Bind(IAssetDatabase& assets, rhi::Device& device)
{
    m_Assets = &assets;
    m_AssetDb = dynamic_cast<AssetDatabase*>(&assets);
    m_Device = &device;

    if (m_Preview)
    {
        m_Preview->SetAssetDatabase(assets);
        Close();
        return;
    }

    m_Preview = CreateViewportRenderer(device, assets);
    m_Preview->SetEditorVisualsEnabled(false);
    m_PreviewWorld = sceneplay::CreateEditWorld();

    entt::registry& registry = m_PreviewWorld->Registry();
    m_PreviewMesh = m_PreviewWorld->Create();
    registry.emplace<NameComponent>(m_PreviewMesh, NameComponent{"PreviewMesh"});
    registry.emplace<TransformComponent>(m_PreviewMesh);
    MeshComponent mesh;
    mesh.Kind = MeshKind::Sphere;
    registry.emplace<MeshComponent>(m_PreviewMesh, mesh);

    const entt::entity light = m_PreviewWorld->Create();
    TransformComponent lightTransform;
    lightTransform.Rotation = glm::quat(glm::radians(glm::vec3{-50.0F, -35.0F, 0.0F}));
    registry.emplace<NameComponent>(light, NameComponent{"PreviewLight"});
    registry.emplace<TransformComponent>(light, lightTransform);
    registry.emplace<DirectionalLightComponent>(light);
}

void MaterialEditorController::Open(const AssetHandle& materialHandle)
{
    if (m_AssetDb == nullptr)
    {
        return;
    }
    Result<MaterialAsset> loaded = m_AssetDb->LoadMaterial(materialHandle);
    if (!loaded)
    {
        return;
    }
    m_Editing = loaded.Value();
    m_Editing.Handle = materialHandle;
    m_Current = materialHandle;

    if (m_PreviewWorld)
    {
        if (auto* mesh = m_PreviewWorld->Registry().try_get<MeshComponent>(m_PreviewMesh))
        {
            mesh->Material = materialHandle;
        }
    }

    m_Open = true;
    NotifyUi();
}

void MaterialEditorController::Close()
{
    m_Open = false;
    m_Current.reset();
    NotifyUi();
}

void MaterialEditorController::SetFloatField(const std::string_view id, const float value)
{
    if (!m_Open)
    {
        return;
    }
    if (id == "mat-ed-basecolor-r") m_Editing.BaseColor.r = value;
    else if (id == "mat-ed-basecolor-g") m_Editing.BaseColor.g = value;
    else if (id == "mat-ed-basecolor-b") m_Editing.BaseColor.b = value;
    else if (id == "mat-ed-basecolor-a") m_Editing.BaseColor.a = value;
    else if (id == "mat-ed-metallic") m_Editing.Metallic = value;
    else if (id == "mat-ed-roughness") m_Editing.Roughness = value;
    else if (id == "mat-ed-emissive-r") m_Editing.EmissiveColor.r = value;
    else if (id == "mat-ed-emissive-g") m_Editing.EmissiveColor.g = value;
    else if (id == "mat-ed-emissive-b") m_Editing.EmissiveColor.b = value;
    else if (id == "mat-ed-emissive-intensity") m_Editing.EmissiveIntensity = value;
    else if (id == "mat-ed-alpha-cutoff") m_Editing.AlphaCutoff = value;
    else if (id == "mat-ed-uv-scale-x") m_Editing.UVScale.x = value;
    else if (id == "mat-ed-uv-scale-y") m_Editing.UVScale.y = value;
    else if (id == "mat-ed-uv-offset-x") m_Editing.UVOffset.x = value;
    else if (id == "mat-ed-uv-offset-y") m_Editing.UVOffset.y = value;
    else return;

    Save();
    NotifyUi();
}

void MaterialEditorController::CycleAlphaMode()
{
    if (!m_Open)
    {
        return;
    }
    m_Editing.AlphaMode =
        static_cast<AlphaMode>((static_cast<int>(m_Editing.AlphaMode) + 1) % 3);
    Save();
    NotifyUi();
}

void MaterialEditorController::ToggleDoubleSided()
{
    if (!m_Open)
    {
        return;
    }
    m_Editing.DoubleSided = !m_Editing.DoubleSided;
    Save();
    NotifyUi();
}

bool MaterialEditorController::HandleButton(const std::string& id)
{
    if (id == "material-editor-close")
    {
        Close();
        return true;
    }
    if (id == "mat-preview-cube")
    {
        SetPreviewShape(0);
        return true;
    }
    if (id == "mat-preview-sphere")
    {
        SetPreviewShape(1);
        return true;
    }
    if (id == "mat-preview-plane")
    {
        SetPreviewShape(2);
        return true;
    }
    if (id == "mat-ed-alpha-mode")
    {
        CycleAlphaMode();
        return true;
    }
    if (id == "mat-ed-double-sided")
    {
        ToggleDoubleSided();
        return true;
    }
    if (id == "mat-ed-tex-basecolor-clear")
    {
        return ClearTextureSlot("basecolor");
    }
    if (id == "mat-ed-tex-normal-clear")
    {
        return ClearTextureSlot("normal");
    }
    if (id == "mat-ed-tex-mr-clear")
    {
        return ClearTextureSlot("mr");
    }
    if (id == "mat-ed-tex-emissive-clear")
    {
        return ClearTextureSlot("emissive");
    }
    return false;
}

bool MaterialEditorController::TryDropTexture(
    const std::string& slotElementId, const AssetHandle& texture)
{
    if (slotElementId == "mat-ed-tex-basecolor-drop")
    {
        return SetTextureSlot("basecolor", texture);
    }
    if (slotElementId == "mat-ed-tex-normal-drop")
    {
        return SetTextureSlot("normal", texture);
    }
    if (slotElementId == "mat-ed-tex-mr-drop")
    {
        return SetTextureSlot("mr", texture);
    }
    if (slotElementId == "mat-ed-tex-emissive-drop")
    {
        return SetTextureSlot("emissive", texture);
    }
    return false;
}

bool MaterialEditorController::SetTextureSlot(
    const std::string_view slot, const AssetHandle& texture)
{
    if (!m_Open)
    {
        return false;
    }
    if (slot == "basecolor") m_Editing.BaseColorTexture = texture;
    else if (slot == "normal") m_Editing.NormalTexture = texture;
    else if (slot == "mr") m_Editing.MetallicRoughnessTexture = texture;
    else if (slot == "emissive") m_Editing.EmissiveTexture = texture;
    else return false;
    Save();
    NotifyUi();
    return true;
}

bool MaterialEditorController::ClearTextureSlot(const std::string_view slot)
{
    return SetTextureSlot(slot, InvalidAssetHandle);
}

void MaterialEditorController::SetPreviewShape(const int shape)
{
    m_PreviewShape = shape;
    if (m_PreviewWorld)
    {
        if (auto* mesh = m_PreviewWorld->Registry().try_get<MeshComponent>(m_PreviewMesh))
        {
            mesh->Kind =
                shape == 0 ? MeshKind::Cube : shape == 2 ? MeshKind::Plane : MeshKind::Sphere;
        }
    }
    NotifyUi();
}

std::string MaterialEditorController::TextureSlotDisplayName(const std::string_view slot) const
{
    AssetHandle handle = InvalidAssetHandle;
    if (slot == "basecolor") handle = m_Editing.BaseColorTexture;
    else if (slot == "normal") handle = m_Editing.NormalTexture;
    else if (slot == "mr") handle = m_Editing.MetallicRoughnessTexture;
    else if (slot == "emissive") handle = m_Editing.EmissiveTexture;
    if (handle == InvalidAssetHandle)
    {
        return "None";
    }
    if (m_Assets != nullptr)
    {
        if (const AssetMetadata* meta = m_Assets->Meta(handle))
        {
            return meta->SourcePath.filename().string();
        }
    }
    return "Texture";
}

void MaterialEditorController::Save()
{
    if (m_AssetDb == nullptr || !m_Current)
    {
        return;
    }
    m_Editing.Handle = *m_Current;
    static_cast<void>(m_AssetDb->SaveMaterial(m_Editing));
}

void MaterialEditorController::RenderPreview(
    const std::uint32_t width, const std::uint32_t height)
{
    if (!m_Open || !m_Preview || !m_PreviewWorld || width < 8 || height < 8)
    {
        return;
    }
    if (width != m_PreviewWidth || height != m_PreviewHeight)
    {
        m_Preview->Resize(rhi::Extent2D{width, height});
        m_PreviewWidth = width;
        m_PreviewHeight = height;
    }

    m_Orbit += 0.01F;
    const float radius = 2.4F;
    const glm::vec3 center{0.0F, 0.0F, 0.0F};
    const glm::vec3 eye =
        center + glm::vec3{std::cos(m_Orbit) * radius, 1.05F, std::sin(m_Orbit) * radius};
    const glm::mat4 view = glm::lookAt(eye, center, glm::vec3{0.0F, 1.0F, 0.0F});
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const glm::mat4 projection = glm::perspective(glm::radians(40.0F), aspect, 0.05F, 50.0F);
    m_Preview->SetCamera(view, projection);
    m_Preview->DrawWorld(*m_PreviewWorld);
}

SceneViewSource MaterialEditorController::PreviewSource() const
{
    SceneViewSource source;
    if (m_Open && m_Preview && m_PreviewWidth > 0 && m_PreviewHeight > 0)
    {
        if (rhi::Texture* color = m_Preview->ColorTarget())
        {
            source.Texture = GetNativeTextureHandle(*color);
            source.Width = m_PreviewWidth;
            source.Height = m_PreviewHeight;
        }
    }
    return source;
}
}
