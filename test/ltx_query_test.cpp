/* Tests for the query layer: pattern management, compilation errors, filters
 * and the shape of what a match reports. */

#include <gtest/gtest.h>

#include "latex/ltx_query.hpp"
#include "test_utils.hpp"

#include <string>
#include <vector>

using namespace Ltx;

namespace {

	const char* kSource =
		"\\documentclass{article}\n"
		"\\begin{document}\n"
		"\\section{Introduction}\n"
		"\\section{Results}\n"
		"\\label{sec:results}\n"
		"\\end{document}\n";

	const char* kTitlePattern = "(section text: (curly_group (text) @title))";
	const char* kLabelPattern = "(label_definition) @def";

} // namespace

/* ------------------------------------------------------ pattern management */

TEST(Query, EmptyQueryIsValidButHasNoPatterns) {
	Query query(ltx_test::latex());

	EXPECT_TRUE(query.is_valid());
	EXPECT_FALSE(query.has_patterns());
	EXPECT_EQ(query.pattern_count(), 0u);
	EXPECT_EQ(query.capture_count(), 0u);
}

TEST(Query, AddAssignsSequentialPatternIndices) {
	Query query(ltx_test::latex());

	const auto first = query.add(kTitlePattern);
	const auto second = query.add(kLabelPattern);

	ASSERT_TRUE(first.has_value());
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(*first, 0u);
	EXPECT_EQ(*second, 1u);
	EXPECT_EQ(query.pattern_count(), 2u);
	EXPECT_TRUE(query.has_patterns());
}

TEST(Query, StoresAndReturnsPatternSources) {
	Query query(ltx_test::latex());
	query.add(kTitlePattern);

	ASSERT_EQ(query.pattern_sources().size(), 1u);
	EXPECT_EQ(query.pattern_source(0), kTitlePattern);
	EXPECT_EQ(query.pattern_sources().front(), kTitlePattern);

	/* Out of range yields an empty view rather than undefined behaviour. */
	EXPECT_TRUE(query.pattern_source(7).empty());
}

TEST(Query, InvalidPatternIsRejectedAndEarlierOnesSurvive) {
	Query query(ltx_test::latex());
	ASSERT_TRUE(query.add(kTitlePattern).has_value());

	EXPECT_FALSE(query.add("(nosuchnode) @x").has_value());

	/* The failure must not damage the query that already worked. */
	EXPECT_TRUE(query.is_valid());
	EXPECT_EQ(query.pattern_count(), 1u);
	EXPECT_EQ(query.pattern_source(0), kTitlePattern);

	const std::vector<PatternError>& errors = query.pattern_errors();
	ASSERT_EQ(errors.size(), 1u);
	EXPECT_EQ(errors.front().pattern_index, 1u);
	EXPECT_FALSE(errors.front().message.empty());
	EXPECT_EQ(errors.front().source, "(nosuchnode) @x");
}

TEST(Query, SuccessfulMutationClearsPreviousErrors) {
	Query query(ltx_test::latex());
	EXPECT_FALSE(query.add("(nosuchnode) @x").has_value());
	ASSERT_FALSE(query.pattern_errors().empty());

	ASSERT_TRUE(query.add(kTitlePattern).has_value());
	EXPECT_TRUE(query.pattern_errors().empty());
	EXPECT_TRUE(query.error_message().empty());
}

TEST(Query, AddManyCompilesAsOneBatch) {
	Query query(ltx_test::latex());

	EXPECT_EQ(query.add_many({ kTitlePattern, kLabelPattern }), 2u);
	EXPECT_EQ(query.pattern_count(), 2u);

	EXPECT_EQ(query.add_many({}), 0u);

	/* One bad pattern rejects the batch, leaving the query as it was. */
	EXPECT_EQ(query.add_many({ "(nosuchnode) @x" }), 0u);
	EXPECT_EQ(query.pattern_count(), 2u);
}

TEST(Query, RemoveShiftsRemainingIndices) {
	Query query(ltx_test::latex());
	query.add(kTitlePattern);
	query.add(kLabelPattern);

	ASSERT_TRUE(query.remove(0));
	EXPECT_EQ(query.pattern_count(), 1u);
	EXPECT_EQ(query.pattern_source(0), kLabelPattern);

	EXPECT_FALSE(query.remove(5));
}

TEST(Query, RemovingTheLastPatternLeavesAValidEmptyQuery) {
	Query query(ltx_test::latex());
	query.add(kLabelPattern);

	ASSERT_TRUE(query.remove(0));
	EXPECT_TRUE(query.is_valid());
	EXPECT_FALSE(query.has_patterns());
	EXPECT_EQ(query.pattern_count(), 0u);
}

TEST(Query, ClearDropsEveryPattern) {
	Query query(ltx_test::latex());
	query.add(kTitlePattern);
	query.add(kLabelPattern);

	query.clear();

	EXPECT_FALSE(query.has_patterns());
	EXPECT_TRUE(query.pattern_sources().empty());
	EXPECT_EQ(query.pattern_count(), 0u);
}

TEST(Query, NullLanguageIsInvalid) {
	Query query(nullptr);

	EXPECT_FALSE(query.is_valid());
	EXPECT_FALSE(query.add(kTitlePattern).has_value());
}

TEST(Query, CaptureNamesAreResolvable) {
	Query query(ltx_test::latex());
	query.add(kTitlePattern);
	query.add(kLabelPattern);

	EXPECT_EQ(query.capture_count(), 2u);
	EXPECT_TRUE(query.has_capture("title"));
	EXPECT_TRUE(query.has_capture("def"));
	EXPECT_FALSE(query.has_capture("missing"));

	EXPECT_EQ(query.capture_name(0), "title");
	EXPECT_TRUE(query.capture_name(99).empty());
}

/* --------------------------------------------------------------- reading */

TEST(Query, CapturesReportNameRangeTextAndNodeType) {
	ltx_test::ParsedSource parsed(kSource);
	ASSERT_TRUE(parsed.valid());

	Query query(ltx_test::latex());
	ASSERT_TRUE(query.add(kTitlePattern).has_value());

	const std::vector<Capture> captures = query.captures(parsed.root(), parsed.source());
	ASSERT_EQ(captures.size(), 2u);

	EXPECT_EQ(captures[0].name, "title");
	EXPECT_EQ(captures[0].text, "Introduction");
	EXPECT_EQ(std::string(captures[0].node_type()), "text");
	EXPECT_EQ(captures[1].text, "Results");

	/* The range must address exactly the captured bytes. */
	const ByteRange range = captures[0].range;
	EXPECT_EQ(parsed.source().substr(range.start, range.length()), "Introduction");
}

TEST(Query, MatchesCarryPatternIndexAndSource) {
	ltx_test::ParsedSource parsed(kSource);

	Query query(ltx_test::latex());
	query.add(kTitlePattern);
	query.add(kLabelPattern);

	const std::vector<Match> matches = query.matches(parsed.root(), parsed.source());
	ASSERT_EQ(matches.size(), 3u); /* two titles, one label */

	EXPECT_EQ(matches[0].pattern_index, 0u);
	EXPECT_EQ(matches[0].pattern_source, kTitlePattern);
	EXPECT_EQ(matches[2].pattern_index, 1u);
	EXPECT_EQ(matches[2].pattern_source, kLabelPattern);
}

TEST(Query, MatchExposesCaptureLookupAndSpanningRange) {
	ltx_test::ParsedSource parsed(kSource);

	Query query(ltx_test::latex());
	ASSERT_TRUE(query.add(kLabelPattern).has_value());

	const std::vector<Match> matches = query.matches(parsed.root(), parsed.source());
	ASSERT_EQ(matches.size(), 1u);

	const Capture* found = matches[0].find_capture("def");
	ASSERT_NE(found, nullptr);
	EXPECT_EQ(found->text, "\\label{sec:results}");

	EXPECT_EQ(matches[0].find_capture("missing"), nullptr);

	const ByteRange range = matches[0].range();
	EXPECT_EQ(parsed.source().substr(range.start, range.length()), "\\label{sec:results}");
}

TEST(Query, EmptyQueryMatchesNothing) {
	ltx_test::ParsedSource parsed(kSource);
	Query query(ltx_test::latex());

	EXPECT_TRUE(query.matches(parsed.root(), parsed.source()).empty());
	EXPECT_TRUE(query.captures(parsed.root(), parsed.source()).empty());
}

TEST(Query, NullRootMatchesNothing) {
	ltx_test::ParsedSource parsed(kSource);

	Query query(ltx_test::latex());
	query.add(kTitlePattern);

	const TSNode null_node{};
	EXPECT_TRUE(query.matches(null_node, parsed.source()).empty());
	EXPECT_TRUE(query.captures(null_node, parsed.source()).empty());
}

/* --------------------------------------------------------------- filters */

/* Regression guard: the CLI once bound this string_view to a temporary and
   every --capture filter silently matched nothing. */
TEST(Query, CaptureFilterKeepsOnlyThatName) {
	ltx_test::ParsedSource parsed(kSource);

	Query query(ltx_test::latex());
	query.add(kTitlePattern);
	query.add(kLabelPattern);

	MatchOptions options;
	options.capture = "title";

	const std::vector<Capture> titles = query.captures(parsed.root(), parsed.source(), options);
	ASSERT_EQ(titles.size(), 2u);
	for (const Capture& capture : titles) EXPECT_EQ(capture.name, "title");

	options.capture = "def";
	EXPECT_EQ(query.captures(parsed.root(), parsed.source(), options).size(), 1u);

	options.capture = "absent";
	EXPECT_TRUE(query.captures(parsed.root(), parsed.source(), options).empty());
}

TEST(Query, CaptureFilterAlsoNarrowsMatchCaptures) {
	ltx_test::ParsedSource parsed(kSource);

	Query query(ltx_test::latex());
	query.add("(section text: (curly_group (text) @title) @whole)");

	MatchOptions options;
	options.capture = "title";

	const std::vector<Match> matches = query.matches(parsed.root(), parsed.source(), options);
	ASSERT_EQ(matches.size(), 2u);

	/* Only the requested capture survives inside the match. */
	for (const Match& match : matches) {
		ASSERT_EQ(match.captures.size(), 1u);
		EXPECT_EQ(match.captures.front().name, "title");
	}
}

TEST(Query, PatternIndexFilterSelectsOnePattern) {
	ltx_test::ParsedSource parsed(kSource);

	Query query(ltx_test::latex());
	query.add(kTitlePattern);
	query.add(kLabelPattern);

	MatchOptions options;
	options.pattern_index = 1;

	const std::vector<Capture> captures = query.captures(parsed.root(), parsed.source(), options);
	ASSERT_EQ(captures.size(), 1u);
	EXPECT_EQ(captures.front().text, "\\label{sec:results}");

	options.pattern_index = 0;
	EXPECT_EQ(query.captures(parsed.root(), parsed.source(), options).size(), 2u);
}

TEST(Query, LimitStopsAfterNResults) {
	ltx_test::ParsedSource parsed(kSource);

	Query query(ltx_test::latex());
	query.add(kTitlePattern);

	MatchOptions options;
	options.limit = 1;

	EXPECT_EQ(query.captures(parsed.root(), parsed.source(), options).size(), 1u);
	EXPECT_EQ(query.matches(parsed.root(), parsed.source(), options).size(), 1u);

	/* Zero means unlimited, not "none". */
	options.limit = 0;
	EXPECT_EQ(query.captures(parsed.root(), parsed.source(), options).size(), 2u);
}

TEST(Query, ByteRangeRestrictsTheSearch) {
	ltx_test::ParsedSource parsed(kSource);

	Query query(ltx_test::latex());
	ASSERT_TRUE(query.add(kTitlePattern).has_value());

	const size_t second_title = parsed.source().find("Results");
	ASSERT_NE(second_title, std::string_view::npos);

	MatchOptions options;
	options.byte_range = ByteRange{ 0, static_cast<uint32_t>(second_title) };

	const std::vector<Capture> captures = query.captures(parsed.root(), parsed.source(), options);
	ASSERT_EQ(captures.size(), 1u);
	EXPECT_EQ(captures.front().text, "Introduction");
}

/* ---------------------------------------------------------- move semantics */

TEST(Query, MoveTransfersTheCompiledQuery) {
	Query source(ltx_test::latex());
	source.add(kTitlePattern);

	Query moved(std::move(source));
	EXPECT_TRUE(moved.is_valid());
	EXPECT_EQ(moved.pattern_count(), 1u);
	EXPECT_EQ(moved.pattern_source(0), kTitlePattern);

	Query assigned(ltx_test::latex());
	assigned = std::move(moved);
	EXPECT_TRUE(assigned.is_valid());
	EXPECT_EQ(assigned.pattern_count(), 1u);
	EXPECT_EQ(assigned.pattern_source(0), kTitlePattern);
}
