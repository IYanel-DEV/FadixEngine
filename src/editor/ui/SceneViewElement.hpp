#pragma once

#include "editor/ui/SceneViewSource.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <functional>

namespace fadix::editor
{
// Custom RmlUi element (<sceneview>) that draws the offscreen scene render
// target as part of the normal document paint, so the viewport composes
// correctly with dock backgrounds, overlays, tooltips and clipping instead of
// being blitted underneath the UI.
class SceneViewElement final : public Rml::Element
{
public:
    explicit SceneViewElement(const Rml::String& tag);
    ~SceneViewElement() override;

    // Registers the <sceneview> tag with the RmlUi factory. Call once after
    // Rml::Initialise and before loading documents.
    static void RegisterInstancer();

    // The provider is queried every frame during rendering, once per <sceneview>
    // element, with that element's id (so the main viewport and the material
    // preview can supply different textures). Return a null texture to draw
    // nothing (e.g. outside the workbench, or an unknown element).
    static void SetSourceProvider(std::function<SceneViewSource(const Rml::String& elementId)> provider);

protected:
    void OnRender() override;

private:
    void ReleaseGeometry();

    Rml::CompiledGeometryHandle m_Geometry{0};
    Rml::Vector2f m_CompiledSize{0.0F, 0.0F};
};
}
