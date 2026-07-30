#include "assets/AssetDatabase.hpp"

#include "assets/MaterialAssetJson.hpp"
#include "assets/TextureProcessor.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <fstream>
#include <future>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <SDL3_image/SDL_image.h>
#include "engine/assets/TextureAsset.hpp"
#include "engine/assets/MaterialAsset.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{
using namespace fadix;

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string AssetTypeFor(const std::filesystem::path& path)
{
    const std::string extension = Lower(path.extension().string());
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
        extension == ".bmp" || extension == ".tga" || extension == ".hdr")
    {
        return "Texture";
    }
    if (extension == ".dds")
    {
        return "TextureUnsupported";
    }
    if (extension == ".gltf" || extension == ".glb")
    {
        return "Mesh";
    }
    if (extension == ".material" || extension == ".mat")
    {
        return "Material";
    }
    if (extension == ".scene" || extension == ".fadixscene")
    {
        return "Scene";
    }
    if (extension == ".prefab")
    {
        return "Prefab";
    }
    if (extension == ".fdxanim")
    {
        return "Animation";
    }
    if (extension == ".fdxcontroller")
    {
        return "AnimatorController";
    }
    if (extension == ".lua" || extension == ".cpp" || extension == ".hpp")
    {
        return "Script";
    }
    if (extension == ".wav" || extension == ".ogg" || extension == ".mp3" || extension == ".flac")
    {
        return "Audio";
    }
    if (extension == ".rml")
    {
        return "UI";
    }
    if (extension == ".rcss")
    {
        return "UIStyle";
    }
    return {};
}

std::string PathKey(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
    if (error)
    {
        error.clear();
        normalized = std::filesystem::absolute(path, error).lexically_normal();
        if (error)
        {
            normalized = path.lexically_normal();
        }
    }
    std::string key = normalized.generic_string();
#ifdef _WIN32
    key = Lower(std::move(key));
#endif
    return key;
}

std::filesystem::path UniquePath(
    const std::filesystem::path& folder,
    const std::string& stem,
    const std::string& extension)
{
    std::filesystem::path candidate = folder / (stem + extension);
    std::error_code error;
    for (int suffix = 2; std::filesystem::exists(candidate, error) && !error; ++suffix)
    {
        candidate = folder / (stem + "_" + std::to_string(suffix) + extension);
    }
    return candidate;
}

#ifdef _WIN32
std::wstring PythonQuote(const std::filesystem::path& path)
{
    std::wstring value = path.wstring();
    std::wstring escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back(L'\'');
    for (const wchar_t character : value)
    {
        if (character == L'\\' || character == L'\'')
        {
            escaped.push_back(L'\\');
        }
        escaped.push_back(character);
    }
    escaped.push_back(L'\'');
    return escaped;
}

std::optional<std::filesystem::path> FindBlender()
{
    const auto environmentValue = [](const wchar_t* name) -> std::optional<std::wstring> {
        const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0)
        {
            return std::nullopt;
        }
        std::wstring value(required, L'\0');
        const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
        if (written == 0 || written >= required)
        {
            return std::nullopt;
        }
        value.resize(written);
        return value;
    };

    if (const std::optional<std::wstring> configured =
            environmentValue(L"FADIX_BLENDER_PATH"))
    {
        std::filesystem::path candidate{*configured};
        if (std::filesystem::is_directory(candidate))
        {
            candidate /= "blender.exe";
        }
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error)
        {
            return candidate;
        }
    }

    const std::optional<std::wstring> programFiles = environmentValue(L"ProgramFiles");
    if (!programFiles)
    {
        return std::nullopt;
    }
    const std::filesystem::path root =
        std::filesystem::path{*programFiles} / "Blender Foundation";
    std::error_code error;
    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::directory_iterator it{root, error}, end; !error && it != end;
         it.increment(error))
    {
        const std::filesystem::path candidate = it->path() / "blender.exe";
        std::error_code fileError;
        if (std::filesystem::is_regular_file(candidate, fileError) && !fileError)
        {
            candidates.push_back(candidate);
        }
    }
    std::sort(candidates.begin(), candidates.end(), std::greater<>{});
    return candidates.empty() ? std::nullopt : std::optional{candidates.front()};
}

Result<void> ConvertModelWithBlender(
    const std::filesystem::path& blender,
    const std::filesystem::path& source,
    const std::filesystem::path& destination)
{
    const std::string extension = Lower(source.extension().string());
    std::wstring import;
    if (extension == ".blend")
    {
        import = L"bpy.ops.wm.open_mainfile(filepath=" + PythonQuote(source) + L");";
    }
    else
    {
        import = L"bpy.ops.object.select_all(action='SELECT');"
                 L"bpy.ops.object.delete(use_global=False);";
        if (extension == ".fbx")
        {
            import += L"bpy.ops.import_scene.fbx(filepath=" + PythonQuote(source) +
                L",use_image_search=False);";
        }
        else if (extension == ".obj")
        {
            import += L"bpy.ops.wm.obj_import(filepath=" + PythonQuote(source) + L");";
        }
        else if (extension == ".gltf")
        {
            import += L"bpy.ops.import_scene.gltf(filepath=" + PythonQuote(source) + L");";
        }
        else if (extension == ".stl")
        {
            import += L"bpy.ops.wm.stl_import(filepath=" + PythonQuote(source) + L");";
        }
        else if (extension == ".ply")
        {
            import += L"bpy.ops.wm.ply_import(filepath=" + PythonQuote(source) + L");";
        }
        else if (extension == ".dae")
        {
            import += L"bpy.ops.wm.collada_import(filepath=" + PythonQuote(source) + L");";
        }
        else
        {
            return Result<void>::Error("Unsupported 3D model format: " + extension);
        }
    }

    const std::wstring script = L"import bpy;" + import +
        L"assert any(o.type=='MESH' for o in bpy.context.scene.objects),'No mesh geometry found';"
        L"bpy.ops.export_scene.gltf(filepath=" + PythonQuote(destination) +
        L",export_format='GLB',export_materials='NONE',export_cameras=False,"
        L"export_lights=False,export_apply=True);bpy.ops.wm.quit_blender();";
    std::wstring command = L"\"" + blender.wstring() +
        L"\" --background --factory-startup --python-exit-code 1 --python-expr \"" +
        script + L"\"";

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(blender.c_str(),
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process))
    {
        return Result<void>::Error(
            "Could not start Blender (Windows error " + std::to_string(GetLastError()) + ")");
    }
    // The editor invokes this blocking process on its external-import worker,
    // keeping Blender startup time off the viewport thread.
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0)
    {
        return Result<void>::Error(
            "Blender could not convert " + source.filename().string() +
            " (exit " + std::to_string(exitCode) + ")");
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(destination, error) || error ||
        std::filesystem::file_size(destination, error) < 20 || error)
    {
        return Result<void>::Error("Blender did not create a valid GLB output");
    }
    return Result<void>::Ok();
}
#endif

std::filesystem::path MetadataPath(const std::filesystem::path& sourcePath)
{
    return std::filesystem::path{sourcePath.string() + ".fadixmeta"};
}

Result<void> WriteMetadata(
    const std::filesystem::path& sourcePath,
    const AssetHandle& handle,
    const std::string& type,
    const std::filesystem::path& importedPath)
{
    std::ofstream stream{MetadataPath(sourcePath), std::ios::trunc};
    if (!stream)
    {
        return Result<void>::Error("Could not write asset metadata for " + sourcePath.string());
    }
    stream << "handle=" << handle.ToString() << '\n'
           << "type=" << type << '\n'
           << "imported=" << importedPath.generic_string() << '\n';
    if (!stream)
    {
        return Result<void>::Error("Could not finish writing asset metadata for " + sourcePath.string());
    }
    return Result<void>::Ok();
}

std::optional<AssetHandle> ReadHandle(const std::filesystem::path& sourcePath)
{
    std::ifstream stream{MetadataPath(sourcePath)};
    std::string line;
    while (std::getline(stream, line))
    {
        constexpr std::string_view prefix{"handle="};
        if (line.starts_with(prefix))
        {
            return AssetHandle::Parse(std::string_view{line}.substr(prefix.size()));
        }
    }
    return std::nullopt;
}
}

namespace fadix
{
struct AssetDatabase::ImportJob
{
    AssetMetadata Metadata;
    std::promise<Result<AssetHandle>> Promise;
    std::future<void> Worker;
    std::atomic<AssetImportState> State{AssetImportState::Queued};
    std::atomic<float> Progress{0.0F};
    std::mutex ErrorMutex;
    std::string Error;
    std::size_t StatusIndex{};
};

AssetDatabase::AssetDatabase(std::filesystem::path projectRoot)
    : m_ProjectRoot(std::move(projectRoot)),
      m_AssetsRoot(m_ProjectRoot / "Assets"),
      m_ImportRoot(m_ProjectRoot / ".fadix" / "imported")
{
    static_cast<void>(Refresh());
}

AssetDatabase::~AssetDatabase()
{
    for (const std::unique_ptr<ImportJob>& job : m_Jobs)
    {
        if (job->Worker.valid())
        {
            job->Worker.wait();
        }
    }
}

std::future<Result<AssetHandle>> AssetDatabase::ImportAsync(
    const std::filesystem::path& sourcePath)
{
    const std::filesystem::path resolved = ResolveSource(sourcePath);
    Result<AssetMetadata> metadataResult = ReadOrCreateMetadata(resolved);
    if (!metadataResult)
    {
        std::promise<Result<AssetHandle>> promise;
        promise.set_value(Result<AssetHandle>::Error(metadataResult.ErrorMessage()));
        return promise.get_future();
    }

    auto job = std::make_unique<ImportJob>();
    job->Metadata = std::move(metadataResult).Value();
    job->StatusIndex = m_Statuses.size();
    m_Statuses.push_back(AssetImportStatus{
        job->Metadata.Handle, job->Metadata.SourcePath, AssetImportState::Queued, 0.0F, {}});
    std::future<Result<AssetHandle>> result = job->Promise.get_future();
    ImportJob* jobPointer = job.get();

    job->Worker = std::async(std::launch::async, [jobPointer]()
    {
        jobPointer->State.store(AssetImportState::Importing);
        jobPointer->Progress.store(0.15F);
        try
        {
            std::error_code error;
            std::filesystem::create_directories(jobPointer->Metadata.ImportedPath.parent_path(), error);
            if (error)
            {
                throw std::filesystem::filesystem_error{
                    "Could not create imported asset directory",
                    jobPointer->Metadata.ImportedPath.parent_path(),
                    error};
            }

            jobPointer->Progress.store(0.45F);

            if (jobPointer->Metadata.Type == "TextureUnsupported")
            {
                throw std::runtime_error(
                    "Recognized texture extension is not supported yet: " +
                    jobPointer->Metadata.SourcePath.extension().string());
            }

            if (jobPointer->Metadata.Type == "Texture")
            {
                const TextureImportSettings settings =
                    ReadTextureImportSettings(MetadataPath(jobPointer->Metadata.SourcePath));
                const std::string ext = Lower(jobPointer->Metadata.SourcePath.extension().string());
                // .hdr decodes to linear float via stb; everything else via SDL_image.
                Result<ProcessedTextureData> processed = ext == ".hdr"
                    ? ProcessHdrFile(jobPointer->Metadata.SourcePath, settings)
                    : [&]() {
                          SDL_Surface* surface =
                              IMG_Load(jobPointer->Metadata.SourcePath.string().c_str());
                          if (!surface)
                          {
                              return Result<ProcessedTextureData>::Error(
                                  std::string("Failed to load image: ") + SDL_GetError());
                          }
                          Result<ProcessedTextureData> result =
                              ProcessTextureSurface(surface, settings);
                          SDL_DestroySurface(surface);
                          return result;
                      }();
                if (!processed)
                {
                    throw std::runtime_error(processed.ErrorMessage());
                }

                Result<void> writeProcessed = WriteProcessedTexture(
                    jobPointer->Metadata.ImportedPath,
                    processed.Value());
                if (!writeProcessed)
                {
                    throw std::runtime_error(writeProcessed.ErrorMessage());
                }
                static_cast<void>(WriteTextureImportSettings(
                    MetadataPath(jobPointer->Metadata.SourcePath),
                    processed.Value().Settings));
            }
            else if (jobPointer->Metadata.Type == "Material")
            {
                // Material source lives in Assets; imported copy mirrors it.
                std::filesystem::copy_file(
                    jobPointer->Metadata.SourcePath,
                    jobPointer->Metadata.ImportedPath,
                    std::filesystem::copy_options::overwrite_existing,
                    error);
                if (error)
                {
                    throw std::filesystem::filesystem_error{
                        "Could not import material asset",
                        jobPointer->Metadata.SourcePath,
                        error};
                }
            }
            else
            {
                // For other types (e.g. Mesh, Scene) just copy for now
                std::filesystem::copy_file(
                    jobPointer->Metadata.SourcePath,
                    jobPointer->Metadata.ImportedPath,
                    std::filesystem::copy_options::overwrite_existing,
                    error);
                if (error)
                {
                    throw std::filesystem::filesystem_error{
                        "Could not import asset", jobPointer->Metadata.SourcePath, error};
                }
            }

            jobPointer->Progress.store(0.85F);
            Result<void> writeResult = WriteMetadata(
                jobPointer->Metadata.SourcePath,
                jobPointer->Metadata.Handle,
                jobPointer->Metadata.Type,
                jobPointer->Metadata.ImportedPath);
            if (!writeResult)
            {
                throw std::runtime_error{writeResult.ErrorMessage()};
            }

            jobPointer->Progress.store(1.0F);
            jobPointer->State.store(AssetImportState::Completed);
            jobPointer->Promise.set_value(Result<AssetHandle>::Ok(jobPointer->Metadata.Handle));
        }
        catch (const std::exception& exception)
        {
            {
                std::scoped_lock lock{jobPointer->ErrorMutex};
                jobPointer->Error = exception.what();
            }
            jobPointer->State.store(AssetImportState::Failed);
            jobPointer->Promise.set_value(Result<AssetHandle>::Error(exception.what()));
        }
    });

    m_Jobs.push_back(std::move(job));
    return result;
}

std::optional<AssetHandle> AssetDatabase::FindByPath(const std::filesystem::path& path) const
{
    const std::string key = PathKey(ResolveSource(path));
    const auto iterator = std::find_if(m_Assets.begin(), m_Assets.end(), [&key](const AssetMetadata& metadata)
    {
        return PathKey(metadata.SourcePath) == key;
    });
    return iterator == m_Assets.end() ? std::nullopt : std::optional{iterator->Handle};
}

const AssetMetadata* AssetDatabase::Meta(const AssetHandle& handle) const noexcept
{
    const auto iterator = std::find_if(m_Assets.begin(), m_Assets.end(), [&handle](const AssetMetadata& metadata)
    {
        return metadata.Handle == handle;
    });
    return iterator == m_Assets.end() ? nullptr : &*iterator;
}

void AssetDatabase::PollImports()
{
    for (auto iterator = m_Jobs.begin(); iterator != m_Jobs.end();)
    {
        ImportJob& job = **iterator;
        AssetImportStatus& status = m_Statuses[job.StatusIndex];
        status.State = job.State.load();
        status.Progress = job.Progress.load();
        if (status.State == AssetImportState::Failed)
        {
            std::scoped_lock lock{job.ErrorMutex};
            status.Error = job.Error;
        }

        if (status.State == AssetImportState::Completed)
        {
            Upsert(job.Metadata);
            std::error_code error;
            if (const auto timestamp =
                    std::filesystem::last_write_time(job.Metadata.SourcePath, error);
                !error)
            {
                std::scoped_lock lock{m_StateMutex};
                m_SourceTimestamps[job.Metadata.Handle] = timestamp;
            }
        }
        if (status.State == AssetImportState::Completed || status.State == AssetImportState::Failed)
        {
            if (job.Worker.valid())
            {
                job.Worker.get();
            }
            iterator = m_Jobs.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
    PollHotReimports();
}

std::span<const AssetMetadata> AssetDatabase::List() const noexcept
{
    return m_Assets;
}

Result<void> AssetDatabase::Refresh()
{
    std::error_code error;
    std::filesystem::create_directories(m_AssetsRoot, error);
    if (error)
    {
        return Result<void>::Error("Could not create Assets directory: " + error.message());
    }

    std::vector<AssetMetadata> scanned;
    const auto scan = [this, &scanned](const std::filesystem::path& root) -> Result<void> {
        std::error_code scanError;
        std::filesystem::recursive_directory_iterator iterator{
            root, std::filesystem::directory_options::skip_permission_denied, scanError};
        const std::filesystem::recursive_directory_iterator end;
        while (!scanError && iterator != end)
        {
            if (iterator->is_regular_file(scanError) &&
                iterator->path().extension() != ".fadixmeta" &&
                !AssetTypeFor(iterator->path()).empty())
            {
                Result<AssetMetadata> result = ReadOrCreateMetadata(iterator->path());
                if (result)
                {
                    scanned.push_back(std::move(result).Value());
                }
            }
            iterator.increment(scanError);
        }
        return scanError
            ? Result<void>::Error("Could not scan content directory: " + scanError.message())
            : Result<void>::Ok();
    };
    if (Result<void> result = scan(m_AssetsRoot); !result)
    {
        return result;
    }
    const std::filesystem::path scenesRoot = m_ProjectRoot / "Scenes";
    if (PathKey(scenesRoot) != PathKey(m_AssetsRoot))
    {
        std::filesystem::create_directories(scenesRoot, error);
        if (error)
        {
            return Result<void>::Error("Could not create Scenes directory: " + error.message());
        }
        if (Result<void> result = scan(scenesRoot); !result)
        {
            return result;
        }
    }
    std::sort(scanned.begin(), scanned.end(), [](const AssetMetadata& left, const AssetMetadata& right)
    {
        return left.SourcePath.generic_string() < right.SourcePath.generic_string();
    });
    m_Assets = std::move(scanned);
    {
        std::scoped_lock lock{m_StateMutex};
        m_SourceTimestamps.clear();
        for (const AssetMetadata& metadata : m_Assets)
        {
            std::error_code timestampError;
            if (const auto timestamp =
                    std::filesystem::last_write_time(metadata.SourcePath, timestampError);
                !timestampError)
            {
                m_SourceTimestamps[metadata.Handle] = timestamp;
            }
        }
    }
    return Result<void>::Ok();
}

std::span<const AssetImportStatus> AssetDatabase::ImportStatuses() const noexcept
{
    return m_Statuses;
}

const std::filesystem::path& AssetDatabase::ProjectRoot() const noexcept
{
    return m_ProjectRoot;
}

const std::filesystem::path& AssetDatabase::AssetsRoot() const noexcept
{
    return m_AssetsRoot;
}

Result<std::filesystem::path> AssetDatabase::ImportExternalModel(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationFolder,
    const bool refreshDatabase)
{
    std::error_code error;
    const std::filesystem::path source =
        std::filesystem::absolute(sourcePath, error).lexically_normal();
    if (error || !std::filesystem::is_regular_file(source, error) || error)
    {
        return Result<std::filesystem::path>::Error(
            "Model file does not exist: " + sourcePath.string());
    }

    const std::filesystem::path destination =
        (destinationFolder.empty() ? m_AssetsRoot : ResolveSource(destinationFolder))
            .lexically_normal();
    const std::string assetsKey = PathKey(m_AssetsRoot);
    const std::string destinationKey = PathKey(destination);
    if (destinationKey != assetsKey &&
        !destinationKey.starts_with(assetsKey + "/"))
    {
        return Result<std::filesystem::path>::Error(
            "Models can only be imported into the project's Assets folder");
    }
    std::filesystem::create_directories(destination, error);
    if (error)
    {
        return Result<std::filesystem::path>::Error(
            "Could not create import folder: " + error.message());
    }

    const std::string extension = Lower(source.extension().string());
    constexpr std::array supported{
        ".glb", ".gltf", ".fbx", ".blend", ".obj", ".stl", ".ply", ".dae"};
    if (std::find(supported.begin(), supported.end(), extension) == supported.end())
    {
        return Result<std::filesystem::path>::Error(
            "Unsupported 3D model format: " + source.extension().string());
    }

    const std::filesystem::path output =
        UniquePath(destination, source.stem().string(), ".glb");
    if (extension == ".glb")
    {
        std::filesystem::copy_file(source, output, std::filesystem::copy_options::none, error);
        if (error)
        {
            return Result<std::filesystem::path>::Error(
                "Could not copy model into Assets: " + error.message());
        }
    }
    else
    {
#ifdef _WIN32
        const std::optional<std::filesystem::path> blender = FindBlender();
        if (!blender)
        {
            return Result<std::filesystem::path>::Error(
                "Blender is required to import " + source.extension().string() +
                " models. Install Blender or set FADIX_BLENDER_PATH.");
        }
        Result<void> conversion = ConvertModelWithBlender(*blender, source, output);
        if (!conversion)
        {
            std::filesystem::remove(output, error);
            return Result<std::filesystem::path>::Error(conversion.ErrorMessage());
        }
#else
        return Result<std::filesystem::path>::Error(
            "This model format requires Blender conversion, which is only configured on Windows");
#endif
    }

    if (refreshDatabase)
    {
        Result<void> refresh = Refresh();
        if (!refresh)
        {
            return Result<std::filesystem::path>::Error(refresh.ErrorMessage());
        }
    }
    return Result<std::filesystem::path>::Ok(output);
}

std::filesystem::path AssetDatabase::ResolveSource(const std::filesystem::path& path) const
{
    if (path.is_absolute())
    {
        return path.lexically_normal();
    }
    if (!path.empty() && *path.begin() == "Assets")
    {
        return (m_ProjectRoot / path).lexically_normal();
    }
    return (m_AssetsRoot / path).lexically_normal();
}

Result<AssetMetadata> AssetDatabase::ReadOrCreateMetadata(
    const std::filesystem::path& sourcePath)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(sourcePath, error) || error)
    {
        return Result<AssetMetadata>::Error("Asset source does not exist: " + sourcePath.string());
    }
    const std::string type = AssetTypeFor(sourcePath);
    if (type.empty())
    {
        return Result<AssetMetadata>::Error("Unsupported asset type: " + sourcePath.extension().string());
    }
    if (type == "TextureUnsupported")
    {
        return Result<AssetMetadata>::Error(
            "Recognized texture extension is not supported yet: " + sourcePath.extension().string());
    }

    AssetHandle handle = ReadHandle(sourcePath).value_or(AssetHandle::Generate());
    const std::filesystem::path imported =
        m_ImportRoot / (handle.ToString() + Lower(sourcePath.extension().string()));
    Result<void> writeResult = WriteMetadata(sourcePath, handle, type, imported);
    if (!writeResult)
    {
        return Result<AssetMetadata>::Error(writeResult.ErrorMessage());
    }
    return Result<AssetMetadata>::Ok(AssetMetadata{handle, sourcePath, imported, type});
}

void AssetDatabase::Upsert(AssetMetadata metadata)
{
    const auto iterator = std::find_if(m_Assets.begin(), m_Assets.end(), [&metadata](const AssetMetadata& existing)
    {
        return existing.Handle == metadata.Handle;
    });
    if (iterator == m_Assets.end())
    {
        m_Assets.push_back(std::move(metadata));
    }
    else
    {
        *iterator = std::move(metadata);
    }
}

std::optional<AssetHandle> AssetDatabase::FindByHandle(const AssetHandle& handle) const
{
    const auto iterator = std::find_if(m_Assets.begin(), m_Assets.end(), [&handle](const AssetMetadata& metadata)
    {
        return metadata.Handle == handle;
    });
    return iterator == m_Assets.end() ? std::nullopt : std::optional{iterator->Handle};
}

Result<AssetHandle> AssetDatabase::CreateMaterial(
    const std::string_view name,
    const std::filesystem::path& folder)
{
    if (name.empty())
    {
        return Result<AssetHandle>::Error("Material name is required");
    }
    const std::filesystem::path targetFolder = folder.empty() ? m_AssetsRoot : ResolveSource(folder);
    std::error_code error;
    std::filesystem::create_directories(targetFolder, error);
    if (error)
    {
        return Result<AssetHandle>::Error("Could not create material folder: " + error.message());
    }
    const std::filesystem::path source = targetFolder / (std::string{name} + ".material");
    if (std::filesystem::exists(source, error))
    {
        return Result<AssetHandle>::Error("A material with that name already exists");
    }

    const AssetHandle handle = AssetHandle::Generate();
    MaterialAsset asset = DefaultMaterialAsset(handle, std::string{name});
    Result<void> saveResult = SaveMaterialAsset(source, asset);
    if (!saveResult)
    {
        return Result<AssetHandle>::Error(saveResult.ErrorMessage());
    }

    Result<AssetMetadata> metadataResult = ReadOrCreateMetadata(source);
    if (!metadataResult)
    {
        return Result<AssetHandle>::Error(metadataResult.ErrorMessage());
    }
    Upsert(std::move(metadataResult).Value());
    static_cast<void>(Refresh());
    return Result<AssetHandle>::Ok(handle);
}

Result<AssetHandle> AssetDatabase::DuplicateAsset(const std::filesystem::path& sourcePath)
{
    const std::filesystem::path resolved = ResolveSource(sourcePath);
    const std::optional<AssetHandle> existing = FindByPath(resolved);
    if (!existing)
    {
        return Result<AssetHandle>::Error("Asset does not exist: " + resolved.string());
    }
    const AssetMetadata* metadata = Meta(*existing);
    if (metadata == nullptr)
    {
        return Result<AssetHandle>::Error("Asset metadata is missing");
    }

    const std::string stem = resolved.stem().string() + "_Copy";
    std::filesystem::path destination = resolved.parent_path() / (stem + resolved.extension().string());
    int suffix = 1;
    std::error_code error;
    while (std::filesystem::exists(destination, error))
    {
        destination = resolved.parent_path() /
            (stem + std::to_string(suffix++) + resolved.extension().string());
    }

    std::filesystem::copy_file(resolved, destination, std::filesystem::copy_options::none, error);
    if (error)
    {
        return Result<AssetHandle>::Error("Could not duplicate asset: " + error.message());
    }

    if (metadata->Type == "Material")
    {
        Result<MaterialAsset> material = LoadMaterialAsset(resolved, *existing);
        if (!material)
        {
            std::filesystem::remove(destination, error);
            return Result<AssetHandle>::Error(material.ErrorMessage());
        }
        material.Value().Name = destination.stem().string();
        static_cast<void>(SaveMaterialAsset(destination, material.Value()));
    }

    Result<AssetMetadata> metadataResult = ReadOrCreateMetadata(destination);
    if (!metadataResult)
    {
        std::filesystem::remove(destination, error);
        return Result<AssetHandle>::Error(metadataResult.ErrorMessage());
    }
    AssetMetadata duplicated = std::move(metadataResult).Value();
    Upsert(duplicated);
    static_cast<void>(ImportAsync(duplicated.SourcePath));
    static_cast<void>(Refresh());
    return Result<AssetHandle>::Ok(duplicated.Handle);
}

std::future<Result<AssetHandle>> AssetDatabase::ReimportTexture(const AssetHandle& handle)
{
    const AssetMetadata* metadata = Meta(handle);
    if (metadata == nullptr || metadata->Type != "Texture")
    {
        std::promise<Result<AssetHandle>> promise;
        promise.set_value(Result<AssetHandle>::Error("Texture asset not found"));
        return promise.get_future();
    }
    return ImportAsync(metadata->SourcePath);
}

void AssetDatabase::PollHotReimports()
{
    for (const AssetMetadata& metadata : m_Assets)
    {
        if (metadata.Type != "Texture")
        {
            continue;
        }
        std::error_code error;
        const auto timestamp = std::filesystem::last_write_time(metadata.SourcePath, error);
        if (error)
        {
            continue;
        }
        std::filesystem::file_time_type previous{};
        {
            std::scoped_lock lock{m_StateMutex};
            const auto iterator = m_SourceTimestamps.find(metadata.Handle);
            if (iterator == m_SourceTimestamps.end())
            {
                m_SourceTimestamps[metadata.Handle] = timestamp;
                continue;
            }
            previous = iterator->second;
            if (timestamp <= previous)
            {
                continue;
            }
            m_SourceTimestamps[metadata.Handle] = timestamp;
        }

        bool alreadyQueued = false;
        for (const std::unique_ptr<ImportJob>& job : m_Jobs)
        {
            if (job->Metadata.Handle == metadata.Handle)
            {
                alreadyQueued = true;
                break;
            }
        }
        if (!alreadyQueued)
        {
            static_cast<void>(ImportAsync(metadata.SourcePath));
        }
    }
}

Result<MaterialAsset> AssetDatabase::LoadMaterial(const AssetHandle& handle) const
{
    const AssetMetadata* metadata = Meta(handle);
    if (metadata == nullptr || metadata->Type != "Material")
    {
        return Result<MaterialAsset>::Error("Material asset not found");
    }
    return LoadMaterialAsset(metadata->SourcePath, handle);
}

Result<void> AssetDatabase::SaveMaterial(const MaterialAsset& asset)
{
    const AssetMetadata* metadata = Meta(asset.Handle);
    if (metadata == nullptr || metadata->Type != "Material")
    {
        return Result<void>::Error("Material asset not found");
    }
    Result<void> saveResult = SaveMaterialAsset(metadata->SourcePath, asset);
    if (!saveResult)
    {
        return saveResult;
    }
    std::error_code error;
    std::filesystem::copy_file(
        metadata->SourcePath,
        metadata->ImportedPath,
        std::filesystem::copy_options::overwrite_existing,
        error);
    if (error)
    {
        return Result<void>::Error("Could not refresh imported material copy: " + error.message());
    }
    return Result<void>::Ok();
}

std::unique_ptr<IAssetDatabase> CreateAssetDatabase(const std::filesystem::path& projectRoot)
{
    return std::make_unique<AssetDatabase>(projectRoot);
}
}
