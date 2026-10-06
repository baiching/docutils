/* Tests for Ltx::Document - the query layer entry point that owns the source,
 * the tree and the revision counter, and turns query results into CRUD. */

#include <gtest/gtest.h>

#include "latex/ltx_document.hpp"
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

	MatchOptions named(const char* capture) {
		MatchOptions options;
		options.capture = capture;
		return options;
	}

	std::vector<Capture> titles(const Document& doc, const Query& query) {
		return doc.captures(query, named("title"));
	}

} // namespace

/* ------------------------------------------------------------- lifecycle */

TEST(Document, ParsesAValidSource) {
	Document doc(kSource, ltx_test::latex());

	EXPECT_TRUE(doc.is_valid());
	EXPECT_FALSE(doc.has_errors());
	EXPECT_EQ(doc.source(), std::string(kSource));
	EXPECT_EQ(doc.revision(), 0u);
	EXPECT_EQ(std::string(ts_node_type(doc.root())), "source_file");
	EXPECT_FALSE(doc.s_expression().empty());
	EXPECT_EQ(doc.language(), ltx_test::latex());
}

TEST(Document, NullLanguageIsInvalid) {
	Document doc("x", nullptr);

	EXPECT_FALSE(doc.is_valid());
	EXPECT_TRUE(doc.has_errors());
	EXPECT_TRUE(doc.matches(Query(nullptr), {}).empty());
}

TEST(Document, EmptySourceParses) {
	Document doc("", ltx_test::latex());

	EXPECT_TRUE(doc.is_valid());
	EXPECT_FALSE(doc.has_errors());
	EXPECT_TRUE(doc.source().empty());
}

/* ------------------------------------------------------------------ read */

TEST(Document, CapturesAndFindFirst) {
	Document doc(kSource, ltx_test::latex());
	Query query(ltx_test::latex());
	ASSERT_TRUE(query.add(kTitlePattern).has_value());

	EXPECT_EQ(titles(doc, query).size(), 2u);

	const auto first = doc.find_first(query, "title");
	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(first->text, "Introduction");

	EXPECT_FALSE(doc.find_first(query, "missing").has_value());
}

TEST(Document, MatchesExposeWholeRules) {
	Document doc(kSource, ltx_test::latex());
	Query query(ltx_test::latex());
	ASSERT_TRUE(query.add(kLabelPattern).has_value());

	const std::vector<Match> matches = doc.matches(query);
	ASSERT_EQ(matches.size(), 1u);
	EXPECT_EQ(matches.front().pattern_index, 0u);
	EXPECT_EQ(matches.front().pattern_source, kLabelPattern);
}

/* ---------------------------------------------------------------- update */

TEST(Document, ReplaceUpdatesSourceAndRevision) {
	Document doc(kSource, ltx_test::latex());
	Query query(ltx_test::latex());
	ASSERT_TRUE(query.add(kTitlePattern).has_value());

	const auto title = doc.find_first(query, "title");
	ASSERT_TRUE(title.has_value());

	const uint32_t before = doc.revision();
	ASSERT_TRUE(doc.replace(*title, "Intro"));

	EXPECT_EQ(doc.revision(), before + 1);
	EXPECT_TRUE(doc.is_stale(before));
	EXPECT_FALSE(doc.is_stale(doc.revision()));

	EXPECT_TRUE(ltx_test::contains(std::string(doc.source()), "\\section{Intro}"));
	EXPECT_FALSE(doc.has_errors());
}

TEST(Document, ReplaceWholeMatchUsesTheSpanningRange) {
	Document doc(kSource, ltx_test::latex());
	Query query(ltx_test::latex());
	ASSERT_TRUE(query.add(kLabelPattern).has_value());

	const std::vector<Match> matches = doc.matches(query);
	ASSERT_EQ(matches.size(), 1u);

	ASSERT_TRUE(doc.replace(matches.front(), "\\label{new}"));
	EXPECT_TRUE(ltx_test::contains(std::string(doc.source()), "\\label{new}"));
	EXPECT_FALSE(ltx_test::contains(std::string(doc.source()), "sec:results"));
}

/* ---------------------------------------------------------------- delete */

TEST(Document, EraseRemovesTheCapturedBytes) {
	Document doc(kSource, ltx_test::latex());
	Query query(ltx_test::latex());
	ASSERT_TRUE(query.add(kLabelPattern).has_value());

	const auto definition = doc.find_first(query, "def");
	ASSERT_TRUE(definition.has_value());

	ASSERT_TRUE(doc.erase(*definition));
	EXPECT_FALSE(ltx_test::contains(std::string(doc.source()), "\\label{sec:results}"));
	EXPECT_FALSE(doc.has_errors());
}

/* ---------------------------------------------------------------- create */

TEST(Document, InsertBeforeAndAfterACapture) {
	Document doc(kSource, ltx_test::latex());
	Query query(ltx_test::latex());
	ASSERT_TRUE(query.add(kTitlePattern).has_value());

	auto title = doc.find_first(query, "title");
	ASSERT_TRUE(title.has_value());
	ASSERT_TRUE(doc.insert_before(*title, "<<"));
	EXPECT_TRUE(ltx_test::contains(std::string(doc.source()), "<<Introduction"));

	/* The previous capture belongs to the old revision, so re-query. */
	title = doc.find_first(query, "title");
	ASSERT_TRUE(title.has_value());
	ASSERT_TRUE(doc.insert_after(*title, ">>"));
	EXPECT_TRUE(ltx_test::contains(std::string(doc.source()), "<<Introduction>>"));
}

TEST(Document, InsertAtExplicitOffset) {
	Document doc("abc", ltx_test::latex());

	/* Offsets address the current revision, so the second insert sees "[abc". */
	ASSERT_TRUE(doc.insert(0, "["));
	ASSERT_TRUE(doc.insert(2, "]"));
	EXPECT_EQ(doc.source(), "[a]bc");
}

TEST(Document, InsertAtEndOfBufferIsAllowed) {
	Document doc("abc", ltx_test::latex());

	ASSERT_TRUE(doc.insert(3, "!"));
	EXPECT_EQ(doc.source(), "abc!");
}

/* ----------------------------------------------------------------- batch */

TEST(Document, ApplyAllCommitsSeveralEditsAsOneRevision) {
	Document doc(kSource, ltx_test::latex());
	Query query(ltx_test::latex());
	ASSERT_TRUE(query.add(kTitlePattern).has_value());

	const std::vector<Capture> found = titles(doc, query);
	ASSERT_EQ(found.size(), 2u);

	std::vector<TextEdit> edits{
		TextEdit::update(found[0].range, "First"),
		TextEdit::update(found[1].range, "Second"),
	};

	const uint32_t before = doc.revision();
	ASSERT_TRUE(doc.apply_all(std::move(edits)));

	EXPECT_EQ(doc.revision(), before + 1); /* one commit, not two */
	EXPECT_TRUE(ltx_test::contains(std::string(doc.source()), "\\section{First}"));
	EXPECT_TRUE(ltx_test::contains(std::string(doc.source()), "\\section{Second}"));
	EXPECT_FALSE(doc.has_errors());
}

TEST(Document, RejectedBatchLeavesTheDocumentUntouched) {
	Document doc(kSource, ltx_test::latex());

	const std::string original(doc.source());
	const uint32_t revision = doc.revision();

	std::vector<TextEdit> edits{
		TextEdit::update(ByteRange{ 0, 5 }, "x"),
		TextEdit::update(ByteRange{ 2, 9 }, "y"), /* overlaps the first */
	};

	EXPECT_FALSE(doc.apply_all(std::move(edits)));
	EXPECT_EQ(doc.source(), original);
	EXPECT_EQ(doc.revision(), revision);
}

TEST(Document, EmptyBatchChangesNothing) {
	Document doc(kSource, ltx_test::latex());

	EXPECT_FALSE(doc.apply_all({}));
	EXPECT_EQ(doc.source(), std::string(kSource));
	EXPECT_EQ(doc.revision(), 0u);
}

TEST(Document, OutOfBoundsEditIsRejected) {
	Document doc(kSource, ltx_test::latex());

	EXPECT_FALSE(doc.insert(9999, "x"));
	EXPECT_EQ(doc.source(), std::string(kSource));
	EXPECT_EQ(doc.revision(), 0u);
}

/* -------------------------------------------------------------- reparse */

TEST(Document, RepeatedEditsKeepTheTreeConsistent) {
	Document doc(kSource, ltx_test::latex());
	Query query(ltx_test::latex());
	ASSERT_TRUE(query.add(kTitlePattern).has_value());

	for (int i = 0; i < 3; ++i) {
		const auto title = doc.find_first(query, "title");
		ASSERT_TRUE(title.has_value()) << "iteration " << i;

		ASSERT_TRUE(doc.replace(*title, "T" + std::to_string(i)));
		EXPECT_FALSE(doc.has_errors()) << "reparse after edit " << i;
	}

	EXPECT_EQ(doc.revision(), 3u);
	EXPECT_TRUE(ltx_test::contains(std::string(doc.source()), "\\section{T2}"));
	EXPECT_EQ(titles(doc, query).size(), 2u);
}

TEST(Document, EditsAreReflectedInTheReparsedTree) {
	Document doc("\\section{One}\n", ltx_test::latex());

	Query query(ltx_test::latex());
	ASSERT_TRUE(query.add(kTitlePattern).has_value());

	const auto title = doc.find_first(query, "title");
	ASSERT_TRUE(title.has_value());
	ASSERT_TRUE(doc.replace(*title, "Two"));

	const auto updated = doc.find_first(query, "title");
	ASSERT_TRUE(updated.has_value());
	EXPECT_EQ(updated->text, "Two");
}

TEST(Document, AnEditCanIntroduceParseErrors) {
	Document doc("\\begin{document}\n\\end{document}\n", ltx_test::latex());
	ASSERT_FALSE(doc.has_errors());

	Query query(ltx_test::latex());
	ASSERT_TRUE(query.add("(begin) @b").has_value());

	const auto begin = doc.find_first(query, "b");
	ASSERT_TRUE(begin.has_value());

	/* Drop the closing brace of \begin{document}: the reparse must notice. */
	ASSERT_TRUE(doc.replace(*begin, "\\begin{document"));

	EXPECT_TRUE(doc.has_errors());
}

/* ------------------------------------------------------- move semantics */

TEST(Document, MoveTransfersOwnership) {
	Document doc(kSource, ltx_test::latex());
	ASSERT_TRUE(doc.is_valid());

	Document moved(std::move(doc));
	EXPECT_TRUE(moved.is_valid());
	EXPECT_EQ(moved.source(), std::string(kSource));

	Document assigned("x", ltx_test::latex());
	assigned = std::move(moved);
	EXPECT_TRUE(assigned.is_valid());
	EXPECT_EQ(assigned.source(), std::string(kSource));
}
