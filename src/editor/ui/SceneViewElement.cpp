#include "editor/ui/SceneViewElement.hpp"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Vertex.h>

#include <array>

namespace fadix::editor
{
namespace
{
std::function<SceneViewSource(const Rml::String&)> g_SourceProvider;

Rml::ElementInstancerGeneric<SceneViewElement>& Instancer()
{
    static Rml::ElementInstancerGeneric<SceneViewElement> instancer;
    return instancer;
}
}

SceneViewElement::SceneViewElement(const Rml::String& tag) : Rml::Element(tag) {}

SceneViewElement::~SceneViewElement()
{
    ReleaseGeometry();
}

void SceneViewElement::RegisterInstancer()
{
    Rml::Factory::RegisterElementInstancer("sceneview", &Instancer());
}

void SceneViewElement::SetSourceProvider(
    std::function<SceneViewSource(const Rml::String&)> provider)
{
    g_SourceProvider = std::move(provider);
}

void SceneViewElement::ReleaseGeometry()
{
    if (m_Geometry != 0)
    {
        if (Rml::RenderInterface* render = Rml::GetRenderInterface())
        {
            render->ReleaseGeometry(m_Geometry);
        }
        m_Geometry = 0;
    }
}

void SceneViewElement::OnRender()
{
    if (!g_SourceProvider)
    {
        return;
    }
    const SceneViewSource source = g_SourceProvider(GetId());
    if (source.Texture == nullptr || source.Width == 0 || source.Height == 0)
    {
        return;
    }
    Rml::RenderInterface* render = Rml::GetRenderInterface();
    if (render == nullptr)
    {
        return;
    }

    const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    if (size.x < 1.0F || size.y < 1.0F)
    {
        return;
    }
    if (m_Geometry == 0 || size != m_CompiledSize)
    {
        ReleaseGeometry();
        // Quad in element-local space; the texture is opaque so premultiplied
        // white keeps the UI blend state exact.
        std::array<Rml::Vertex, 4> vertices{};
        const Rml::ColourbPremultiplied white{255, 255, 255, 255};
        vertices[0].position = {0.0F, 0.0F};
        vertices[0].tex_coord = {0.0F, 0.0F};
        vertices[1].position = {size.x, 0.0F};
        vertices[1].tex_coord = {1.0F, 0.0F};
        vertices[2].position = {size.x, size.y};
        vertices[2].tex_coord = {1.0F, 1.0F};
        vertices[3].position = {0.0F, size.y};
        vertices[3].tex_coord = {0.0F, 1.0F};
        for (Rml::Vertex& vertex : vertices)
        {
            vertex.colour = white;
        }
        const std::array<int, 6> indices{0, 2, 1, 0, 3, 2};
        m_Geometry = render->CompileGeometry(
            {vertices.data(), vertices.size()}, {indices.data(), indices.size()});
        if (m_Geometry == 0)
        {
            Rml::Log::Message(
                Rml::Log::LT_ERROR, "sceneview: could not compile viewport quad geometry");
            return;
        }
        m_CompiledSize = size;
    }

    const Rml::Vector2f offset = GetAbsoluteOffset(Rml::BoxArea::Content);
    render->RenderGeometry(
        m_Geometry, offset, reinterpret_cast<Rml::TextureHandle>(source.Texture));
}
}
