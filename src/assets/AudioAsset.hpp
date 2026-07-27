#pragma once

#include <filesystem>
#include <string>

namespace fadix
{

struct AudioAsset
{
    std::filesystem::path SourcePath;
    std::string Name;
    std::string Format;
};

[[nodiscard]] inline const char* AudioFormatFromExtension(const std::filesystem::path& path)
{
    const std::string extension = path.extension().string();
    if (extension == ".wav" || extension == ".WAV")
    {
        return "wav";
    }
    if (extension == ".ogg" || extension == ".OGG")
    {
        return "ogg";
    }
    if (extension == ".mp3" || extension == ".MP3")
    {
        return "mp3";
    }
    if (extension == ".flac" || extension == ".FLAC")
    {
        return "flac";
    }
    return "";
}

} // namespace fadix
