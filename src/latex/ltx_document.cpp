#include "ltx_document.hpp"

#include <cstdlib>
#include <utility>

namespace Ltx {

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

	std::vector<Match> Document::matches(const Query& query, const MatchOptions& options) const {
		if (!m_tree) return {};
		return query.matches(root(), m_source, options);
	}

	std::vector<Capture> Document::captures(const Query& query, const MatchOptions& options) const {
		if (!m_tree) return {};
		return query.captures(root(), m_source, options);
	}

	std::optional<Capture> Document::find_first(const Query& query, std::string_view capture_name) const {
		MatchOptions options;
		options.capture = capture_name;
		options.limit = 1;

		std::vector<Capture> found = captures(query, options);
		if (found.empty()) return std::nullopt;

		return found.front();
	}

	bool Document::insert(uint32_t byte_offset, std::string_view text) {
		return apply(TextEdit::create(byte_offset, text));
	}

	bool Document::insert_before(const Capture& capture, std::string_view text) {
		return apply(TextEdit::create(capture.range.start, text));
	}

	bool Document::insert_after(const Capture& capture, std::string_view text) {
		return apply(TextEdit::create(capture.range.end, text));
	}

	bool Document::replace(const Capture& capture, std::string_view text) {
		return apply(TextEdit::update(capture.range, text));
	}

	bool Document::replace(const Match& match, std::string_view text) {
		return apply(TextEdit::update(match.range(), text));
	}

	bool Document::erase(const Capture& capture) {
		return apply(TextEdit::remove(capture.range));
	}

	bool Document::erase(const Match& match) {
		return apply(TextEdit::remove(match.range()));
	}

	bool Document::apply(const TextEdit& edit) {
		std::vector<TextEdit> single{ edit };
		return apply_all(std::move(single));
	}

	bool Document::apply_all(std::vector<TextEdit> edits) {
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
