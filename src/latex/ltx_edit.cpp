#include "ltx_edit.hpp"

#include <algorithm>

namespace Ltx {

	TextEdit TextEdit::create(uint32_t at, std::string_view text) {
		TextEdit edit;
		edit.range = ByteRange{ at, at };
		edit.text.assign(text);
		edit.kind = EditKind::Create;
		return edit;
	}

	TextEdit TextEdit::update(ByteRange range, std::string_view text) {
		TextEdit edit;
		edit.range = range;
		edit.text.assign(text);
		edit.kind = EditKind::Update;
		return edit;
	}

	TextEdit TextEdit::remove(ByteRange range) {
		TextEdit edit;
		edit.range = range;
		edit.kind = EditKind::Delete;
		return edit;
	}

	TSPoint point_for_byte(std::string_view source, uint32_t byte) {
		if (byte > source.size()) byte = static_cast<uint32_t>(source.size());

		uint32_t row = 0;
		uint32_t line_start = 0;

		for (uint32_t i = 0; i < byte; ++i) {
			if (source[i] == '\n') {
				++row;
				line_start = i + 1;
			}
		}

		return TSPoint{ row, byte - line_start };
	}

	bool apply_edits(std::string& source, std::vector<TextEdit> edits, TSInputEdit& out_edit) {
		if (edits.empty()) return false;

		const uint32_t old_len = static_cast<uint32_t>(source.size());

		/* Sort by start offset so overlap detection is a single pass. Stable so
		   that edits sharing a start offset keep their relative order. */
		std::stable_sort(edits.begin(), edits.end(),
			[](const TextEdit& a, const TextEdit& b) { return a.range.start < b.range.start; });

		uint32_t prev_end = 0;
		for (size_t i = 0; i < edits.size(); ++i) {
			const ByteRange range = edits[i].range;

			/* Reject a reversed range or one past the end of the buffer. */
			if (range.start > range.end || range.end > old_len) return false;

			/* Reject overlap. Touching boundaries are fine, so a zero-length
			   insert may sit at the edge of a replacement. */
			if (i > 0 && range.start < prev_end) return false;

			prev_end = range.end;
		}

		/* The batch is applied as one span: from the first change to the last.
		   Every byte outside that span is untouched, so tree-sitter can reuse
		   the corresponding subtrees. Points are captured against the pre-edit
		   text, which is still intact at this stage. */
		const uint32_t span_start = edits.front().range.start;
		const uint32_t span_old_end = edits.back().range.end;

		const TSPoint start_point = point_for_byte(source, span_start);
		const TSPoint old_end_point = point_for_byte(source, span_old_end);

		/* Apply back to front so the offsets of the edits still pending stay
		   valid as earlier regions are rewritten. */
		for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
			source.replace(it->range.start, it->range.length(), it->text);
		}

		const uint32_t new_len = static_cast<uint32_t>(source.size());

		/* Signed delta: the batch can shrink the buffer. */
		const int64_t delta = static_cast<int64_t>(new_len) - static_cast<int64_t>(old_len);
		const uint32_t new_end_byte =
			static_cast<uint32_t>(static_cast<int64_t>(span_old_end) + delta);

		out_edit.start_byte = span_start;
		out_edit.old_end_byte = span_old_end;
		out_edit.new_end_byte = new_end_byte;
		out_edit.start_point = start_point;
		out_edit.old_end_point = old_end_point;
		out_edit.new_end_point = point_for_byte(source, new_end_byte);

		return true;
	}

	bool apply_edit(std::string& source, const TextEdit& edit, TSInputEdit& out_edit) {
		std::vector<TextEdit> single{ edit };
		return apply_edits(source, std::move(single), out_edit);
	}
}
