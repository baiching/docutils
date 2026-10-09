#pragma once

/* ==========================================================================
 * Layer 1 — TextBuffer: an owned, editable byte buffer.
 *
 * The primitive layer. Knows nothing about queries, matches or the LaTeX
 * grammar: it moves bytes and produces the TSInputEdit that tree-sitter needs
 * to bring a tree up to date.
 *
 * Plain structs describe *what* to change. TextBuffer is the only thing that
 * changes it. Application code normally reaches this layer through
 * Ltx::Document, which is the query-layer entry point.
 *
 * CONTRACT
 *   - Bounds checks are internal. No caller can hand in an edit pointing
 *     outside the buffer.
 *   - All-or-nothing. A rejected operation leaves text() and revision()
 *     exactly as they were.
 *   - revision() bumps by one per accepted change.
 *   - Geometry is 0-based, like TSPoint. The CLI adds 1 for display.
 *   - Offsets are bytes, not codepoints.
 *   - Not thread-safe. Concurrent access is serialised one layer up.
 *
 * FAILURE
 *   Ok applied
 *   OutOfBounds  a range fell outside the buffer, or was reversed
 *   Overlapping  two edits in a batch shared a byte
 *   EmptyBatch   nothing to apply
 *
 * Touching is not overlapping: a zero-length insert may sit at either edge of
 * a replacement. Overlap is rejected rather than resolved, because the
 * intended result is not knowable at this layer.
 *
 * apply / apply_all report every change as one spanning TSInputEdit, because
 * ts_tree_edit accepts exactly one. Bytes inside that span are re-lexed even
 * if untouched.
 * ========================================================================== */

#include <tree_sitter/api.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Ltx {

	struct ByteRange
	{
		uint32_t start{ 0 };
		uint32_t end{ 0 };

		uint32_t length() const { return end > start ? end - start : 0; }
		bool empty() const { return end <= start; }
	};

	enum class EditType
	{
		Create,
		Update,
		Delete
	};

	struct Edit
	{
		ByteRange range{};
		std::string text;
		EditType type{ EditType::Update };
	};

	enum class EditStatus
	{
		Ok,
		OutOfBounds,
		Overlapping,
		EmptyBatch
	};

	/* ----------------------------------------------------------------- buffer */

	class Buffer
	{
	public:
		//explicit TextBuffer(std::string text = {});

		/* ------------------------------------------------------------- reading */

		std::string_view text() const { return m_text; }
		uint32_t size() const { return static_cast<uint32_t>(m_text.size()); }
		uint32_t revision() const { return m_revision; }

		/* ------------------------------------------------------------ geometry */

		/* 0-based, matching TSPoint. Offsets past the end clamp. */
		TSPoint point_at(uint32_t byte) const;
		uint32_t line_at(uint32_t byte) const;
		uint32_t column_at(uint32_t byte) const;

		/* ------------------------------------------------------------- editing */

		EditStatus insert(uint32_t at, std::string_view text);
		EditStatus replace(ByteRange range, std::string_view text);
		EditStatus erase(ByteRange range);

		EditStatus apply(Edit edit);
		EditStatus apply_all(std::vector<Edit> edits);

		/* The spanning edit from the last operation that returned Ok. Read it
		 * only on the success path:
		 *
		 *     if (buf.apply_all(edits) != EditStatus::Ok) return false;
		 *     ts_tree_edit(tree, &buf.last_change());
		 */
		const TSInputEdit& last_change() const { return m_last_change; }

		/* ------------------------------------------------------------ factories */

		static Edit make_insert(uint32_t at, std::string_view text);
		static Edit make_replace(ByteRange range, std::string_view text);
		static Edit make_remove(ByteRange range);

	private:
		EditStatus check(ByteRange range) const;
		EditStatus record(const TSInputEdit& change);

	private:
		std::string m_text;
		uint32_t m_revision{ 0 };
		TSInputEdit m_last_change{};
	};

} // namespace Ltx