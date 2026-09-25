#include "satview_csv_tokenizer.h"

#include <catch2/catch_test_macros.hpp>

using namespace draxul::satview;

TEST_CASE("SatView CSV tokenizer covers line endings quotes and empty rows", "[satview][catalog][csv]")
{
    std::vector<SatViewCsvRow> rows;
    const auto result = tokenize_satview_csv(
        "a,b,\r\n\"embedded\nline\",\"escaped \"\"quote\"\"\"\r"
        "x,,z\n\n",
        rows);

    REQUIRE(result);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0] == SatViewCsvRow{ "a", "b", "" });
    CHECK(rows[1] == SatViewCsvRow{ "embedded\nline", "escaped \"quote\"" });
    CHECK(rows[2] == SatViewCsvRow{ "x", "", "z" });
}

TEST_CASE("SatView CSV tokenizer reports typed errors and retains completed rows", "[satview][catalog][csv]")
{
    std::vector<SatViewCsvRow> rows;
    auto result = tokenize_satview_csv("ok,row\n\"closed\"tail", rows);
    CHECK(result.error == SatViewCsvLexError::UnexpectedDataAfterQuotedField);
    REQUIRE(rows.size() == 1);
    CHECK(rows.front() == SatViewCsvRow{ "ok", "row" });

    rows.clear();
    result = tokenize_satview_csv("ok,row\nbad\"quote", rows);
    CHECK(result.error == SatViewCsvLexError::UnexpectedQuote);
    REQUIRE(rows.size() == 1);

    rows.clear();
    result = tokenize_satview_csv("ok,row\n\"unterminated", rows);
    CHECK(result.error == SatViewCsvLexError::UnterminatedQuotedField);
    REQUIRE(rows.size() == 1);
}

TEST_CASE("SatView CSV tokenizer handles empty input and final fields", "[satview][catalog][csv]")
{
    std::vector<SatViewCsvRow> rows;
    CHECK(tokenize_satview_csv({}, rows));
    CHECK(rows.empty());

    CHECK(tokenize_satview_csv("a,b,", rows));
    REQUIRE(rows.size() == 1);
    CHECK(rows.front() == SatViewCsvRow{ "a", "b", "" });
}
