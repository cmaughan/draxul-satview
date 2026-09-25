#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace draxul::satview
{

using SatViewCsvRow = std::vector<std::string>;

enum class SatViewCsvLexError
{
    None,
    UnexpectedDataAfterQuotedField,
    UnexpectedQuote,
    UnterminatedQuotedField,
};

struct SatViewCsvLexResult
{
    SatViewCsvLexError error = SatViewCsvLexError::None;

    [[nodiscard]] explicit operator bool() const
    {
        return error == SatViewCsvLexError::None;
    }
};

// RFC 4180-style lexical ownership for every SatView text catalogue. This
// routine only separates rows and fields; domain validation and diagnostics
// remain with each catalogue parser. Completed rows remain available on error.
SatViewCsvLexResult tokenize_satview_csv(
    std::string_view csv,
    std::vector<SatViewCsvRow>& rows);

} // namespace draxul::satview
