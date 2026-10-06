/* Tests for the agent-facing CLI.
 *
 * Every case drives Ltx::Cli::run() in-process (see test_utils.hpp), so these
 * assert the contract an agent relies on: one command per invocation, JSON on
 * stdout, errors on stderr, and meaningful exit codes. */

#include <gtest/gtest.h>

#include "test_utils.hpp"

#include <string>
#include <vector>

using ltx_test::contains;
using ltx_test::run_cli;
using ltx_test::TempFile;

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

/* ------------------------------------------------------ dispatch and usage */

TEST(Cli, VersionReportsToolAndApiLevel) {
	const auto result = run_cli({ "version" });

	EXPECT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(result.out, "\"tool\":\"docutils\""));
	EXPECT_TRUE(contains(result.out, "\"tree_sitter_api_version\""));
}

TEST(Cli, HelpSucceeds) {
	const auto result = run_cli({ "help" });

	EXPECT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(result.out, "EXIT CODES"));
}

TEST(Cli, BareInvocationIsAUsageError) {
	const auto result = run_cli({});

	EXPECT_EQ(result.exit_code, 1);
	EXPECT_TRUE(contains(result.out, "USAGE"));
}

TEST(Cli, UnknownCommandFails) {
	const auto result = run_cli({ "frobnicate" });

	EXPECT_EQ(result.exit_code, 1);
	EXPECT_TRUE(contains(result.err, "unknown_command"));
}

TEST(Cli, UnknownOptionFails) {
	TempFile file("a.tex", kSource);
	const auto result = run_cli({ "query", "--file", file.path(), "--bogus" });

	/* Option parsing reports the generic usage_error code, with the offending
	   option named in the message. */
	EXPECT_EQ(result.exit_code, 1);
	EXPECT_TRUE(contains(result.err, "usage_error"));
	EXPECT_TRUE(contains(result.err, "unknown option '--bogus'"));
}

TEST(Cli, OptionWithoutItsValueFails) {
	TempFile file("a.tex", kSource);
	const auto result = run_cli({ "query", "--file", file.path(), "--query" });

	EXPECT_EQ(result.exit_code, 1);
	EXPECT_TRUE(contains(result.err, "requires a value"));
}

TEST(Cli, MissingInputFileFails) {
	const auto result = run_cli({
		"query", "--file", "definitely-missing-file.tex", "--query", "(section) @s" });

	EXPECT_EQ(result.exit_code, 1);
	EXPECT_TRUE(contains(result.err, "input_error"));
}

/* -------------------------------------------------------------- query */

TEST(Cli, QueryReportsCapturesAndExitsZero) {
	TempFile file("a.tex", kSource);
	const auto result = run_cli({
		"query", "--file", file.path(), "--query", kTitlePattern });

	EXPECT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(result.out, "\"matched\":2"));
	EXPECT_TRUE(contains(result.out, "\"text\":\"Introduction\""));
}

TEST(Cli, QueryWithoutMatchesExitsTwo) {
	TempFile file("a.tex", kSource);
	const auto result = run_cli({ "query", "--file", file.path(), "--query", "(chapter) @c" });

	EXPECT_EQ(result.exit_code, 2);
	EXPECT_TRUE(contains(result.out, "\"matched\":0"));
}

TEST(Cli, QueryWithoutAPatternIsAUsageError) {
	TempFile file("a.tex", kSource);
	const auto result = run_cli({ "query", "--file", file.path() });

	EXPECT_EQ(result.exit_code, 1);
	EXPECT_TRUE(contains(result.err, "no query given"));
}

TEST(Cli, InvalidPatternFails) {
	TempFile file("a.tex", kSource);
	const auto result = run_cli({ "query", "--file", file.path(), "--query", "(nosuchnode) @x" });

	EXPECT_EQ(result.exit_code, 1);
	EXPECT_TRUE(contains(result.err, "query_error"));
}

TEST(Cli, PositionalFileIsAccepted) {
	TempFile file("a.tex", kSource);
	const auto result = run_cli({ "query", file.path(), "--query", kLabelPattern });

	EXPECT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(result.out, "\"matched\":1"));
}

TEST(Cli, GroupReportsWholeMatches) {
	TempFile file("a.tex", kSource);
	const auto result = run_cli({ "query", "--file", file.path(), "--group", "--query", kLabelPattern });

	ASSERT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(result.out, "\"matches\":["));
	EXPECT_TRUE(contains(result.out, "\"pattern_source\""));
}

TEST(Cli, TextFormatIsNotJson) {
	TempFile file("a.tex", kSource);
	const auto result = run_cli({
		"query", "--file", file.path(), "--format", "text", "--query", kTitlePattern });

	ASSERT_EQ(result.exit_code, 0);
	EXPECT_FALSE(contains(result.out, "\"command\""));
	EXPECT_TRUE(contains(result.out, "Introduction"));
}

TEST(Cli, PatternIndexOutOfRangeFails) {
	TempFile file("a.tex", kSource);
	const auto result = run_cli({
		"query", "--file", file.path(), "--query", "(section) @s", "--pattern-index", "5" });

	EXPECT_EQ(result.exit_code, 1);
	EXPECT_TRUE(contains(result.err, "out of range"));
}

TEST(Cli, ByteRangeSyntaxIsValidated) {
	TempFile file("a.tex", kSource);

	const auto malformed = run_cli({
		"query", "--file", file.path(), "--query", "(section) @s", "--byte-range", "notarange" });
	EXPECT_EQ(malformed.exit_code, 1);
	EXPECT_TRUE(contains(malformed.err, "--byte-range expects"));

	const auto reversed = run_cli({
		"query", "--file", file.path(), "--query", "(section) @s", "--byte-range", "9:1" });
	EXPECT_EQ(reversed.exit_code, 1);
	EXPECT_TRUE(contains(reversed.err, "must not exceed"));
}

TEST(Cli, LimitCapsTheReportedMatches) {
	TempFile file("a.tex", kSource);
	const auto result = run_cli({
		"query", "--file", file.path(), "--query", kTitlePattern, "--limit", "1" });

	ASSERT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(result.out, "\"matched\":1"));
}

/* Regression guard: --capture used to bind a string_view to a temporary, so
   every filtered query silently matched nothing. */
TEST(Cli, CaptureFilterSelectsTheRequestedName) {
	TempFile file("a.tex", kSource);

	const auto titles = run_cli({
		"query", "--file", file.path(), "--query", kTitlePattern, "--capture", "title" });
	EXPECT_EQ(titles.exit_code, 0);
	EXPECT_TRUE(contains(titles.out, "\"matched\":2"));

	const auto absent = run_cli({
		"query", "--file", file.path(), "--query", kTitlePattern, "--capture", "absent" });
	EXPECT_EQ(absent.exit_code, 2);
}

/* ------------------------------------------------------- selection rules */

TEST(Cli, AmbiguousSelectionIsRefusedAndWritesNothing) {
	TempFile file("a.tex", kSource);

	const auto result = run_cli({
		"replace", "--file", file.path(),
		"--query", kTitlePattern, "--capture", "title", "--text", "X" });

	EXPECT_EQ(result.exit_code, 1);
	EXPECT_TRUE(contains(result.err, "2 captures matched"));
	EXPECT_TRUE(contains(result.err, "--match N or --all"));
	EXPECT_EQ(file.read(), std::string(kSource));
}

TEST(Cli, OutOfRangeMatchIndexFails) {
	TempFile file("a.tex", kSource);

	const auto result = run_cli({
		"replace", "--file", file.path(), "--query", kTitlePattern, "--capture", "title",
		"--match", "9", "--text", "X" });

	EXPECT_EQ(result.exit_code, 1);
	EXPECT_TRUE(contains(result.err, "is out of range"));
	EXPECT_EQ(file.read(), std::string(kSource));
}

/* --------------------------------------------------- writing and safety */

TEST(Cli, EditWithoutADestinationIsAPreview) {
	TempFile file("a.tex", kSource);

	const auto result = run_cli({
		"replace", "--file", file.path(), "--query", kTitlePattern, "--capture", "title",
		"--match", "0", "--text", "Intro" });

	ASSERT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(result.out, "\"wrote\":null"));
	EXPECT_EQ(file.read(), std::string(kSource));
}

/* Regression guard: the report used to read the pre-edit source view after the
   buffer had been replaced, producing mojibake and wrong line numbers. */
TEST(Cli, EditReportDescribesTheBytesChanged) {
	TempFile file("a.tex", kSource);

	const auto result = run_cli({
		"replace", "--file", file.path(), "--query", kTitlePattern, "--capture", "title",
		"--match", "0", "--text", "Intro" });

	ASSERT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(result.out, "\"before\":\"Introduction\""));
	EXPECT_TRUE(contains(result.out, "\"after\":\"Intro\""));
	EXPECT_TRUE(contains(result.out, "\"line\":3"));
	EXPECT_TRUE(contains(result.out, "\"column\":10"));
}

TEST(Cli, InPlaceWriteUpdatesTheFile) {
	TempFile file("a.tex", kSource);

	const auto result = run_cli({
		"replace", "--file", file.path(), "--query", kTitlePattern, "--capture", "title",
		"--match", "0", "--text", "Intro", "--in-place" });

	ASSERT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(result.out, "\"wrote\":"));
	EXPECT_TRUE(contains(file.read(), "\\section{Intro}"));
	EXPECT_FALSE(contains(file.read(), "\\section{Introduction}"));
}

TEST(Cli, DryRunOverridesAnExplicitDestination) {
	TempFile file("a.tex", kSource);

	const auto result = run_cli({
		"replace", "--file", file.path(), "--query", kTitlePattern, "--capture", "title",
		"--match", "0", "--text", "CHANGED", "--in-place", "--dry-run" });

	ASSERT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(result.out, "\"wrote\":null"));
	EXPECT_EQ(file.read(), std::string(kSource));
}

TEST(Cli, OutputToAFileLeavesTheInputAlone) {
	TempFile file("a.tex", kSource);
	TempFile destination("out.tex", "");

	const auto result = run_cli({
		"replace", "--file", file.path(), "--query", kTitlePattern, "--capture", "title",
		"--match", "0", "--text", "Intro", "--output", destination.path() });

	ASSERT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(result.out, "\"wrote\":"));
	EXPECT_TRUE(contains(destination.read(), "\\section{Intro}"));
	EXPECT_EQ(file.read(), std::string(kSource));
}

TEST(Cli, OutputDashStreamsSourceInsteadOfJson) {
	TempFile file("a.tex", kSource);

	const auto result = run_cli({
		"replace", "--file", file.path(), "--query", kTitlePattern, "--capture", "title",
		"--match", "0", "--text", "Intro", "--output", "-" });

	ASSERT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(result.out, "\\section{Intro}"));
	EXPECT_FALSE(contains(result.out, "\"command\""));
	EXPECT_EQ(file.read(), std::string(kSource));
}

/* ------------------------------------------------------- the CRUD verbs */

TEST(Cli, EraseAllRemovesEveryLabel) {
	TempFile file("a.tex", kSource);

	const auto result = run_cli({
		"erase", "--file", file.path(), "--query", kLabelPattern, "--capture", "def",
		"--all", "--in-place" });

	ASSERT_EQ(result.exit_code, 0);
	EXPECT_FALSE(contains(file.read(), "sec:results"));
	EXPECT_TRUE(contains(file.read(), "\\end{document}"));
}

TEST(Cli, ReplaceAllRewritesEveryMatch) {
	TempFile file("a.tex", kSource);

	const auto result = run_cli({
		"replace", "--file", file.path(), "--query", kTitlePattern, "--capture", "title",
		"--all", "--text", "T", "--in-place" });

	ASSERT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(result.out, "\"selected\":2"));
	EXPECT_TRUE(contains(file.read(), "\\section{T}"));
	EXPECT_FALSE(contains(file.read(), "Introduction"));
}

TEST(Cli, InsertBeforeACapture) {
	TempFile file("a.tex", kSource);

	const auto result = run_cli({
		"insert", "--file", file.path(), "--query", kLabelPattern, "--capture", "def",
		"--text", "%% ", "--at", "before", "--in-place" });

	ASSERT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(file.read(), "%% \\label{sec:results}"));
}

TEST(Cli, InsertDefaultsToAfterTheCapture) {
	TempFile file("a.tex", kSource);

	const auto result = run_cli({
		"insert", "--file", file.path(), "--query", kLabelPattern, "--capture", "def",
		"--text", "%%", "--in-place" });

	ASSERT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(file.read(), "\\label{sec:results}%%"));
}

TEST(Cli, InsertRejectsAnUnknownAnchor) {
	TempFile file("a.tex", kSource);

	const auto result = run_cli({
		"insert", "--file", file.path(), "--query", kLabelPattern, "--capture", "def",
		"--text", "x", "--at", "sideways" });

	EXPECT_EQ(result.exit_code, 1);
	EXPECT_TRUE(contains(result.err, "--at expects"));
}

TEST(Cli, ReplaceWithoutTextIsAUsageError) {
	TempFile file("a.tex", kSource);

	const auto result = run_cli({
		"replace", "--file", file.path(), "--query", kLabelPattern, "--capture", "def" });

	EXPECT_EQ(result.exit_code, 1);
	EXPECT_TRUE(contains(result.err, "requires --text"));
}

/* ----------------------------------------------------------- check/tree */

TEST(Cli, CheckAcceptsAValidDocument) {
	TempFile file("a.tex", kSource);

	const auto result = run_cli({ "check", "--file", file.path() });

	EXPECT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(result.out, "\"parse_errors\":false"));
	EXPECT_TRUE(contains(result.out, "\"error_nodes\":0"));
}

TEST(Cli, CheckReportsBrokenDocumentsWithExitTwo) {
	TempFile file("broken.tex", "\\begin{document}\n");

	const auto result = run_cli({ "check", "--file", file.path() });

	EXPECT_EQ(result.exit_code, 2);
	EXPECT_TRUE(contains(result.out, "\"parse_errors\":true"));
}

TEST(Cli, TreePrintsAnSExpression) {
	TempFile file("a.tex", kSource);

	const auto result = run_cli({ "tree", "--file", file.path() });

	ASSERT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(result.out, "source_file"));
	EXPECT_TRUE(contains(result.out, "\"s_expression\""));
}

TEST(Cli, CheckAcceptsAnEmptyFile) {
	TempFile file("empty.tex", "");

	const auto result = run_cli({ "check", "--file", file.path() });

	EXPECT_EQ(result.exit_code, 0);
	EXPECT_TRUE(contains(result.out, "\"parse_errors\":false"));
}
