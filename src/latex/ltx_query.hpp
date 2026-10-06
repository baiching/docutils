#pragma once

#include <tree_sitter/api.h>

/* ByteRange lives with the edit primitives, which is the layer a query result
   feeds into. Keeps the dependency one-way: query -> edit. */
#include "ltx_edit.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Ltx {

	/* captures a single match @cap_name (eg., @section_title) */
	struct Capture
	{
		std::string name;

		/* The capture's own id (index into the query's capture table), not the
		   position within the match. Use this with ts_query_capture_name_for_id. */
		uint32_t capture_id{ 0 };

		TSNode node;
		std::string_view text;

		/* Source range of `node`. This is the anchor the edit layer rewrites:
		   replacing [start, end) with new text is how a node gets edited. */
		ByteRange range{};

		/* Convenience: ts_node_type(node). Points into the grammar's static
		   storage, so it outlives the query and the tree. */
		const char* node_type() const { return ts_node_type(node); }
	};

	/* a full rule match containing a pattern index and all associated Capture items */
	struct Match {
		uint32_t pattern_index{ 0 };

		/* The pattern source that produced this match, so a caller running one
		   combined query can tell which of its patterns fired. */
		std::string_view pattern_source;

		std::vector<Capture> captures;

		/* Returns the first capture with the given name, or nullptr. */
		const Capture* find_capture(std::string_view name) const;

		/* Source range spanning every capture in this match. */
		ByteRange range() const;
	};

	/* Options controlling a single execution. Defaulted so the common call
	   site stays `q.matches(root, src)`. */
	struct MatchOptions
	{
		/* Only return matches belonging to this pattern id. Unset = all. */
		std::optional<uint32_t> pattern_index{};

		/* Only return captures with this name. A match with no such capture is
		   dropped entirely. Unset = all. */
		std::optional<std::string_view> capture{};

		/* Restrict execution to a byte range of the source. */
		std::optional<ByteRange> byte_range{};

		/* Max matches returned; 0 means unlimited. */
		uint32_t limit{ 0 };
	};

	/* Error detail for one pattern that failed to compile. */
	struct PatternError
	{
		uint32_t pattern_index{ 0 };
		std::string source;
		std::string message;
		uint32_t offset{ 0 };
	};

	class Query {
	public:
		/* The language is fixed for the lifetime of the Query; the patterns are
		   not. This is the whole point of the reshape: construct once per
		   language/parser, then add whatever queries you need. */
		explicit Query(const TSLanguage* language);
		~Query();

		/* Prevent unsafe copying(RAII for TSQuery* handle) */
		Query(const Query&) = delete;
		Query& operator=(const Query&) = delete;

		/* Enable move semantics */
		Query(Query&& other) noexcept;
		Query& operator=(Query&& other) noexcept;

	public:
		/* Pattern management.
		   Tree-sitter has no API to remove one pattern from a compiled TSQuery,
		   so every mutation rebuilds the TSQuery from the stored source list.
		   Rebuild cost is trivial next to a parse; correctness is worth it. */

		/* Appends a pattern source. Returns its pattern_index on success, or
		   std::nullopt if the source fails to compile. A failed add leaves the
		   query exactly as it was. */
		std::optional<uint32_t> add(std::string_view pattern_source);

		/* Appends several sources at once. Returns the number that compiled.
		   Sources that fail are recorded in pattern_errors(). */
		uint32_t add_many(const std::vector<std::string>& pattern_sources);

		/* Removes the pattern at pattern_index. Pattern ids are positional and
		   DO shift after a removal, so re-read pattern_index()/pattern_sources()
		   rather than caching ids across mutations. */
		bool remove(uint32_t pattern_index);

		/* Drops every pattern. The language is retained. */
		void clear();

		/* True when at least one pattern is compiled and ready to execute. */
		bool has_patterns() const { return m_query != nullptr; }

	public:
		/* Query Status.
		   is_valid() describes the query as a whole: a language is set and every
		   stored pattern compiled. An empty pattern list is valid, just empty -
		   has_patterns() is what tells you whether anything can match.
		   Per-pattern failures are reported separately by pattern_errors(), and a
		   rejected add/remove leaves a previously working query working. */
		bool is_valid() const {
			return m_language != nullptr && (m_patterns.empty() || m_query != nullptr);
		}
		std::string_view error_message() const { return m_error_msg; }
		uint32_t error_offset() const { return m_error_offset; }

		/* Every pattern that failed to compile during the most recent mutation.
		   A bad third pattern no longer kills the other four. */
		const std::vector<PatternError>& pattern_errors() const { return m_pattern_errors; }

		/* Query Metadata */
		uint32_t pattern_count() const;
		uint32_t capture_count() const;

		/* The stored pattern sources, in pattern_index order. */
		const std::vector<std::string>& pattern_sources() const { return m_patterns; }

		/* The compiled source for one pattern, or an empty view if out of range. */
		std::string_view pattern_source(uint32_t pattern_index) const;

		/* Resolves a capture's id to its name. Empty view if out of range. */
		std::string_view capture_name(uint32_t capture_id) const;

		/* True when the compiled query mentions @name at least once. Useful for
		   failing fast before executing a filter that can never match. */
		bool has_capture(std::string_view name) const;

		/* Query Execution. Both overloads take an optional filter so callers can
		   narrow results without recompiling a second query. */
		std::vector<Match> matches(TSNode root, std::string_view source) const;
		std::vector<Match> matches(TSNode root, std::string_view source, const MatchOptions& options) const;

		/* Returns all individual captures, in document order rather than grouped
		   by match. This is what the edit layer wants: it hands back the exact
		   nodes and byte ranges to rewrite. */
		std::vector<Capture> captures(TSNode root, std::string_view source) const;
		std::vector<Capture> captures(TSNode root, std::string_view source, const MatchOptions& options) const;

	private:
		/* Rebuilds m_query from m_patterns. Resets m_error_* to describe the
		   overall outcome and refills m_pattern_errors. */
		void rebuild();

		/* Builds a Capture from one TSQueryCapture. Shared by both exec paths. */
		Capture make_capture(const TSQueryCapture& cap, std::string_view source) const;

		/* True if this match satisfies the option filters. */
		bool match_passes(const Match& match, const MatchOptions& options) const;

	private:
		const TSLanguage* m_language{ nullptr };

		TSQuery* m_query{ nullptr };

		/* The stored pattern sources; the source of truth m_query is built from. */
		std::vector<std::string> m_patterns;

		std::vector<PatternError> m_pattern_errors;

		std::string m_error_msg;
		uint32_t m_error_offset{ 0 };
	};
}
