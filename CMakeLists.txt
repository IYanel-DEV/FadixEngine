cmake_minimum_required(VERSION 3.24)

project(FadixEngine VERSION 0.9.130 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

option(FADIX_ENABLE_PHYSICS "Build Jolt 3D and Box2D 2D physics" ON)
option(FADIX_ENABLE_LUA "Build FXS/Lua scripting" ON)
option(FADIX_PORTABLE_BUILD "Use only assets embedded in fadix_editor" OFF)
option(FADIX_STATIC_MSVC_RUNTIME "Link the Microsoft runtime into Windows binaries" OFF)

if(MSVC)
    if(FADIX_STATIC_MSVC_RUNTIME)
        set(FADIX_MSVC_RUNTIME "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        set(FADIX_MSVC_RUNTIME "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "${FADIX_MSVC_RUNTIME}")
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Platform, input, audio, and the modern D3D12/Vulkan/Metal GPU abstraction.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.10
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL)

# RmlUi's SDL GPU backend uses SDL_image for document images. Keep the first
# milestone lean; more codecs can be enabled by the asset pipeline later.
set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TESTS OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_VENDORED OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JPG OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JXL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL_image
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
    GIT_TAG release-3.4.4
    GIT_SHALLOW TRUE
    GIT_SUBMODULES "")
FetchContent_MakeAvailable(SDL_image)

# Audio decode/mix (SDL3_mixer 3.x MIX_* API). Prefer header-only decoders.
set(SDLMIXER_VENDORED OFF CACHE BOOL "" FORCE)
set(SDLMIXER_TESTS OFF CACHE BOOL "" FORCE)
set(SDLMIXER_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDLMIXER_GME OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MOD OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MIDI OFF CACHE BOOL "" FORCE)
set(SDLMIXER_OPUS OFF CACHE BOOL "" FORCE)
set(SDLMIXER_WAVPACK OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VORBIS_VORBISFILE OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VORBIS_TREMOR OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VORBIS_STB ON CACHE BOOL "" FORCE)
set(SDLMIXER_MP3_MPG123 OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MP3_DRMP3 ON CACHE BOOL "" FORCE)
set(SDLMIXER_FLAC_LIBFLAC OFF CACHE BOOL "" FORCE)
set(SDLMIXER_FLAC_DRFLAC ON CACHE BOOL "" FORCE)
FetchContent_Declare(SDL_mixer
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_mixer.git
    GIT_TAG release-3.2.4
    GIT_SHALLOW TRUE
    GIT_SUBMODULES "")
FetchContent_MakeAvailable(SDL_mixer)

set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
# FreeType, like every other dependency, is fetched from source. No Conan.
if(NOT TARGET freetype)
    FetchContent_Declare(freetype
        GIT_REPOSITORY https://github.com/freetype/freetype.git
        GIT_TAG VER-2-13-3
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(freetype)
    if(NOT TARGET Freetype::Freetype)
        add_library(Freetype::Freetype INTERFACE IMPORTED)
        target_link_libraries(Freetype::Freetype INTERFACE freetype)
    endif()
endif()

# RmlUi is still used for play-mode GameUI canvases; editor chrome is Dear ImGui.
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_SHELL ON CACHE BOOL "" FORCE)
set(RMLUI_BACKEND SDL_GPU CACHE STRING "" FORCE)
set(RMLUI_SDL_VERSION_MAJOR 3 CACHE STRING "" FORCE)
set(RMLUI_FONT_ENGINE freetype CACHE STRING "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(RmlUi
    GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
    GIT_TAG 6.2
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(RmlUi)

# Dear ImGui docking branch pinned for fadix_editor. Core + SDL3/SDL_GPU backends.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.8-docking
    GIT_SHALLOW TRUE)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

# ImGui's bundled DXBC shadercross blobs fail pipeline creation on some legacy
# D3D12 drivers. Let ImGuiLayer provide a runtime-D3DCompile pipeline only for
# DXBC-only D3D12 devices; every other backend keeps the upstream pipeline.
set(FADIX_IMGUI_SDLGPU_BACKEND "${CMAKE_CURRENT_BINARY_DIR}/generated/imgui_impl_sdlgpu3.cpp")
file(READ "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp" FADIX_IMGUI_SDLGPU_SOURCE)
string(REPLACE
    "    ImGui_ImplSDLGPU3_CreateGraphicsPipeline();"
    "    if (strcmp(SDL_GetGPUDeviceDriver(v->Device), \"direct3d12\") != 0 || (SDL_GetGPUShaderFormats(v->Device) & SDL_GPU_SHADERFORMAT_DXIL) != 0)\n        ImGui_ImplSDLGPU3_CreateGraphicsPipeline();"
    FADIX_IMGUI_SDLGPU_SOURCE
    "${FADIX_IMGUI_SDLGPU_SOURCE}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${FADIX_IMGUI_SDLGPU_BACKEND}" "${FADIX_IMGUI_SDLGPU_SOURCE}")

add_library(fadix_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${FADIX_IMGUI_SDLGPU_BACKEND}")
target_include_directories(fadix_imgui PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends")
target_link_libraries(fadix_imgui PUBLIC SDL3::SDL3-static)
if(WIN32)
    target_sources(fadix_imgui PRIVATE
        "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp")
    target_link_libraries(fadix_imgui PUBLIC d3d11 dxgi)
endif()

if(WIN32)
    FetchContent_Declare(scintilla
        GIT_REPOSITORY https://github.com/elcuco/scintilla-code.git
        GIT_TAG rel-5-5-1
        GIT_SHALLOW TRUE)
    FetchContent_GetProperties(scintilla)
    if(NOT scintilla_POPULATED)
        FetchContent_Populate(scintilla)
    endif()

    FetchContent_Declare(lexilla
        GIT_REPOSITORY https://github.com/ScintillaOrg/lexilla.git
        GIT_TAG rel-5-5-1
        GIT_SHALLOW TRUE)
    FetchContent_GetProperties(lexilla)
    if(NOT lexilla_POPULATED)
        FetchContent_Populate(lexilla)
    endif()

    file(GLOB FADIX_SCINTILLA_SRC CONFIGURE_DEPENDS "${scintilla_SOURCE_DIR}/src/*.cxx")
    add_library(fadix_scintilla STATIC
        ${FADIX_SCINTILLA_SRC}
        "${scintilla_SOURCE_DIR}/win32/HanjaDic.cxx"
        "${scintilla_SOURCE_DIR}/win32/PlatWin.cxx"
        "${scintilla_SOURCE_DIR}/win32/ScintillaWin.cxx")
    target_include_directories(fadix_scintilla PUBLIC
        "${scintilla_SOURCE_DIR}/include"
        "${scintilla_SOURCE_DIR}/src")
    target_compile_definitions(fadix_scintilla PUBLIC STATIC_BUILD)
    target_compile_features(fadix_scintilla PUBLIC cxx_std_17)
    target_link_libraries(fadix_scintilla PUBLIC
        imm32 ole32 oleaut32 uuid gdi32 user32 advapi32)
    if(MSVC)
        set_target_properties(fadix_scintilla PROPERTIES
            MSVC_RUNTIME_LIBRARY "${FADIX_MSVC_RUNTIME}")
        target_compile_options(fadix_scintilla PRIVATE /W0 /utf-8)
    endif()

    file(GLOB FADIX_LEXILLA_LEXLIB CONFIGURE_DEPENDS "${lexilla_SOURCE_DIR}/lexlib/*.cxx")
    add_library(fadix_lexilla STATIC
        ${FADIX_LEXILLA_LEXLIB}
        "${lexilla_SOURCE_DIR}/lexers/LexLua.cxx"
        "${lexilla_SOURCE_DIR}/lexers/LexCPP.cxx"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/editor/scripting/FadixLexilla.cpp")
    target_include_directories(fadix_lexilla PUBLIC
        "${lexilla_SOURCE_DIR}/include"
        "${lexilla_SOURCE_DIR}/lexlib"
        "${scintilla_SOURCE_DIR}/include")
    target_compile_features(fadix_lexilla PUBLIC cxx_std_17)
    target_link_libraries(fadix_lexilla PUBLIC fadix_scintilla)
    if(MSVC)
        set_target_properties(fadix_lexilla PROPERTIES
            MSVC_RUNTIME_LIBRARY "${FADIX_MSVC_RUNTIME}")
        target_compile_options(fadix_lexilla PRIVATE /W0 /utf-8)
    endif()

    add_library(fadix_fxs_editor STATIC
        src/editor/scripting/ScriptEditorController.cpp
        src/editor/scripting/FxsCodeEditor.cpp
        src/editor/scripting/FxsCodeEditorAutocomplete.cpp
        src/editor/scripting/FxsEditorSettings.cpp
        src/editor/scripting/FxsTextSearch.cpp
        src/editor/scripting/FxsApiCatalog.cpp
        src/editor/scripting/FxsDiagnostics.cpp
        src/editor/scripting/FxsProjectSearch.cpp
        src/editor/scripting/FxsEditorSession.cpp)
    target_include_directories(fadix_fxs_editor PUBLIC src)
    target_link_libraries(fadix_fxs_editor PUBLIC fadix_scintilla fadix_lexilla)
    target_link_libraries(fadix_fxs_editor PRIVATE Comctl32 shell32 ole32)
    if(MSVC)
        set_target_properties(fadix_fxs_editor PROPERTIES
            MSVC_RUNTIME_LIBRARY "${FADIX_MSVC_RUNTIME}")
    endif()
endif()

set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_GL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_VALIDATOR_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_BUILD_BUILDER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE BOOL "" FORCE)
set(TINYGLTF_NO_STB_IMAGE_WRITE ON CACHE BOOL "" FORCE)
FetchContent_Declare(tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG v2.9.5
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(tinygltf)
if(MSVC AND TARGET tinygltf)
    set_target_properties(tinygltf PROPERTIES
        MSVC_RUNTIME_LIBRARY "${FADIX_MSVC_RUNTIME}")
endif()

FetchContent_Declare(EnTT
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.16.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(EnTT)

FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glm)

if(FADIX_ENABLE_LUA)
    FetchContent_Declare(lua
        URL https://www.lua.org/ftp/lua-5.4.7.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_GetProperties(lua)
    if(NOT lua_POPULATED)
        FetchContent_Populate(lua)
    endif()
    file(GLOB LUA_SOURCES CONFIGURE_DEPENDS "${lua_SOURCE_DIR}/src/*.c")
    list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac|onelua)\\.c$")
    add_library(lua_static STATIC ${LUA_SOURCES})
    target_include_directories(lua_static PUBLIC "${lua_SOURCE_DIR}/src")
    if(MSVC)
        set_target_properties(lua_static PROPERTIES
            MSVC_RUNTIME_LIBRARY "${FADIX_MSVC_RUNTIME}")
    endif()

    FetchContent_Declare(sol2
        GIT_REPOSITORY https://github.com/ThePhD/sol2.git
        GIT_TAG v3.3.0
        GIT_SHALLOW TRUE)
    FetchContent_GetProperties(sol2)
    if(NOT sol2_POPULATED)
        FetchContent_Populate(sol2)
    endif()

    add_library(fadix_lua INTERFACE)
    target_include_directories(fadix_lua INTERFACE "${sol2_SOURCE_DIR}/include")
    target_link_libraries(fadix_lua INTERFACE lua_static)
    target_compile_definitions(fadix_lua INTERFACE FADIX_ENABLE_LUA=1)
endif()

add_library(fadix_runtime STATIC
    src/runtime/World.cpp
    src/runtime/World.hpp
    src/runtime/Components.hpp
    src/network/ReplicationRegistry.cpp
    src/network/ReplicationRegistry.hpp)
target_include_directories(fadix_runtime PUBLIC src)
target_link_libraries(fadix_runtime PUBLIC EnTT::EnTT glm::glm)

if(FADIX_ENABLE_PHYSICS)
    set(JPH_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
    set(JPH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(JPH_BUILD_VIEWER OFF CACHE BOOL "" FORCE)
    set(JPH_BUILD_HELLO_WORLD OFF CACHE BOOL "" FORCE)
    set(JPH_USE_AVX2 OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY ${FADIX_STATIC_MSVC_RUNTIME} CACHE BOOL "" FORCE)
    FetchContent_Declare(JoltPhysics
        GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
        GIT_TAG v5.5.0
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR Build)
    FetchContent_MakeAvailable(JoltPhysics)

    set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
    set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(BOX2D_DOCS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(box2d
        GIT_REPOSITORY https://github.com/erincatto/box2d.git
        GIT_TAG v3.1.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(box2d)
    if(MSVC)
        set_property(TARGET box2d PROPERTY MSVC_RUNTIME_LIBRARY
            "${FADIX_MSVC_RUNTIME}")
    endif()

    target_sources(fadix_runtime PRIVATE
        src/physics/PhysicsWorld.cpp
        src/physics/PhysicsWorld.hpp)
    target_link_libraries(fadix_runtime PUBLIC Jolt box2d)
    target_compile_definitions(fadix_runtime PUBLIC FADIX_ENABLE_PHYSICS=1)
endif()

file(GLOB_RECURSE FADIX_IMGUI_EDITOR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/editor/imgui/*.cpp")

# Shared engine/project code for the ImGui editor (no RmlUi chrome adapters).
file(GLOB_RECURSE FADIX_IMGUI_SESSION_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/project/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rhi/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/render/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/editor/scene/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/editor/command/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/editor/play/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/editor/assets/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/editor/camera/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/editor/gizmo/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/editor/scripting/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/editor/material/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/scripting/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/engine/audio/*.cpp")
list(FILTER FADIX_IMGUI_SESSION_SOURCES EXCLUDE REGEX "MaterialEditorRmlAdapter\\.cpp$")
list(FILTER FADIX_IMGUI_SESSION_SOURCES EXCLUDE REGEX "ScriptEditorRmlAdapter\\.cpp$")
list(FILTER FADIX_IMGUI_SESSION_SOURCES EXCLUDE REGEX "_test\\.cpp$")
list(FILTER FADIX_IMGUI_SESSION_SOURCES EXCLUDE REGEX "FxsCodeEditor|FxsEditorSettings|FxsTextSearch|FxsApiCatalog|FxsDiagnostics|FxsProjectSearch|FxsEditorSession|FxsClangdCompileFlags|FxsClangdClient|FadixLexilla|ScriptEditorController")
list(APPEND FADIX_IMGUI_SESSION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/editor/ui/GameUIOverlay.cpp")

set(FADIX_EMBEDDED_ASSETS_HPP "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/EmbeddedAssets.hpp")
file(GLOB_RECURSE FADIX_EMBEDDED_ASSET_INPUTS CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/editor/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/templates/*")
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_custom_command(
        OUTPUT "${FADIX_EMBEDDED_ASSETS_HPP}"
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_assets.py"
            --asset-root "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            --output "${FADIX_EMBEDDED_ASSETS_HPP}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_assets.py"
            ${FADIX_EMBEDDED_ASSET_INPUTS}
        COMMENT "Embedding editor runtime assets"
        VERBATIM)
else()
    if(NOT EXISTS "${FADIX_EMBEDDED_ASSETS_HPP}")
        message(FATAL_ERROR "EmbeddedAssets.hpp missing and Python3 not found to regenerate it")
    endif()
endif()
add_custom_target(fadix_embed_assets DEPENDS "${FADIX_EMBEDDED_ASSETS_HPP}")

add_executable(fadix_portable_assets_smoke
    tools/PortableAssetsSmoke.cpp
    src/assets/EmbeddedAssetProvider.cpp)
target_include_directories(fadix_portable_assets_smoke PRIVATE src)
target_compile_definitions(fadix_portable_assets_smoke PRIVATE
    FADIX_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets"
    FADIX_PORTABLE_BUILD=1)
add_dependencies(fadix_portable_assets_smoke fadix_embed_assets)
if(MSVC)
    target_compile_options(fadix_portable_assets_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(fadix_portable_assets_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(fadix_dock_smoke
    tools/DockLayoutSmoke.cpp
    src/editor/dock/DockLayout.cpp
    src/project/ProjectJson.cpp)
target_include_directories(fadix_dock_smoke PRIVATE src)

add_executable(fadix_editor
    src/main_imgui.cpp
    src/editor/EditorSession.cpp
    src/editor/EditorSession.hpp
    src/editor/EditorLog.cpp
    src/editor/EditorLog.hpp
    src/engine/Uuid.cpp
    src/engine/command/UndoStack.cpp
    src/engine/app/ModuleFactories.cpp
    ${FADIX_IMGUI_EDITOR_SOURCES}
    ${FADIX_IMGUI_SESSION_SOURCES})
add_executable(fadix ALIAS fadix_editor)
add_dependencies(fadix_editor fadix_dock_smoke)
add_dependencies(fadix_editor fadix_portable_assets_smoke)
target_include_directories(fadix_editor PRIVATE src)
target_link_libraries(fadix_editor PRIVATE
    fadix_runtime
    fadix_imgui
    RmlUi::RmlUi
    rmlui_backend_SDL_GPU
    SDL3::SDL3-static
    SDL3_image::SDL3_image
    SDL3_mixer::SDL3_mixer
    tinygltf
    glm::glm)
if(FADIX_ENABLE_LUA)
    target_link_libraries(fadix_editor PRIVATE fadix_lua)
endif()
if(WIN32)
    target_link_libraries(fadix_editor PRIVATE
        fadix_fxs_editor d3d11 dxgi ole32 shell32 uuid)
endif()
target_compile_definitions(fadix_editor PRIVATE
    FADIX_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets")
if(FADIX_PORTABLE_BUILD)
    target_compile_definitions(fadix_editor PRIVATE FADIX_PORTABLE_BUILD=1)
else()
    target_compile_definitions(fadix_editor PRIVATE FADIX_SRC_DIR="${CMAKE_SOURCE_DIR}/src")
endif()

# -----------------------------------------------------------------------------
# fadix_player — the standalone runtime that ships with an exported game.
# Reuses the exact shared session sources as the editor (render, rhi, scene,
# play, scripting, assets, audio, project) but drops all editor/imgui chrome.
# A stranger runs an exported game with fadix_player.exe + assets, no IDE.
# -----------------------------------------------------------------------------
add_executable(fadix_player
    src/main_player.cpp
    src/player/PlayerApplication.cpp
    src/player/PlayerApplication.hpp
    src/engine/Uuid.cpp
    src/engine/command/UndoStack.cpp
    src/engine/app/ModuleFactories.cpp
    ${FADIX_IMGUI_SESSION_SOURCES})
target_include_directories(fadix_player PRIVATE src)
target_link_libraries(fadix_player PRIVATE
    fadix_runtime
    RmlUi::RmlUi
    rmlui_backend_SDL_GPU
    SDL3::SDL3-static
    SDL3_image::SDL3_image
    SDL3_mixer::SDL3_mixer
    tinygltf
    glm::glm)
if(FADIX_ENABLE_LUA)
    target_link_libraries(fadix_player PRIVATE fadix_lua)
endif()
if(WIN32)
    target_link_libraries(fadix_player PRIVATE d3d11 dxgi ole32 shell32 uuid)
endif()
target_compile_definitions(fadix_player PRIVATE
    FADIX_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets")
if(FADIX_PORTABLE_BUILD)
    target_compile_definitions(fadix_player PRIVATE FADIX_PORTABLE_BUILD=1)
else()
    target_compile_definitions(fadix_player PRIVATE FADIX_SRC_DIR="${CMAKE_SOURCE_DIR}/src")
endif()
if(MSVC)
    target_compile_options(fadix_player PRIVATE /W4 /permissive-)
else()
    target_compile_options(fadix_player PRIVATE -Wall -Wextra)
endif()

add_executable(fadix_project_smoke
    tools/ProjectServiceSmoke.cpp
    src/assets/EmbeddedAssetProvider.cpp
    src/project/ProjectService.cpp
    src/project/ProjectJson.cpp
    src/engine/Uuid.cpp)
target_include_directories(fadix_project_smoke PRIVATE src)
target_compile_definitions(fadix_project_smoke PRIVATE
    FADIX_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets")
add_dependencies(fadix_project_smoke fadix_embed_assets)
if(WIN32)
    target_link_libraries(fadix_project_smoke PRIVATE ole32 shell32 uuid)
endif()

add_executable(fadix_export_smoke
    tools/ExportSmoke.cpp
    src/assets/EmbeddedAssetProvider.cpp
    src/project/ExportService.cpp
    src/project/ProjectService.cpp
    src/project/ProjectJson.cpp
    src/engine/Uuid.cpp)
target_include_directories(fadix_export_smoke PRIVATE src)
target_compile_definitions(fadix_export_smoke PRIVATE
    FADIX_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets")
add_dependencies(fadix_export_smoke fadix_embed_assets)
if(WIN32)
    target_link_libraries(fadix_export_smoke PRIVATE ole32 shell32 uuid)
endif()
if(MSVC)
    target_compile_options(fadix_export_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(fadix_export_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(FADIX_ENABLE_LUA)
    add_executable(fadix_lua_smoke
        tools/LuaSmoke.cpp
        src/scripting/LuaVM.cpp
        src/engine/audio/AudioEngine.cpp)
    target_include_directories(fadix_lua_smoke PRIVATE src)
    target_link_libraries(fadix_lua_smoke PRIVATE
        fadix_lua SDL3::SDL3-static SDL3_mixer::SDL3_mixer EnTT::EnTT glm::glm)

    add_executable(fadix_script_runtime_smoke
        tools/ScriptRuntimeSmoke.cpp
        src/scripting/ScriptRunner.cpp
        src/scripting/LuaVM.cpp
        src/scripting/NativeScriptLoader.cpp
        src/engine/audio/AudioEngine.cpp)
    target_include_directories(fadix_script_runtime_smoke PRIVATE src)
    target_link_libraries(fadix_script_runtime_smoke PRIVATE
        fadix_lua fadix_runtime SDL3::SDL3-static SDL3_mixer::SDL3_mixer)
endif()

# Headless smoke tests for scene safety (Subtask 1) and physics colliders
# (Subtask 3). Reuses fadix_runtime for World + Jolt + Box2D + EnTT + glm.
add_executable(fadix_engine_smoke
    tools/EngineSmoke.cpp
    src/assets/MaterialAssetJson.cpp
    src/assets/ScriptAsset.cpp
    src/assets/ScriptDatabase.cpp
    src/assets/TextureProcessor.cpp
    src/editor/command/EntityCommands.cpp
    src/editor/scene/EntityTextIO.cpp
    src/editor/scene/SceneSerializer.cpp
    src/engine/Uuid.cpp
    src/project/ProjectJson.cpp)
target_include_directories(fadix_engine_smoke PRIVATE src)
target_link_libraries(fadix_engine_smoke PRIVATE
    fadix_runtime
    SDL3::SDL3-static
    SDL3_image::SDL3_image
    EnTT::EnTT
    glm::glm)
if(WIN32)
    target_link_libraries(fadix_engine_smoke PRIVATE ole32 shell32 uuid)
endif()

add_executable(fadix_animation_smoke tools/AnimationSmoke.cpp)
target_include_directories(fadix_animation_smoke PRIVATE src)
target_link_libraries(fadix_animation_smoke PRIVATE glm::glm)
if(MSVC)
    target_compile_options(fadix_animation_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(fadix_animation_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_dependencies(fadix_editor fadix_animation_smoke)

# Forward+ tile light-culling stress + correctness check (no GPU needed).
add_executable(fadix_light_stress_smoke
    tools/LightStressSmoke.cpp
    src/render/ForwardPlusLights.cpp)
target_include_directories(fadix_light_stress_smoke PRIVATE src)
target_link_libraries(fadix_light_stress_smoke PRIVATE glm::glm)
if(MSVC)
    target_compile_options(fadix_light_stress_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(fadix_light_stress_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_dependencies(fadix_editor fadix_light_stress_smoke)

add_executable(fadix_scene_persistence_smoke
    tools/ScenePersistenceSmoke.cpp
    src/editor/scene/EntityTextIO.cpp
    src/editor/scene/SceneSerializer.cpp
    src/engine/Uuid.cpp)
target_include_directories(fadix_scene_persistence_smoke PRIVATE src)
target_link_libraries(fadix_scene_persistence_smoke PRIVATE fadix_runtime)
if(WIN32)
    target_link_libraries(fadix_scene_persistence_smoke PRIVATE ole32 shell32 uuid)
endif()
if(MSVC)
    target_compile_options(fadix_scene_persistence_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(fadix_scene_persistence_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_dependencies(fadix_editor fadix_scene_persistence_smoke)

add_executable(fadix_asset_browser_smoke
    tools/AssetBrowserSmoke.cpp
    src/assets/AssetDatabase.cpp
    src/assets/MaterialAssetJson.cpp
    src/assets/TextureProcessor.cpp
    src/editor/assets/AssetBrowserController.cpp
    src/engine/Uuid.cpp
    src/project/ProjectJson.cpp)
target_include_directories(fadix_asset_browser_smoke PRIVATE src)
target_link_libraries(fadix_asset_browser_smoke PRIVATE
    SDL3::SDL3-static SDL3_image::SDL3_image glm::glm)
if(MSVC)
    target_compile_options(fadix_asset_browser_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(fadix_asset_browser_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_dependencies(fadix_editor fadix_asset_browser_smoke)

if(FADIX_ENABLE_PHYSICS)
    add_executable(fadix_collision_smoke
        tools/CollisionSmoke.cpp
        src/engine/Uuid.cpp)
    target_include_directories(fadix_collision_smoke PRIVATE src)
    target_link_libraries(fadix_collision_smoke PRIVATE fadix_runtime)
    if(MSVC)
        target_compile_options(fadix_collision_smoke PRIVATE /W4 /permissive-)
    else()
        target_compile_options(fadix_collision_smoke PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_dependencies(fadix_editor fadix_collision_smoke)
endif()

add_executable(fadix_quality_smoke
    tools/QualitySmoke.cpp
    src/render/RenderQuality.cpp)
target_include_directories(fadix_quality_smoke PRIVATE src)
if(MSVC)
    target_compile_options(fadix_quality_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(fadix_quality_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_dependencies(fadix_editor fadix_quality_smoke)

# Pure cascaded-shadow math + BuildCascades integration (split scheme, texel
# snapping, TAA-jitter stability, active-count). No GPU/window.
add_executable(fadix_shadow_smoke
    tools/ShadowMathSmoke.cpp
    src/render/ShadowCascades.cpp)
target_include_directories(fadix_shadow_smoke PRIVATE src)
target_link_libraries(fadix_shadow_smoke PRIVATE glm::glm)
if(MSVC)
    target_compile_options(fadix_shadow_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(fadix_shadow_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_dependencies(fadix_editor fadix_shadow_smoke)

add_executable(fadix_graphics_prefs_smoke
    tools/GraphicsPrefsSmoke.cpp
    src/editor/imgui/GraphicsPreferences.cpp
    src/render/RenderQuality.cpp
    src/project/ProjectJson.cpp)
target_include_directories(fadix_graphics_prefs_smoke PRIVATE src)
if(MSVC)
    target_compile_options(fadix_graphics_prefs_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(fadix_graphics_prefs_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    add_executable(fadix_shader_compile_smoke
        tools/ShaderCompileSmoke.cpp
        src/assets/EmbeddedAssetProvider.cpp
        src/render/ShaderCompiler.cpp)
    target_include_directories(fadix_shader_compile_smoke PRIVATE src)
    target_compile_definitions(fadix_shader_compile_smoke PRIVATE
        FADIX_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets")
    if(MSVC)
        target_compile_options(fadix_shader_compile_smoke PRIVATE /W4 /permissive-)
    endif()
    add_dependencies(fadix_editor fadix_shader_compile_smoke)
    add_dependencies(fadix_shader_compile_smoke fadix_embed_assets)

    # Real GPU pipeline smoke: builds an actual SDL_GPU device and creates every
    # post-processing pipeline. Catches SDL_CreateGPUGraphicsPipeline failures
    # (0x80070057) that the compile-only smoke cannot see.
    add_executable(fadix_gpu_pipeline_smoke
        tools/GpuPipelineSmoke.cpp
        src/assets/EmbeddedAssetProvider.cpp
        src/assets/GltfMeshCache.cpp
        src/engine/Uuid.cpp
        src/render/PostProcessing.cpp
        src/render/ShaderCompiler.cpp
        src/rhi/d3d11/D3D11Rhi.cpp
        src/rhi/sdl/SdlRhi.cpp)
    target_include_directories(fadix_gpu_pipeline_smoke PRIVATE src)
    target_link_libraries(fadix_gpu_pipeline_smoke PRIVATE
        SDL3::SDL3-static glm::glm tinygltf d3d11 dxgi)
    target_compile_definitions(fadix_gpu_pipeline_smoke PRIVATE
        FADIX_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets")
    if(MSVC)
        target_compile_options(fadix_gpu_pipeline_smoke PRIVATE /W4 /permissive-)
    endif()
    add_dependencies(fadix_editor fadix_gpu_pipeline_smoke)
    add_dependencies(fadix_gpu_pipeline_smoke fadix_embed_assets)
endif()

if(MSVC)
    target_compile_options(fadix_runtime PRIVATE /W4 /permissive-)
    target_compile_options(fadix_imgui PRIVATE /W4 /permissive-)
    target_compile_options(fadix_dock_smoke PRIVATE /W4 /permissive-)
    target_compile_options(fadix_editor PRIVATE /W4 /permissive-)
    target_compile_options(fadix_project_smoke PRIVATE /W4 /permissive-)
    target_compile_options(fadix_engine_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(fadix_runtime PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(fadix_imgui PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(fadix_dock_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(fadix_editor PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(fadix_project_smoke PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(fadix_engine_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

configure_file(
    "${rmlui_SOURCE_DIR}/Samples/assets/LatoLatin-Regular.ttf"
    "${CMAKE_SOURCE_DIR}/assets/fonts/LatoLatin-Regular.ttf"
    COPYONLY)
