#pragma once

/* Layer 1 of the LaTeX editing stack: byte-range text edits.
 *
 * This is the CRUD primitive layer. It knows nothing about queries, matches or
 * the LaTeX grammar; it only moves bytes and produces the TSInputEdit that
 * tree-sitter needs to bring a tree up to date in place.
 *
 * Application code normally reaches this layer through Ltx::Document
 * (ltx_document.hpp), which is the query-layer entry point. Use these types
 * directly only when building edits outside a document.
 */

#include <tree_sitter/api.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Ltx {

	/* Half-open byte range [start, end). */
	struct ByteRange
	{
		uint32_t start{ 0 };
		uint32_t end{ 0 };

		uint32_t length() const { return end > start ? end - start : 0; }
		bool empty() const { return end <= start; }
	};

	/* The CRUD verb a TextEdit represents. Descriptive only: `range` plus
	   `text` fully determine what is applied. */
	enum class EditKind
	{
		Create,   /* insert text at a point (range is empty) */
		Update,   /* replace the bytes in range with text */
		Delete    /* remove the bytes in range (text stays empty) */
	};

	/* One text edit, expressed in byte offsets against the pre-edit source. */
	struct TextEdit
	{
		ByteRange range{};
		std::string text;
		EditKind kind{ EditKind::Update };

		static TextEdit create(uint32_t at, std::string_view text);
		static TextEdit update(ByteRange range, std::string_view text);
		static TextEdit remove(ByteRange range);
	};

	/* Converts a byte offset into the (row, column) pair tree-sitter uses.
	   Columns are byte offsets within the row, not codepoints. Offsets past the
	   end of `source` clamp to the end. */
	TSPoint point_for_byte(std::string_view source, uint32_t byte);

	/* Applies one edit to `source` in place.
	   Returns false, leaving `source` untouched, when the range is out of
	   bounds. On success `out_edit` describes the change for ts_tree_edit. */
	bool apply_edit(std::string& source, const TextEdit& edit, TSInputEdit& out_edit);

	/* Applies a batch of non-overlapping edits to `source` in place.
	   Edits may arrive in any order: they are validated, then applied from the
	   end of the buffer backwards so earlier offsets stay valid. Returns false,
	   leaving `source` untouched, if any range is out of bounds or two edits
	   overlap. On success `out_edit` is a single spanning edit covering every
	   change, which lets tree-sitter reuse the untouched parts of the tree. */
	bool apply_edits(std::string& source, std::vector<TextEdit> edits, TSInputEdit& out_edit);
}
