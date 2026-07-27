@echo off
setlocal EnableExtensions EnableDelayedExpansion

if "%~1"=="" goto :build_debug
if "%~1"=="1" goto :build_debug
echo Usage: build.bat 1
echo   1 - build fadix_editor Debug
exit /b 2

:build_debug
where cl.exe >nul 2>nul
if errorlevel 1 (
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
    if errorlevel 1 exit /b %errorlevel%
)

rem Codex-hosted shells can expose both Path and PATH. MSBuild treats those as
rem duplicate dictionary keys, so remove the mixed-case entry for child tools.
set "CONAN_EXE="
for /f "delims=" %%I in ('where conan.exe 2^>nul') do if not defined CONAN_EXE set "CONAN_EXE=%%I"
if not defined CONAN_EXE (
    for /f "delims=" %%I in ('dir /b /s "%APPDATA%\Python\conan.exe" 2^>nul') do if not defined CONAN_EXE set "CONAN_EXE=%%I"
)
if not defined CONAN_EXE (
    echo [Fadix] Conan 2 was not found. Install it with: python -m pip install --user "conan^>=2,^<3"
    exit /b 1
)

set "Path="
set "CMAKE_EXE=C:\Program Files\CMake\bin\cmake.exe"

"%CONAN_EXE%" profile path default >nul 2>nul
if errorlevel 1 (
    "%CONAN_EXE%" profile detect --force
    if errorlevel 1 exit /b !errorlevel!
)

"%CONAN_EXE%" install . --output-folder=.build\conan --build=missing -s:h build_type=Debug -s:h compiler.cppstd=20 -s:b compiler.cppstd=20
if errorlevel 1 exit /b !errorlevel!

"%CMAKE_EXE%" -S . -B .build\conan-cmake -DCMAKE_TOOLCHAIN_FILE=.build/conan/conan_toolchain.cmake -DFADIX_ENABLE_PHYSICS=ON
if errorlevel 1 exit /b %errorlevel%
"%CMAKE_EXE%" --build .build\conan-cmake --config Debug --target fadix_editor --parallel 8
exit /b %errorlevel%
