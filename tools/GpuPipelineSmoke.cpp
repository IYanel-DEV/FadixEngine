// Real GPU-side smoke for post-processing pipeline creation. The shader-compile
// smoke proves HLSL compiles; this one proves the SDL_GPU backend can actually
// build the pipeline objects (the step that failed with 0x80070057).
//
// It opens a hidden SDL window + a real SDL_GPU device, initialises the
// PostProcessor, creates every mandatory pipeline, creates the optional pipelines
// and reports each unsupported one individually, resizes to 64x64, and runs the
// minimum tone-map/copy path. It exits non-zero if any mandatory pipeline
// (or SDL_CreateGPUGraphicsPipeline) fails.

#include "render/PostProcessing.h"
#include "render/ShaderCompiler.hpp"

#include "assets/GltfMeshCache.hpp"

#include "engine/rhi/CommandList.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/rhi/Pipeline.hpp"
#include "engine/rhi/RenderTarget.hpp"
#include "engine/rhi/Sampler.hpp"
#include "engine/rhi/Shader.hpp"
#include "engine/rhi/Texture.hpp"
#include "engine/rhi/Types.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace fadix
{
// Defined in src/rhi/sdl/SdlRhi.cpp.
std::unique_ptr<rhi::Device> CreateDeviceFromWindow(void* sdlWindow);
}

namespace
{
// Real-backend creation check for the lit mesh pipeline after cascades raised the
// fragment sampler count to 8 (t0-t3 material + t4-t7 four cascades) and enlarged
// the fragment uniform with four light-space matrices. This is exactly the kind of
// increased-count pipeline that previously failed with 0x80070057, so it is worth
// validating headlessly.
int CheckViewportLitPipeline(fadix::rhi::Device& device)
{
    using namespace fadix;
    try
    {
        const std::vector<std::byte> src = render::ReadShaderSource("viewport.hlsl");
        const std::vector<std::byte> vsCode =
            render::CompileShader(src, "VertexMain", "vs_5_1", "viewport.hlsl");
        const std::vector<std::byte> psCode =
            render::CompileShader(src, "FragmentMain", "ps_5_1", "viewport.hlsl");
        auto vs = device.CreateShader({"VertexMain", "viewport_vertex", 0, 3}, vsCode);
        auto ps = device.CreateShader({"FragmentMain", "viewport_fragment_lit", 8, 1}, psCode);
        if (!vs)
        {
            std::fprintf(stderr, "FAIL: viewport VS: %s\n", vs.ErrorMessage().c_str());
            return 1;
        }
        if (!ps)
        {
            std::fprintf(
                stderr, "FAIL: viewport lit PS (8 samplers): %s\n", ps.ErrorMessage().c_str());
            return 1;
        }
        auto vsShader = std::move(vs).Value();
        auto psShader = std::move(ps).Value();

        rhi::PipelineDesc desc;
        desc.DebugName = "SmokeViewportLit";
        desc.ColorFormat = rhi::Format::R16G16B16A16Float;
        desc.ColorFormat1 = rhi::Format::R16G16Float;
        desc.DepthFormat = rhi::Format::D32Float;
        desc.DepthTest = true;
        desc.DepthWrite = true;
        desc.Cull = rhi::CullMode::Back;
        desc.UseVertexInput = true;
        desc.VertexShader = vsShader.get();
        desc.FragmentShader = psShader.get();
        desc.VertexBufferLayouts = {{0, 96}};
        desc.VertexAttributes = {
            {0, 0, rhi::VertexElementFormat::Float3, 0},
            {1, 0, rhi::VertexElementFormat::Float3, 12},
            {2, 0, rhi::VertexElementFormat::Float4, 24},
            {3, 0, rhi::VertexElementFormat::Float2, 40},
            {4, 0, rhi::VertexElementFormat::Float4, 48},
            {5, 0, rhi::VertexElementFormat::Float4, 64},
            {6, 0, rhi::VertexElementFormat::Float4, 80}};
        auto pipe = device.CreatePipeline(desc);
        if (!pipe)
        {
            std::fprintf(
                stderr,
                "FAIL: viewport lit pipeline (8 samplers + 4-cascade uniforms): %s\n",
                pipe.ErrorMessage().c_str());
            return 1;
        }
        std::printf("viewport lit pipeline (8 samplers + 4-cascade uniforms) created OK\n");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: viewport lit pipeline exception: %s\n", error.what());
        return 1;
    }
}

int CheckDefaultTrianglePrimitive(fadix::rhi::Device& device)
{
    using namespace fadix;
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "fadix-default-triangle.gltf";
    {
        std::ofstream output{path, std::ios::trunc};
        output << R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":42,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA"}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":36,"byteLength":6,"target":34963}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    }
    GltfMeshCache cache{device};
    const AssetHandle handle = AssetHandle::Generate();
    const GltfMeshAsset* mesh = cache.Load(handle, path.string(), [](const std::string& message) {
        std::fprintf(stderr, "%s\n", message.c_str());
    });
    std::error_code error;
    std::filesystem::remove(path, error);
    if (mesh == nullptr || !mesh->IsValid() || mesh->Primitives.empty())
    {
        std::fprintf(stderr, "FAIL: glTF omitted primitive.mode was not treated as triangles\n");
        return 1;
    }
    std::printf("glTF omitted primitive.mode loaded as triangles OK\n");
    return 0;
}

int CheckModelFile(fadix::rhi::Device& device, const std::filesystem::path& path)
{
    using namespace fadix;
    GltfMeshCache cache{device};
    const GltfMeshAsset* mesh = cache.Load(
        AssetHandle::Generate(), path.string(), [](const std::string& message) {
            std::fprintf(stderr, "%s\n", message.c_str());
        });
    if (mesh == nullptr || !mesh->IsValid())
    {
        std::fprintf(stderr, "FAIL: model did not load: %s\n", path.string().c_str());
        return 1;
    }
    std::printf("model loaded OK: %zu primitive(s), bounds (%g,%g,%g)-(%g,%g,%g)\n",
        mesh->Primitives.size(),
        mesh->BoundingBoxMin.x,
        mesh->BoundingBoxMin.y,
        mesh->BoundingBoxMin.z,
        mesh->BoundingBoxMax.x,
        mesh->BoundingBoxMax.y,
        mesh->BoundingBoxMax.z);
    return 0;
}
}

int main(const int argc, char** argv)
{
    using namespace fadix;

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("fadix_gpu_pipeline_smoke", 64, 64, SDL_WINDOW_HIDDEN);
    if (window == nullptr)
    {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    std::unique_ptr<rhi::Device> device = CreateDeviceFromWindow(window);
    if (device == nullptr)
    {
        std::fprintf(stderr, "CreateDeviceFromWindow failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    int failures = 0;
    {
        PostProcessor post;
        // Initialize creates the mandatory pipelines (fail -> false) and each
        // optional pipeline individually (failures are logged with the exact
        // pipeline name + SDL error inside MakePipeline).
        const bool coreReady = post.Initialize(*device);
        std::printf("PostProcessor.Initialize -> %s\n", coreReady ? "true" : "false");
        if (!coreReady || !post.CoreTonemapReady())
        {
            std::fprintf(stderr, "FAIL: mandatory tone-map / copy-dither pipelines not created\n");
            ++failures;
        }

        std::printf("optional capabilities:\n");
        std::printf("  bloom : %s\n", post.BloomReady() ? "ready" : "UNSUPPORTED");
        std::printf("  FXAA  : %s\n", post.FxaaReady() ? "ready" : "UNSUPPORTED");
        std::printf("  TAA   : %s\n", post.TaaReady() ? "ready" : "UNSUPPORTED");
        std::printf("  SSAO  : %s\n", post.SsaoReady() ? "ready" : "UNSUPPORTED");

        if (coreReady)
        {
            post.Resize(*device, 64, 64);
            if (post.OutputTexture() == nullptr)
            {
                std::fprintf(stderr, "FAIL: no LDR output texture after Resize(64x64)\n");
                ++failures;
            }
            else
            {
                const rhi::TextureDesc sceneDesc{
                    {64U, 64U},
                    rhi::Format::R16G16B16A16Float,
                    rhi::TextureUsage::RenderTarget,
                    1,
                    "SmokeSceneHDR"};
                auto sceneResult = device->CreateRenderTarget(sceneDesc, nullptr);
                if (!sceneResult)
                {
                    std::fprintf(
                        stderr, "FAIL: scene HDR target: %s\n", sceneResult.ErrorMessage().c_str());
                    ++failures;
                }
                else
                {
                    auto scene = std::move(sceneResult).Value();
                    auto commands = device->CreateCommandList();
                    commands->Begin();
                    PostProcessSettings settings;
                    settings.BloomEnabled = false;
                    settings.ColorGradingEnabled = false;
                    settings.ResolvedAntiAlias = AntiAliasMode::Off; // tone-map + copy/dither only
                    post.Execute(*commands, *scene->ColorTexture(), settings);
                    commands->End();
                    device->Submit(*commands);
                    device->WaitIdle();
                    std::printf("tone-map/copy path recorded %d pass(es)\n", post.LastPassCount());
                    if (post.LastPassCount() <= 0)
                    {
                        std::fprintf(stderr, "FAIL: tone-map/copy path recorded no passes\n");
                        ++failures;
                    }
                }
            }
        }
        post.Shutdown();
    }

    // Lit mesh pipeline with the raised (8) sampler count + 4-cascade uniforms.
    failures += CheckViewportLitPipeline(*device);
    failures += CheckDefaultTrianglePrimitive(*device);
    if (argc > 1)
    {
        failures += CheckModelFile(*device, argv[1]);
    }

    device.reset();
    SDL_DestroyWindow(window);
    SDL_Quit();

    if (failures != 0)
    {
        std::fprintf(stderr, "%d GPU pipeline check(s) failed\n", failures);
        return 1;
    }
    std::printf("All GPU pipeline checks passed\n");
    return 0;
}
