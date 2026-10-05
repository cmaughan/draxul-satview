#include "satview_cache_publication.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace draxul::satview::detail
{

namespace
{

std::atomic<std::uint64_t> g_cache_temporary_sequence{ 0 };

std::uint64_t current_process_id()
{
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

void discard_temporary(const std::filesystem::path& temporary)
{
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
}

} // namespace

bool close_cache_temporary(std::ofstream& temporary)
{
    temporary.flush();
    temporary.close();
    return !temporary.fail();
}

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
    return write_cache_text_atomically(destination, content, error, CacheFileOperations{});
}

bool write_cache_text_atomically(
    const std::filesystem::path& destination,
    std::string_view content,
    std::string& error,
    const CacheFileReplacement& replace_file)
{
    CacheFileOperations operations;
    operations.replace_file = replace_file;
    return write_cache_text_atomically(destination, content, error, operations);
}

bool write_cache_text_atomically(
    const std::filesystem::path& destination,
    std::string_view content,
    std::string& error,
    const CacheFileOperations& operations)
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
        + std::to_string(current_process_id()) + "."
        + std::to_string(timestamp) + "."
        + std::to_string(g_cache_temporary_sequence.fetch_add(1, std::memory_order_relaxed));
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            error = "failed to open " + temporary.string();
            discard_temporary(temporary);
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out.good())
        {
            out.close();
            discard_temporary(temporary);
            error = "failed to write " + temporary.string();
            return false;
        }
        // Buffered bytes reach the file only at flush/close; a failure there
        // leaves a truncated temporary that must never replace the cache.
        const bool closed = operations.close_temporary
            ? operations.close_temporary(out)
            : close_cache_temporary(out);
        if (!closed)
        {
            if (out.is_open())
                out.close();
            discard_temporary(temporary);
            error = "failed to finish writing " + temporary.string();
            return false;
        }
    }

    filesystem_error = operations.replace_file
        ? operations.replace_file(temporary, destination)
        : replace_cache_file_atomically(temporary, destination);
    if (filesystem_error)
    {
        // Never remove the destination as a replacement fallback: another
        // service may just have published it. Keep the last valid cache and
        // discard only this writer's private temporary file.
        const std::string replace_error = filesystem_error.message();
        discard_temporary(temporary);
        error = "failed to replace " + destination.string() + ": " + replace_error;
        return false;
    }
    return true;
}

} // namespace draxul::satview::detail
