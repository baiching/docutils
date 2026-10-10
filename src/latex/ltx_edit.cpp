#include "../includes/latex/ltx_edit.hpp"

#include <algorithm>

namespace Ltx {

	Edit Buffer::make_insert(uint32_t at, std::string_view text) {
		Edit edit;
		edit.range = ByteRange{ at, at };
		edit.text.assign(text);
		edit.type = EditType::Create;

		return edit;
	}

	Edit Buffer::make_replace(ByteRange range, std::string_view text) {
		Edit edit;
		edit.range = range;
		edit.text.assign(text);
		edit.type = EditType::Update;

		return edit;
	}

	Edit Buffer::make_remove(ByteRange range) {
		Edit edit;
		edit.range = range;
		edit.type = EditType::Delete;
		return edit;
	}

	TSPoint Buffer::point_at(uint32_t byte) const {
		if (byte > size()) byte = size();

		uint32_t row = 0;
		uint32_t line_start = 0;

		for (uint32_t i = 0; i < byte; ++i) {
			if (m_text[i] == '\n') {
				++row;
				line_start = i + 1;
			}
		}

		return TSPoint{ row, byte - line_start };
	}

	uint32_t Buffer::line_at(uint32_t byte) const {
		return point_at(byte).row;
	}

	uint32_t Buffer::column_at(uint32_t byte) const {
		return point_at(byte).column;
	}

	/* -------------------------------------------------------------- private */

	EditStatus Buffer::check(ByteRange range) const {
		/* Reversed is invalid rather than empty, matching ByteRange::empty(). */
		if (range.start > range.end) return EditStatus::OutOfBounds;
		if (range.end > size()) return EditStatus::OutOfBounds;
		return EditStatus::Ok;
	}

	EditStatus Buffer::record(const TSInputEdit& change) {
		m_last_change = change;
		++m_revision;
		return EditStatus::Ok;
	}

	/* -------------------------------------------------------------- editing */

	EditStatus Buffer::insert(uint32_t at, std::string_view text) {
		return apply(make_insert(at, text));
	}

	EditStatus Buffer::replace(ByteRange range, std::string_view text) {
		return apply(make_replace(range, text));
	}

	EditStatus Buffer::erase(ByteRange range) {
		return apply(make_remove(range));
	}

	EditStatus Buffer::apply(Edit edit) {
		return apply_all(std::vector<Edit>{ std::move(edit) });
	}

	EditStatus Buffer::apply_all(std::vector<Edit> edits) {
		if (edits.empty()) return EditStatus::EmptyBatch;

		const uint32_t old_len = size();

		/* Deterministic order, independent of how the caller supplied the batch.
		 *
		 * Ascending start puts the edits back to front. The tie-break on end
		 * matters: for two edits at the same offset, the zero-length one must
		 * sort FIRST, because the walk below runs in reverse and a zero-length
		 * insert has to be applied last to land outside the replacement rather
		 * than inside it. Getting this backwards lets the insert's bytes be
		 * overwritten by the very edit it was anchored to.
		 *
		 * stable_sort keeps caller order among genuinely equal ranges, so two
		 * inserts at one offset appear in the order they were given. */
		std::stable_sort(edits.begin(), edits.end(),
			[](const Edit& a, const Edit& b) {
				if (a.range.start != b.range.start) return a.range.start < b.range.start;
				return a.range.end < b.range.end;
			});

		/* Validate the whole batch before writing anything: a rejected call must
		 * leave the buffer byte-identical.
		 *
		 * Checking each edit only against its predecessor is sufficient. Sorted by
		 * start, if no adjacent pair shares a byte then no pair shares a byte,
		 * because s[j] >= s[i+1] >= e[i] for every j > i. */
		for (size_t i = 0; i < edits.size(); ++i) {
			const EditStatus bounds = check(edits[i].range);
			if (bounds != EditStatus::Ok) return bounds;

			/* Touching is allowed; sharing a byte is not. A zero-length range has
			 * end == start, so it never overlaps anything. */
			if (i > 0 && edits[i].range.start < edits[i - 1].range.end) {
				return EditStatus::Overlapping;
			}
		}

		/* One spanning edit covers the batch, because ts_tree_edit takes exactly
		 * one. Take the far end as a maximum rather than the last edit's end: a
		 * trailing zero-length insert can start later than any real edit ends. */
		const uint32_t span_start = edits.front().range.start;

		uint32_t span_old_end = 0;
		for (const Edit& edit : edits) {
			span_old_end = std::max(span_old_end, edit.range.end);
		}

		/* Points against the pre-edit text, while it is still intact. */
		const TSPoint start_point = point_at(span_start);
		const TSPoint old_end_point = point_at(span_old_end);

		/* Back to front, so each edit's offsets are still valid when it runs. */
		for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
			m_text.replace(it->range.start, it->range.length(), it->text);
		}

		/* Signed: a batch can shrink the buffer. The region [span_start,
		 * span_old_end) held span_old_end - span_start bytes and now holds that
		 * plus the net delta, which puts the new end at span_old_end + delta. */
		const int64_t delta =
			static_cast<int64_t>(size()) - static_cast<int64_t>(old_len);

		TSInputEdit change{};
		change.start_byte = span_start;
		change.old_end_byte = span_old_end;
		change.new_end_byte = static_cast<uint32_t>(
			static_cast<int64_t>(span_old_end) + delta);
		change.start_point = start_point;
		change.old_end_point = old_end_point;

		/* Against the post-edit text, which point_at clamps if the arithmetic
		 * ever lands outside it. */
		change.new_end_point = point_at(change.new_end_byte);

		return record(change);
	}

}
