#include "satview_cache_publication.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace draxul::satview::detail
{

namespace
{

std::atomic<std::uint64_t> g_cache_temporary_sequence{ 0 };

} // namespace

std::error_code replace_cache_file_atomically(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination)
{
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        return std::error_code(static_cast<int>(GetLastError()), std::system_category());
    }
    return {};
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    return error;
#endif
}

bool write_cache_text_atomically(
    const std::filesystem::path& destination,
    std::string_view content,
    std::string& error)
{
    return write_cache_text_atomically(
        destination, content, error, replace_cache_file_atomically);
}

bool write_cache_text_atomically(
    const std::filesystem::path& destination,
    std::string_view content,
    std::string& error,
    const CacheFileReplacement& replace_file)
{
    std::error_code filesystem_error;
    std::filesystem::create_directories(destination.parent_path(), filesystem_error);
    if (filesystem_error)
    {
        error = "failed to create cache directory: " + filesystem_error.message();
        return false;
    }

    const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path temporary = destination.string() + ".tmp."
        + std::to_string(timestamp) + "."
        + std::to_string(g_cache_temporary_sequence.fetch_add(1, std::memory_order_relaxed));
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            error = "failed to open " + temporary.string();
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out.good())
        {
            error = "failed to write " + temporary.string();
            return false;
        }
    }

    filesystem_error = replace_file(temporary, destination);
    if (filesystem_error)
    {
        // Never remove the destination as a replacement fallback: another
        // service may just have published it. Keep the last valid cache and
        // discard only this writer's private temporary file.
        const std::string replace_error = filesystem_error.message();
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        error = "failed to replace " + destination.string() + ": " + replace_error;
        return false;
    }
    return true;
}

} // namespace draxul::satview::detail
