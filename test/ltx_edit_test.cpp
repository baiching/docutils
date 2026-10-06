/* Unit tests for the byte-range edit primitives.
 *
 * This layer is pure text manipulation: no grammar, no parser. It owns the
 * arithmetic that turns an edit into the TSInputEdit tree-sitter needs, so the
 * offset maths is tested here directly rather than through a document. */

#include <gtest/gtest.h>

#include "latex/ltx_edit.hpp"

#include <string>
#include <vector>

using namespace Ltx;

/* ------------------------------------------------------------- ByteRange */

TEST(ByteRange, LengthAndEmpty) {
	/* Each temporary is parenthesised: a brace-initialised expression contains a
	   comma, which the preprocessor would otherwise read as an extra argument. */
	EXPECT_EQ((ByteRange{ 0, 0 }.length()), 0u);
	EXPECT_TRUE((ByteRange{ 5, 5 }.empty()));

	EXPECT_EQ((ByteRange{ 5, 9 }.length()), 4u);
	EXPECT_FALSE((ByteRange{ 5, 9 }.empty()));

	/* A reversed range reports no length rather than a wrapped one. */
	EXPECT_EQ((ByteRange{ 9, 5 }.length()), 0u);
	EXPECT_TRUE((ByteRange{ 9, 5 }.empty()));
}

/* -------------------------------------------------------------- TextEdit */

TEST(TextEdit, CreateIsAZeroWidthRange) {
	const TextEdit edit = TextEdit::create(3, "abc");
	EXPECT_EQ(edit.kind, EditKind::Create);
	EXPECT_EQ(edit.range.start, 3u);
	EXPECT_EQ(edit.range.end, 3u);
	EXPECT_EQ(edit.text, "abc");
}

TEST(TextEdit, UpdateCarriesReplacementText) {
	const TextEdit edit = TextEdit::update(ByteRange{ 1, 4 }, "xyz");
	EXPECT_EQ(edit.kind, EditKind::Update);
	EXPECT_EQ(edit.range.start, 1u);
	EXPECT_EQ(edit.range.end, 4u);
	EXPECT_EQ(edit.text, "xyz");
}

TEST(TextEdit, RemoveHasNoText) {
	const TextEdit edit = TextEdit::remove(ByteRange{ 2, 6 });
	EXPECT_EQ(edit.kind, EditKind::Delete);
	EXPECT_EQ(edit.range.start, 2u);
	EXPECT_EQ(edit.range.end, 6u);
	EXPECT_TRUE(edit.text.empty());
}

/* --------------------------------------------------------- point_for_byte */

TEST(PointForByte, CountsRowsAndByteColumns) {
	/*           offsets: 0 1 2 3 4 5 6 7 */
	const std::string_view source = "ab\ncde\nf";

	EXPECT_EQ(point_for_byte(source, 0).row, 0u);
	EXPECT_EQ(point_for_byte(source, 0).column, 0u);

	EXPECT_EQ(point_for_byte(source, 2).row, 0u);
	EXPECT_EQ(point_for_byte(source, 2).column, 2u);

	/* Offset 3 is the first byte of the second row. */
	EXPECT_EQ(point_for_byte(source, 3).row, 1u);
	EXPECT_EQ(point_for_byte(source, 3).column, 0u);

	EXPECT_EQ(point_for_byte(source, 5).row, 1u);
	EXPECT_EQ(point_for_byte(source, 5).column, 2u);

	EXPECT_EQ(point_for_byte(source, 7).row, 2u);
	EXPECT_EQ(point_for_byte(source, 7).column, 0u);
}

TEST(PointForByte, ClampsBeyondEndOfSource) {
	const std::string_view source = "ab";

	EXPECT_EQ(point_for_byte(source, 99).row, 0u);
	EXPECT_EQ(point_for_byte(source, 99).column, 2u);
}

TEST(PointForByte, EmptySourceIsOrigin) {
	const std::string_view source = "";
	EXPECT_EQ(point_for_byte(source, 0).row, 0u);
	EXPECT_EQ(point_for_byte(source, 0).column, 0u);
}

/* ------------------------------------------------------------ apply_edit */

TEST(ApplyEdit, InsertAtStart) {
	std::string source = "hello";
	TSInputEdit edit{};

	ASSERT_TRUE(apply_edit(source, TextEdit::create(0, ">> "), edit));

	EXPECT_EQ(source, ">> hello");
	EXPECT_EQ(edit.start_byte, 0u);
	EXPECT_EQ(edit.old_end_byte, 0u);
	EXPECT_EQ(edit.new_end_byte, 3u);
	EXPECT_EQ(edit.start_point.row, 0u);
	EXPECT_EQ(edit.new_end_point.column, 3u);
}

TEST(ApplyEdit, InsertAtEnd) {
	std::string source = "hello";
	TSInputEdit edit{};

	ASSERT_TRUE(apply_edit(source, TextEdit::create(5, "!"), edit));

	EXPECT_EQ(source, "hello!");
	EXPECT_EQ(edit.start_byte, 5u);
	EXPECT_EQ(edit.new_end_byte, 6u);
}

TEST(ApplyEdit, ReplaceSameLength) {
	std::string source = "hello world";
	TSInputEdit edit{};

	ASSERT_TRUE(apply_edit(source, TextEdit::update(ByteRange{ 6, 11 }, "there"), edit));

	EXPECT_EQ(source, "hello there");
	EXPECT_EQ(edit.start_byte, 6u);
	EXPECT_EQ(edit.old_end_byte, 11u);
	EXPECT_EQ(edit.new_end_byte, 11u);
}

TEST(ApplyEdit, EraseShrinksBuffer) {
	std::string source = "hello world";
	TSInputEdit edit{};

	ASSERT_TRUE(apply_edit(source, TextEdit::remove(ByteRange{ 5, 11 }), edit));

	EXPECT_EQ(source, "hello");
	EXPECT_EQ(edit.start_byte, 5u);
	EXPECT_EQ(edit.old_end_byte, 11u);
	EXPECT_EQ(edit.new_end_byte, 5u);
}

TEST(ApplyEdit, ReportsPointsOnMultipleRows) {
	std::string source = "one\ntwo\nthree";
	TSInputEdit edit{};

	ASSERT_TRUE(apply_edit(source, TextEdit::update(ByteRange{ 4, 7 }, "TWO"), edit));

	EXPECT_EQ(source, "one\nTWO\nthree");
	EXPECT_EQ(edit.start_point.row, 1u);
	EXPECT_EQ(edit.start_point.column, 0u);
	EXPECT_EQ(edit.old_end_point.row, 1u);
	EXPECT_EQ(edit.old_end_point.column, 3u);
	EXPECT_EQ(edit.new_end_point.row, 1u);
	EXPECT_EQ(edit.new_end_point.column, 3u);
}

TEST(ApplyEdit, RejectsOutOfBoundsAndLeavesSourceUntouched) {
	std::string source = "hello";
	TSInputEdit edit{};

	EXPECT_FALSE(apply_edit(source, TextEdit::update(ByteRange{ 0, 99 }, "x"), edit));
	EXPECT_EQ(source, "hello");

	EXPECT_FALSE(apply_edit(source, TextEdit::create(99, "x"), edit));
	EXPECT_EQ(source, "hello");

	/* A reversed range is invalid, not an empty one. */
	EXPECT_FALSE(apply_edit(source, TextEdit::update(ByteRange{ 4, 2 }, "x"), edit));
	EXPECT_EQ(source, "hello");
}

/* ----------------------------------------------------------- apply_edits */

TEST(ApplyEdits, AppliesBatchRegardlessOfInputOrder) {
	std::string source = "aaa bbb ccc";
	TSInputEdit edit{};

	/* Deliberately out of order: the later region is listed first. */
	std::vector<TextEdit> edits{
		TextEdit::update(ByteRange{ 8, 11 }, "CCC"),
		TextEdit::update(ByteRange{ 0, 3 }, "AAA"),
	};

	ASSERT_TRUE(apply_edits(source, edits, edit));

	EXPECT_EQ(source, "AAA bbb CCC");
	EXPECT_EQ(edit.start_byte, 0u);
	EXPECT_EQ(edit.old_end_byte, 11u);
	EXPECT_EQ(edit.new_end_byte, 11u);
}

TEST(ApplyEdits, BatchThatChangesLengthAdjustsNewEnd) {
	std::string source = "0123456789";
	TSInputEdit edit{};

	std::vector<TextEdit> edits{
		TextEdit::remove(ByteRange{ 0, 2 }),         /* drop "01" */
		TextEdit::update(ByteRange{ 8, 10 }, "XYZ"), /* "89" -> "XYZ" */
	};

	ASSERT_TRUE(apply_edits(source, edits, edit));

	EXPECT_EQ(source, "234567XYZ");
	EXPECT_EQ(edit.old_end_byte, 10u);
	EXPECT_EQ(edit.new_end_byte, 9u); /* 10 + (9 - 10) */
}

TEST(ApplyEdits, AdjacentEditsAreNotAnOverlap) {
	std::string source = "abcdef";
	TSInputEdit edit{};

	std::vector<TextEdit> edits{
		TextEdit::update(ByteRange{ 0, 3 }, "X"),
		TextEdit::update(ByteRange{ 3, 6 }, "Y"),
	};

	ASSERT_TRUE(apply_edits(source, edits, edit));
	EXPECT_EQ(source, "XY");
}

TEST(ApplyEdits, RejectsOverlappingEditsAndLeavesSourceUntouched) {
	std::string source = "0123456789";
	TSInputEdit edit{};

	std::vector<TextEdit> edits{
		TextEdit::update(ByteRange{ 0, 5 }, "a"),
		TextEdit::update(ByteRange{ 3, 8 }, "b"),
	};

	EXPECT_FALSE(apply_edits(source, edits, edit));
	EXPECT_EQ(source, "0123456789");
}

TEST(ApplyEdits, RejectsAnyOutOfBoundsEdit) {
	std::string source = "abc";
	TSInputEdit edit{};

	std::vector<TextEdit> edits{
		TextEdit::update(ByteRange{ 0, 1 }, "x"),
		TextEdit::update(ByteRange{ 1, 99 }, "y"),
	};

	EXPECT_FALSE(apply_edits(source, edits, edit));
	EXPECT_EQ(source, "abc");
}

TEST(ApplyEdits, EmptyBatchIsRejected) {
	std::string source = "abc";
	TSInputEdit edit{};

	EXPECT_FALSE(apply_edits(source, std::vector<TextEdit>{}, edit));
	EXPECT_EQ(source, "abc");
}

TEST(ApplyEdits, SingleEditMatchesApplyEdit) {
	std::string batch_source = "hello world";
	std::string single_source = "hello world";

	TSInputEdit batch_edit{};
	TSInputEdit single_edit{};

	ASSERT_TRUE(apply_edits(batch_source, { TextEdit::update(ByteRange{ 6, 11 }, "there") }, batch_edit));
	ASSERT_TRUE(apply_edit(single_source, TextEdit::update(ByteRange{ 6, 11 }, "there"), single_edit));

	EXPECT_EQ(batch_source, single_source);
	EXPECT_EQ(batch_edit.start_byte, single_edit.start_byte);
	EXPECT_EQ(batch_edit.old_end_byte, single_edit.old_end_byte);
	EXPECT_EQ(batch_edit.new_end_byte, single_edit.new_end_byte);
}
