@echo off
setlocal EnableExtensions EnableDelayedExpansion
set PATH=C:\Program Files\Python310\;C:\Program Files\Python310\Scripts\;%PATH%

rem =============================================================================
rem Fadix Engine build script.
rem
rem Dependency rule (ONE rule, no exceptions): every third-party library is
rem pulled by CMake FetchContent. There is no Conan step. FreeType, SDL, Jolt,
rem Box2D, RmlUi, ImGui, glm, EnTT, tinygltf and Lua are all fetched and built
rem from source, pinned to the tags in CMakeLists.txt. This keeps a single,
rem reproducible toolchain and avoids the Conan/FetchContent hybrid that used to
rem break FreeType (its Meson build grabbed C:\Windows\python3.exe and died).
rem =============================================================================

if "%~1"=="" goto :build_debug_editor
if /I "%~1"=="editor" goto :build_debug_editor
if /I "%~1"=="player" goto :build_debug_player
if "%~1"=="1" goto :build_debug_all
if "%~1"=="2" goto :build_release
echo Usage: build.bat [editor^|player^|1^|2]
echo   editor - build only fadix_editor Debug (default)
echo   player - build only fadix_player Debug
echo   1      - build fadix_editor + fadix_player Debug
echo   2      - build the portable fadix_editor + fadix_player Release executables
echo.
echo Low-spec defaults: 2 Debug jobs, 1 Release job, and low process priority.
echo Override with FADIX_BUILD_JOBS or FADIX_RELEASE_JOBS if needed.
exit /b 2

:build_debug_editor
set "FADIX_DEBUG_TARGETS=fadix_editor"
goto :build_debug

:build_debug_player
set "FADIX_DEBUG_TARGETS=fadix_player"
goto :build_debug

:build_debug_all
set "FADIX_DEBUG_TARGETS=fadix_editor fadix_player"
goto :build_debug

rem -----------------------------------------------------------------------------
rem Ensure the MSVC x64 toolchain is on PATH (idempotent).
rem -----------------------------------------------------------------------------
:ensure_msvc
where cl.exe >nul 2>nul
if not errorlevel 1 exit /b 0
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo [Fadix] Visual Studio Build Tools were not found.
    exit /b 1
)
for /f "usebackq tokens=*" %%I in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT (
    echo [Fadix] MSVC x64 tools were not found.
    exit /b 1
)
call "!VSROOT!\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
exit /b %errorlevel%

:build_debug
call :ensure_msvc
if errorlevel 1 exit /b %errorlevel%

rem Codex-hosted shells can expose both Path and PATH. MSBuild treats those as
rem duplicate dictionary keys. Collapse to one PATH entry; do not wipe it.
set "FADIX_PATH=%PATH%"
set "Path="
set "PATH=%FADIX_PATH%"

set "CMAKE_EXE=C:\Program Files\CMake\bin\cmake.exe"
if not defined FADIX_BUILD_JOBS set "FADIX_BUILD_JOBS=2"
if not exist ".build\debug-cmake\CMakeCache.txt" (
    "%CMAKE_EXE%" -S . -B .build\debug-cmake -DFADIX_ENABLE_PHYSICS=ON -DFADIX_PORTABLE_BUILD=OFF -DFADIX_STATIC_MSVC_RUNTIME=OFF
    if errorlevel 1 exit /b !errorlevel!
) else (
    echo [Fadix] Reusing .build\debug-cmake
)
echo [Fadix] Building %FADIX_DEBUG_TARGETS% with %FADIX_BUILD_JOBS% low-priority job^(s^)
"%CMAKE_EXE%" --build .build\debug-cmake --config Debug --target %FADIX_DEBUG_TARGETS% --parallel %FADIX_BUILD_JOBS% -- -lowPriority
exit /b %errorlevel%

:build_release
call :ensure_msvc
if errorlevel 1 exit /b %errorlevel%

set "FADIX_PATH=%PATH%"
set "Path="
set "PATH=%FADIX_PATH%"

set "CMAKE_EXE=C:\Program Files\CMake\bin\cmake.exe"
if not defined FADIX_RELEASE_JOBS set "FADIX_RELEASE_JOBS=1"
if not exist ".build\release-cmake\CMakeCache.txt" (
    "%CMAKE_EXE%" -S . -B .build\release-cmake -DFADIX_ENABLE_PHYSICS=ON -DFADIX_PORTABLE_BUILD=ON -DFADIX_STATIC_MSVC_RUNTIME=ON
    if errorlevel 1 exit /b !errorlevel!
) else (
    echo [Fadix] Reusing .build\release-cmake
)
echo [Fadix] Building Release with %FADIX_RELEASE_JOBS% low-priority job^(s^)
"%CMAKE_EXE%" --build .build\release-cmake --config Release --target fadix_editor fadix_player --parallel %FADIX_RELEASE_JOBS% -- -lowPriority
if errorlevel 1 exit /b %errorlevel%

if not exist artifacts mkdir artifacts
copy /Y "bin\Release\fadix_editor.exe" "artifacts\FadixEngine-0.9.136-Windows-x64.exe" >nul
if errorlevel 1 exit /b %errorlevel%
copy /Y "bin\Release\fadix_player.exe" "artifacts\FadixPlayer-0.9.136-Windows-x64.exe" >nul
if errorlevel 1 exit /b %errorlevel%
echo [Fadix] Portable release editor:  artifacts\FadixEngine-0.9.136-Windows-x64.exe
echo [Fadix] Portable release player:  artifacts\FadixPlayer-0.9.136-Windows-x64.exe
exit /b 0
