<p align="center">
  <img src="assets/editor/icons/fadix-logo.png" alt="Fadix Engine" width="320">
</p>

<h1 align="center">Fadix Engine</h1>

<p align="center">
  A Windows-first C++20 game engine and editor for building 2D and 3D projects.
</p>

<p align="center">
  <img alt="Version" src="https://img.shields.io/badge/version-0.9.125-2584d8">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Windows-0078d4">
  <img alt="Language" src="https://img.shields.io/badge/C%2B%2B-20-00599c">
  <a href="LICENSE"><img alt="License" src="https://img.shields.io/badge/license-MIT-green"></a>
</p>

> [!IMPORTANT]
> Fadix Engine is an early preview. It is suitable for experimentation and engine development, but it is not yet recommended for production games.

## Overview

Fadix combines a dockable Dear ImGui editor with an SDL GPU renderer, scene editing, scripting, asset management, audio, and 2D/3D physics. Projects are stored outside the engine checkout and begin from an Empty 2D or Empty 3D template.

Current release: **0.9.127**

## Highlights

- Project launcher with recent projects, templates, news, and development history
- Dockable editor workspace with Scene, Game, Hierarchy, Inspector, Content Browser, Output, Material Editor, and FXS Editor panels
- Scene hierarchy, multi-scene workflow, prefabs, undo/redo, autosave, and persistent editor layouts
- Transform gizmos, editor camera navigation, picking, drag-and-drop asset placement, and editor-only debug visualization
- Physically based materials, directional/point/spot lights, cascaded shadows, ambient occlusion, fog, color grading, quality presets, and render debug views
- Jolt Physics for 3D, Box2D for 2D, mesh and primitive colliders, rigid bodies, and character controllers
- FXS/Lua scripting with an integrated code editor; native C++ script validation is available on Windows
- glTF/GLB mesh loading plus optional Blender-assisted conversion for FBX, OBJ, DAE, STL, PLY, and Blend files
- Audio playback through SDL_mixer and RmlUi-powered in-game UI canvases

## Requirements

The currently supported development environment is:

- Windows 10 or Windows 11, 64-bit
- Visual Studio 2022 with **Desktop development with C++** and the Windows SDK
- [CMake](https://cmake.org/) 3.24 or newer installed in its default Windows location
- [Git](https://git-scm.com/) for dependency downloads
- [Python](https://www.python.org/) 3 for generated embedded editor assets
- [Conan](https://conan.io/) 2.x

Install Conan if it is not already available:

```powershell
python -m pip install --user "conan>=2,<3"
```

An internet connection is required for the first build because CMake downloads pinned third-party dependencies. Blender is optional and is only required when importing formats that Fadix converts to GLB.

## Build

Clone or download the repository, open PowerShell in its root directory, and run:

```powershell
.\build.bat 1
```

The script detects the Visual Studio toolchain, prepares Conan dependencies, configures CMake, and builds the Debug editor. The resulting executable is:

```text
bin\Debug\fadix_editor.exe
```

The first build can take a while because dependencies are compiled locally. Later builds reuse the `.build` cache.

### Manual CMake build

From a Visual Studio x64 developer shell:

```powershell
conan profile detect --force
conan install . --output-folder=.build\conan --build=missing `
  -s:h build_type=Debug -s:h compiler.cppstd=20 -s:b compiler.cppstd=20

cmake -S . -B .build\conan-cmake `
  -DCMAKE_TOOLCHAIN_FILE=.build/conan/conan_toolchain.cmake `
  -DFADIX_ENABLE_PHYSICS=ON `
  -DFADIX_ENABLE_LUA=ON

cmake --build .build\conan-cmake --config Debug --target fadix_editor --parallel 8
```

## Getting started

1. Launch `bin\Debug\fadix_editor.exe`.
2. Choose **Empty 3D** or **Empty 2D** in the project launcher.
3. Enter a project name and parent folder, then select **Create Project**.
4. Add entities from the Hierarchy panel and edit their components in the Inspector.
5. Import assets by dragging supported files into the Content Browser.
6. Drag an imported model from the Content Browser into the Scene View to place it.
7. Use **Play**, **Pause**, and **Step** to test the scene.
8. Use **File > Save All** or `Ctrl+Shift+S` before closing the editor.

Fadix project folders contain a `project.fadix` manifest, a `Scenes` directory, and project asset directories. Generated import data and editor caches are kept under the project's `.fadix` directory.

## Editor controls

| Action | Control |
| --- | --- |
| Select tool | `Q` |
| Move tool | `W` |
| Rotate tool | `E` |
| Scale tool | `R` |
| Toggle local/world transform space | `X` |
| Focus selected entity | `F` |
| Delete selected entity | `Delete` |
| Save | `Ctrl+S` |
| Save all | `Ctrl+Shift+S` |
| Undo / redo | `Ctrl+Z` / `Ctrl+Y` |
| Duplicate selected entity | `Ctrl+D` |
| New / open scene | `Ctrl+N` / `Ctrl+O` |

While the Scene View camera is active, use `W`, `A`, `S`, `D`, `Q`, and `E` to fly; hold Shift for faster movement and use the mouse wheel to adjust fly speed.

## Supported asset formats

| Category | Formats |
| --- | --- |
| Native mesh loading | `.glb`, `.gltf` |
| Blender-assisted mesh import | `.fbx`, `.obj`, `.dae`, `.stl`, `.ply`, `.blend` |
| Scenes and prefabs | `.scene`, `.fadixscene`, prefab assets |
| Scripts | FXS/Lua and native C++ source files |
| Materials and UI | Fadix material files, `.rml`, `.rcss` |

## Tests

The repository uses small executable smoke tests instead of a separate unit-test framework. After configuration, build and run the relevant target from `bin\Debug`. For example:

```powershell
cmake --build .build\conan-cmake --config Debug --target fadix_project_smoke --parallel 8
.\bin\Debug\fadix_project_smoke.exe
```

Other smoke targets cover scene persistence, assets, physics collision, scripting, render quality, shadows, shader compilation, and GPU pipeline creation. Some graphics tests require a compatible GPU and display session.

## Repository layout

```text
assets/       Editor resources, shaders, templates, and validation assets
src/assets/   Asset database, import, metadata, and resource loading
src/editor/   Dear ImGui editor, panels, scene tools, and workflows
src/engine/   Shared engine interfaces and systems
src/physics/  Jolt and Box2D integration
src/project/  Project creation, metadata, persistence, and export services
src/render/   Renderer, shadows, post-processing, terrain, and particles
src/rhi/      SDL GPU rendering backend
src/runtime/  ECS world and components
src/scripting/ FXS/Lua and native script runtime
tools/        Asset embedding and smoke-test programs
```

## Known limitations

- Windows is the only actively verified editor platform.
- The engine and project formats may change before version 1.0.
- A clean first build requires network access and can be large because dependencies are built from source.
- Some advanced rendering, networking, animation, packaging, and platform workflows are still experimental.
- There are no official prebuilt releases or compatibility guarantees yet.

Please report bugs with reproduction steps, the graphics adapter, Windows version, build configuration, and relevant Output-panel or console messages.

## Contributing

Contributions are welcome while the project is in preview:

1. Create a branch from the current development branch.
2. Keep changes focused and follow the existing C++20 style.
3. Build `fadix_editor` in Debug.
4. Run the smoke target related to the changed system.
5. Open a pull request explaining the change and how it was verified.

Do not commit build directories, binaries, editor caches, personal projects, generated embedded assets, credentials, or local AI-agent configuration.

## License

Fadix Engine is available under the [MIT License](LICENSE).

Third-party dependencies retain their own licenses. Notices committed under `third_party/licenses` and the dependency repositories are authoritative for those components.
