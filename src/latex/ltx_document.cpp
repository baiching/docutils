#include "../includes/latex/ltx_document.hpp"

#include "../includes/latex/ltx_utils.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace Ltx {

	namespace {

		/* Mirrors Buffer::apply_all's contract, but against a caller-owned
		   string. Document keeps its own m_source rather than a Buffer, so the
		   batch is validated and applied here and the single spanning TSInputEdit
		   that ts_tree_edit needs is reported back through `out`.

		   Returns false without touching `text` when anything is out of bounds
		   or two edits share a byte. */
		bool apply_edits(std::string& text, std::vector<Edit> edits, TSInputEdit& out) {
			if (edits.empty()) return false;

			const uint32_t old_len = static_cast<uint32_t>(text.size());

			/* Deterministic order, independent of how the caller supplied the batch.
			 *
			 * Ascending start puts the edits back to front. The tie-break on end
			 * matters: for two edits at the same offset, the zero-length one must
			 * sort FIRST, because the walk below runs in reverse and a zero-length
			 * insert has to be applied last to land outside the replacement rather
			 * than inside it. */
			std::stable_sort(edits.begin(), edits.end(),
				[](const Edit& a, const Edit& b) {
					if (a.range.start != b.range.start) return a.range.start < b.range.start;
					return a.range.end < b.range.end;
				});

			/* Validate the whole batch before writing anything. Checking each edit
			 * only against its predecessor is sufficient: sorted by start, if no
			 * adjacent pair shares a byte then no pair shares a byte, because
			 * s[j] >= s[i+1] >= e[i] for every j > i. */
			for (size_t i = 0; i < edits.size(); ++i) {
				if (edits[i].range.start > edits[i].range.end) return false;
				if (edits[i].range.end > old_len) return false;
				if (i > 0 && edits[i].range.start < edits[i - 1].range.end) return false;
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
			const TSPoint start_point = point_for_byte(text, span_start);
			const TSPoint old_end_point = point_for_byte(text, span_old_end);

			/* Back to front, so each edit's offsets are still valid when it runs. */
			for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
				text.replace(it->range.start, it->range.length(), it->text);
			}

			/* Signed: a batch can shrink the text. The region [span_start,
			 * span_old_end) held span_old_end - span_start bytes and now holds that
			 * plus the net delta, which puts the new end at span_old_end + delta. */
			const int64_t delta =
				static_cast<int64_t>(text.size()) - static_cast<int64_t>(old_len);

			out = TSInputEdit{};
			out.start_byte = span_start;
			out.old_end_byte = span_old_end;
			out.new_end_byte = static_cast<uint32_t>(
				static_cast<int64_t>(span_old_end) + delta);
			out.start_point = start_point;
			out.old_end_point = old_end_point;

			/* Against the post-edit text, which point_for_byte clamps if the
			 * arithmetic ever lands outside it. */
			out.new_end_point = point_for_byte(text, out.new_end_byte);

			return true;
		}

	} // namespace

	Document::Document(std::string source, const TSLanguage* language)
		: m_language(language), m_source(std::move(source))
	{
		if (!m_language) return;

		m_parser = ts_parser_new();
		if (!m_parser) return;

		/* ts_parser_set_language returns false rather than asserting when the
		   grammar's ABI version falls outside the range this tree-sitter
		   supports. Ignoring it is how a parser ends up silently unusable. */
		if (!ts_parser_set_language(m_parser, m_language)) {
			ts_parser_delete(m_parser);
			m_parser = nullptr;
			return;
		}

		m_tree = ts_parser_parse_string(
			m_parser,
			nullptr,
			m_source.data(),
			static_cast<uint32_t>(m_source.size()));
	}

	Document::~Document() {
		destroy();
	}

	void Document::destroy() {
		if (m_tree) {
			ts_tree_delete(m_tree);
			m_tree = nullptr;
		}
		if (m_parser) {
			ts_parser_delete(m_parser);
			m_parser = nullptr;
		}
	}

	Document::Document(Document&& other) noexcept
		: m_language(other.m_language),
		  m_parser(other.m_parser),
		  m_tree(other.m_tree),
		  m_source(std::move(other.m_source)),
		  m_revision(other.m_revision)
	{
		other.m_language = nullptr;
		other.m_parser = nullptr;
		other.m_tree = nullptr;
	}

	Document& Document::operator=(Document&& other) noexcept {
		if (this != &other) {
			destroy();

			m_language = other.m_language;
			m_parser = other.m_parser;
			m_tree = other.m_tree;
			m_source = std::move(other.m_source);
			m_revision = other.m_revision;

			other.m_language = nullptr;
			other.m_parser = nullptr;
			other.m_tree = nullptr;
		}
		return *this;
	}

	TSNode Document::root() const {
		if (!m_tree) return TSNode{};
		return ts_tree_root_node(m_tree);
	}

	bool Document::has_errors() const {
		if (!m_tree) return true;
		return ts_node_has_error(root());
	}

	std::string Document::s_expression() const {
		if (!m_tree) return {};

		char* text = ts_node_string(root());
		if (!text) return {};

		std::string out(text);
		free(text); /* tree-sitter allocates this with malloc */
		return out;
	}

	SourceView Document::view() const {
		SourceView out;
		if (!m_tree) return out;
		out.root = ts_tree_root_node(m_tree);
		out.text = m_source;
		return out;
	}

	std::vector<Capture> Document::captures(const Query& query, const QueryOptions& options) const {
		if (!m_tree) return {};
		return query.captures(view(), options);
	}

	std::optional<Capture> Document::find_first(const Query& query, std::string_view capture_name) const {
		QueryOptions options;
		options.capture = std::string(capture_name);
		options.limit = 1;

		std::vector<Capture> found = captures(query, options);
		if (found.empty()) return std::nullopt;

		return found.front();
	}

	bool Document::insert(uint32_t byte_offset, std::string_view text) {
		return apply(Buffer::make_insert(byte_offset, text));
	}

	bool Document::insert_before(const Capture& capture, std::string_view text) {
		return apply(Buffer::make_insert(capture.range.start, text));
	}

	bool Document::insert_after(const Capture& capture, std::string_view text) {
		return apply(Buffer::make_insert(capture.range.end, text));
	}

	bool Document::replace(const Capture& capture, std::string_view text) {
		return apply(Buffer::make_replace(capture.range, text));
	}

	bool Document::erase(const Capture& capture) {
		return apply(Buffer::make_remove(capture.range));
	}

	bool Document::apply(const Edit& edit) {
		std::vector<Edit> single{ edit };
		return apply_all(std::move(single));
	}

	bool Document::apply_all(std::vector<Edit> edits) {
		if (!m_parser || edits.empty()) return false;

		/* Edit a copy first so a rejected batch leaves the document untouched,
		   revision included. */
		std::string candidate = m_source;
		TSInputEdit input_edit{};

		if (!apply_edits(candidate, std::move(edits), input_edit)) return false;

		m_source = std::move(candidate);

		/* Describe the change to the existing tree before re-parsing, so
		   tree-sitter can reuse the subtrees the edit did not touch. */
		if (m_tree) ts_tree_edit(m_tree, &input_edit);

		reparse();

		++m_revision;
		return true;
	}

	void Document::reparse() {
		if (!m_parser) return;

		TSTree* updated = ts_parser_parse_string(
			m_parser,
			m_tree,
			m_source.data(),
			static_cast<uint32_t>(m_source.size()));

		/* Release the old tree either way: it was edited in place already, so it
		   no longer corresponds to a parse of the original text. */
		if (m_tree) {
			ts_tree_delete(m_tree);
			m_tree = nullptr;
		}

		m_tree = updated; /* nullptr on failure; is_valid() then reports false */
	}
}
