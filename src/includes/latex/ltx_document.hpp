#pragma once

/* The query-layer entry point for reading *and* editing LaTeX.
 *
 * Everything a caller does goes through here:
 *   - READ   : captures() runs an Ltx::Query against the document
 *   - CREATE : insert() / insert_before() / insert_after()
 *   - UPDATE : replace()
 *   - DELETE : erase()
 *
 * The CRUD primitives in ltx_edit.hpp are an implementation detail that
 * this class consumes; callers are not expected to build them by hand.
 *
 * LIFETIME: a Document owns its source text and its syntax tree. A Capture it
 * hands out is plain data and stays readable afterwards, but the byte range it
 * points at is only meaningful for the revision it was produced from. Compare
 * revision() against a saved value to detect that.
 */

#include <tree_sitter/api.h>

#include "ltx_edit.hpp"
#include "ltx_query.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Ltx {

	class Document
	{
	public:
		/* Parses `source` immediately. Check is_valid() before use: it is false
		   when the language is null, the grammar is incompatible with the
		   linked tree-sitter, or the initial parse failed. */
		Document(std::string source, const TSLanguage* language);
		~Document();

		/* Owns a parser and a tree, so copying is not allowed. */
		Document(const Document&) = delete;
		Document& operator=(const Document&) = delete;

		Document(Document&& other) noexcept;
		Document& operator=(Document&& other) noexcept;

	public:
		/* ---------------------------------------------------------- context */

		bool is_valid() const { return m_parser != nullptr && m_tree != nullptr; }
		const TSLanguage* language() const { return m_language; }

		/* Current text; mirrors every committed edit. */
		std::string_view source() const { return m_source; }

		TSNode root() const;

		/* True when the last parse produced ERROR or MISSING nodes. An edit can
		   introduce a syntax error, so re-check this after mutating. */
		bool has_errors() const;

		/* S-expression of the current tree; empty when there is no tree. */
		std::string s_expression() const;

		/* Bumped once per committed edit. */
		uint32_t revision() const { return m_revision; }
		bool is_stale(uint32_t revision) const { return revision != m_revision; }

		/* ----------------------------------------------------------- READ */

		/* A read-only handle onto the current text and tree. Hand this to
		   Query::captures() to run a query against this document directly,
		   bypassing the convenience wrapper below. */
		SourceView view() const;

		std::vector<Capture> captures(const Query& query, const QueryOptions& options = {}) const;

		/* First capture named `capture_name`, or nullopt when nothing matched. */
		std::optional<Capture> find_first(const Query& query, std::string_view capture_name) const;

		/* ---------------------------------------------------------- CREATE */

		/* Insert without removing anything. `byte_offset` may equal the length. */
		bool insert(uint32_t byte_offset, std::string_view text);
		bool insert_before(const Capture& capture, std::string_view text);
		bool insert_after(const Capture& capture, std::string_view text);

		/* ---------------------------------------------------------- UPDATE */

		/* Replaces the captured node's own bytes, braces and all. */
		bool replace(const Capture& capture, std::string_view text);

		/* ---------------------------------------------------------- DELETE */

		bool erase(const Capture& capture);

		/* ------------------------------------------------------------- raw */

		/* Escape hatch for an edit that was not derived from a query. */
		bool apply(const Edit& edit);

		/* Batch form: collect every edit first, then commit them together.
		   This is the pattern to use when rewriting several matches, because
		   the edits are expressed against one revision and applied back to
		   front. Nothing is committed unless every edit is in bounds and they
		   do not overlap. An empty batch returns false and changes nothing. */
		bool apply_all(std::vector<Edit> edits);

	private:
		/* Frees the tree and parser. Safe to call more than once. */
		void destroy();

		/* Re-parses incrementally against m_tree, which the caller must already
		   have updated with ts_tree_edit. */
		void reparse();

	private:
		const TSLanguage* m_language{ nullptr };
		TSParser* m_parser{ nullptr };
		TSTree* m_tree{ nullptr };
		std::string m_source;
		uint32_t m_revision{ 0 };
	};
}
