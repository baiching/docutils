#pragma once

/* ==========================================================================
 * Layer 2 — Query: compiled tree-sitter patterns, executed against a tree.
 *
 * INTERNAL. Users never see this. The driving layer above it exposes named
 * endpoints; this is the engine those endpoints are written against. `raw` is
 * the one endpoint that lets a caller reach here directly, and it does so by
 * name, not by including this header.
 *
 *
 * WHAT THIS DOES
 *   Compiles a set of patterns once. Runs them against a tree. Reports where
 *   they matched: name, node type, byte range, text. Nothing else.
 *
 *
 * TWO PROPERTIES, AND THEY ARE THE POINT
 *
 * 1. Immutable. No add / remove / clear. A Query is fixed at construction, so
 *    pattern_source() can never be invalidated by the Query itself. Rebuilding
 *    means building a new Query.
 *
 * 2. Captures own everything. No string_view into the source, no TSNode handle
 *    into the tree. A Capture is plain data that stays valid after the tree is
 *    reparsed and the buffer rewritten.
 *
 *    This is deliberate and it is what lets concurrency live above this layer.
 *    A transaction re-plans by running the same endpoint against a freshly
 *    parsed document; because nothing here borrows, there is no handle to
 *    dangle and no revision stamp to check. Borrowed results would force every
 *    caller to remember a staleness rule, and one of them would forget.
 *
 *
 * FAILURE
 *   Patterns compile as one batch: all succeed, or none do. A single bad
 *   pattern rejects the batch rather than being silently dropped, so a typo in
 *   a pattern cannot produce a quietly empty endpoint.
 *
 *   A Query that failed to compile is inert. is_valid() is false and
 *   captures() returns nothing. An endpoint holding one is a programming
 *   error and should fail loudly at registration, not degrade at runtime.
 *
 *
 * PERFORMANCE
 *   One TSQueryCursor is created per captures() call and destroyed on return.
 *   Repeated execution of the same pattern therefore recompiles nothing but
 *   does recreate the cursor; that is cheap next to a parse, and it keeps this
 *   class safe to construct per query rather than long-lived.
 * ========================================================================== */

#include <tree_sitter/api.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ltx_edit.hpp" /* ByteRange */

namespace Ltx {

	/* The read-only view of a document that queries run against. Endpoints
	 * receive this and never a Buffer, so they cannot mutate anything. */
	struct SourceView
	{
		TSNode root{};
		std::string_view text;

		bool valid() const { return !ts_node_is_null(root); }
	};

	/* Where one pattern matched. Plain data: copyable, printable, and valid
	 * for as long as you hold it. */
	struct Capture
	{
		std::string name;          /* the @capture name */
		std::string node_type;     /* ts_node_type, copied */
		uint32_t pattern_index{ 0 };

		ByteRange range{};
		std::string text;
	};

	/* Filters for one execution. Every field is optional; the default runs
	 * everything, in document order. */
	struct QueryOptions
	{
		/* Only captures from this pattern. */
		std::optional<uint32_t> pattern_index{};

		/* Only captures with this name. Owned, so a caller may pass a
		 * temporary without arranging for it to outlive the call. */
		std::optional<std::string> capture{};

		/* Restrict execution to this byte range of the text. */
		std::optional<ByteRange> byte_range{};

		/* Stop after N results. 0 means unlimited. */
		uint32_t limit{ 0 };
	};

	/* Why the batch failed to compile. All-or-nothing means this holds
	 * exactly one entry when non-empty. */
	struct PatternError
	{
		uint32_t pattern_index{ 0 };
		std::string source;
		std::string message;
		uint32_t offset{ 0 };
	};

	/* ------------------------------------------------------------------ query */

	class Query
	{
	public:
		/* Compiles every pattern as one batch. */
		Query(const TSLanguage* language, std::vector<std::string> patterns);
		~Query();

		Query(const Query&) = delete;
		Query& operator=(const Query&) = delete;
		Query(Query&&) noexcept;
		Query& operator=(Query&&) noexcept;

		/* ---------------------------------------------------------- status */

		bool is_valid() const { return m_query != nullptr; }

		/* Human-readable summary of the failure. Empty when valid. */
		std::string_view error_message() const { return m_error_message; }

		const std::vector<PatternError>& pattern_errors() const { return m_errors; }

		/* -------------------------------------------------------- metadata */

		uint32_t pattern_count() const { return static_cast<uint32_t>(m_patterns.size()); }
		uint32_t capture_count() const;
		std::string_view pattern_source(uint32_t index) const;

		/* ------------------------------------------------------- execution */

		/* Flat captures in document order. There is no grouped variant: an
		 * endpoint wants locations, not rule matches, and match grouping is
		 * only meaningful to someone reading the grammar. */
		std::vector<Capture> captures(SourceView view, const QueryOptions& options = {}) const;

	private:
		Capture make_capture(const TSQueryCapture& raw, SourceView view) const;

	private:
		const TSLanguage* m_language{ nullptr };
		TSQuery* m_query{ nullptr };

		/* Source of truth for the compiled query. Never modified after
		 * construction, which is what makes pattern_source() safe to return
		 * as a view. */
		std::vector<std::string> m_patterns;

		std::vector<PatternError> m_errors;
		std::string m_error_message;
	};

} // namespace Ltx