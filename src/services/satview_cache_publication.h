#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>
#include <system_error>

namespace draxul::satview::detail
{

using CacheFileReplacement = std::function<std::error_code(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination)>;

// Replaces destination without first removing it. On failure the destination
// remains available to readers and the caller may discard only its temporary.
std::error_code replace_cache_file_atomically(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination);

// Finishes the temporary stream: flushes buffered bytes, closes the file, and
// reports whether either stage failed. A stream that fails here must never be
// published, because the buffered tail may never have reached the file.
using CacheTemporaryClose = std::function<bool(std::ofstream& temporary)>;

bool close_cache_temporary(std::ofstream& temporary);

struct CacheFileOperations
{
    CacheTemporaryClose close_temporary = close_cache_temporary;
    CacheFileReplacement replace_file = replace_cache_file_atomically;
};

// Production uses the three-argument overload; catalog and cloud caches both
// publish through it. The operation overloads keep the failure policy
// deterministic in focused tests without changing service configuration or
// publishing a test hook in SatView's public API. On any failure the
// destination is untouched and only this writer's temporary file is removed.
bool write_cache_text_atomically(
    const std::filesystem::path& destination,
    std::string_view content,
    std::string& error);

bool write_cache_text_atomically(
    const std::filesystem::path& destination,
    std::string_view content,
    std::string& error,
    const CacheFileReplacement& replace_file);

bool write_cache_text_atomically(
    const std::filesystem::path& destination,
    std::string_view content,
    std::string& error,
    const CacheFileOperations& operations);

} // namespace draxul::satview::detail
