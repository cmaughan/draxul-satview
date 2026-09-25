#pragma once

#include <filesystem>
#include <functional>
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

// Production uses the three-argument overload. The replacement overload keeps
// the failure policy deterministic in focused tests without changing service
// configuration or publishing a test hook in SatView's public API.
bool write_cache_text_atomically(
    const std::filesystem::path& destination,
    std::string_view content,
    std::string& error);

bool write_cache_text_atomically(
    const std::filesystem::path& destination,
    std::string_view content,
    std::string& error,
    const CacheFileReplacement& replace_file);

} // namespace draxul::satview::detail
