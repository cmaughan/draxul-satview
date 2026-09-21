#include "satview_csv_tokenizer.h"

#include <utility>

namespace draxul::satview
{

SatViewCsvLexResult tokenize_satview_csv(
    std::string_view csv,
    std::vector<SatViewCsvRow>& rows)
{
    SatViewCsvRow row;
    std::string field;
    bool quoted = false;
    bool quote_closed = false;

    auto finish_field = [&]() {
        row.push_back(std::move(field));
        field.clear();
        quote_closed = false;
    };
    auto finish_row = [&]() {
        finish_field();
        rows.push_back(std::move(row));
        row.clear();
    };

    for (std::size_t index = 0; index < csv.size(); ++index)
    {
        const char value = csv[index];
        if (quoted)
        {
            if (value == '"')
            {
                if (index + 1 < csv.size() && csv[index + 1] == '"')
                {
                    field.push_back('"');
                    ++index;
                }
                else
                {
                    quoted = false;
                    quote_closed = true;
                }
            }
            else
            {
                field.push_back(value);
            }
            continue;
        }

        if (quote_closed && value != ',' && value != '\r' && value != '\n')
            return { SatViewCsvLexError::UnexpectedDataAfterQuotedField };
        if (value == '"')
        {
            if (!field.empty() || quote_closed)
                return { SatViewCsvLexError::UnexpectedQuote };
            quoted = true;
        }
        else if (value == ',')
        {
            finish_field();
        }
        else if (value == '\n')
        {
            finish_row();
        }
        else if (value == '\r')
        {
            if (index + 1 >= csv.size() || csv[index + 1] != '\n')
                finish_row();
        }
        else
        {
            field.push_back(value);
        }
    }

    if (quoted)
        return { SatViewCsvLexError::UnterminatedQuotedField };
    if (!field.empty() || !row.empty() || quote_closed)
        finish_row();
    while (!rows.empty() && rows.back().size() == 1 && rows.back().front().empty())
        rows.pop_back();
    return {};
}

} // namespace draxul::satview
