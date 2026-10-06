#include "ltx_query.hpp"

#include <algorithm>
#include <utility>

namespace Ltx {

	/* ---------------------------------------------------------------- Match */

	const Capture* Match::find_capture(std::string_view name) const {
		for (const Capture& cap : captures) {
			if (cap.name == name) return &cap;
		}
		return nullptr;
	}

	ByteRange Match::range() const {
		if (captures.empty()) return {};

		/* Captures are not guaranteed to arrive sorted, so fold over all of them
		   rather than assuming captures.front()/back() bound the match. */
		ByteRange out{ captures.front().range.start, captures.front().range.end };
		for (const Capture& cap : captures) {
			out.start = std::min(out.start, cap.range.start);
			out.end = std::max(out.end, cap.range.end);
		}
		return out;
	}

	/* ------------------------------------------------------------ Lifetime */

	Query::Query(const TSLanguage* language) : m_language(language) {
		if (!m_language) {
			m_error_msg = "Language pointer is null";
			return;
		}
		/* Nothing to compile yet; the query is simply empty until add() is used. */
	}

	Query::~Query() {
		if (m_query) {
			ts_query_delete(m_query);
			m_query = nullptr;
		}
	}

	/* enabling move semantics */
	Query::Query(Query&& other) noexcept
		: m_language(other.m_language),
		  m_query(other.m_query),
		  m_patterns(std::move(other.m_patterns)),
		  m_pattern_errors(std::move(other.m_pattern_errors)),
		  m_error_msg(std::move(other.m_error_msg)),
		  m_error_offset(other.m_error_offset)
	{
		other.m_query = nullptr;
		other.m_language = nullptr;
	}

	/* for operators */
	Query& Query::operator=(Query&& other) noexcept {
		if (this != &other) {
			if (m_query) ts_query_delete(m_query);

			m_language = other.m_language;
			m_query = other.m_query;
			m_patterns = std::move(other.m_patterns);
			m_pattern_errors = std::move(other.m_pattern_errors);
			m_error_msg = std::move(other.m_error_msg);
			m_error_offset = other.m_error_offset;

			other.m_query = nullptr;
			other.m_language = nullptr;
		}
		return *this;
	}

	/* ------------------------------------------------------------- Helpers */

	/* File-local: it is an implementation detail of compiling, and keeping it
	   out of the class lets the free compile_patterns() helper below use it
	   without needing access to Query's privates. */
	static std::string describe_query_error(TSQueryError error_type) {
		switch (error_type) {
		case TSQueryErrorSyntax:    return "Syntax error in query pattern.";
		case TSQueryErrorNodeType:  return "Invalid node type in query pattern.";
		case TSQueryErrorField:     return "Invalid field name in query pattern.";
		case TSQueryErrorCapture:   return "Invalid capture name in query pattern.";
		case TSQueryErrorStructure: return "Structure mismatch in query pattern.";
		case TSQueryErrorLanguage:  return "Language version mismatch in query pattern.";
		default:                    return "Unknown query compilation error.";
		}
	}

	/* Compiles the whole pattern list as one TSQuery. A multi-pattern source is
	   natively supported by tree-sitter: each top-level pattern gets its own
	   pattern_index, which is what lets one query serve many needs in one walk.

	   Returns nullptr and fills `errors` when ANY pattern fails. The caller
	   decides whether to keep the previous query. */
	static TSQuery* compile_patterns(
		const TSLanguage* language,
		const std::vector<std::string>& patterns,
		uint32_t& error_offset,
		std::string& error_msg,
		std::vector<PatternError>& errors)
	{
		errors.clear();
		error_offset = 0;
		error_msg.clear();

		if (!language || patterns.empty()) return nullptr;

		/* Join the sources with newlines. tree-sitter treats each top-level
		   parenthesised form as its own pattern, so this reproduces the exact
		   pattern_index order of the stored list. */
		std::string combined;
		for (const std::string& p : patterns) {
			combined += p;
			combined += '\n';
		}

		uint32_t offset = 0;
		TSQueryError error_type = TSQueryErrorNone;

		TSQuery* query = ts_query_new(
			language,
			combined.data(),
			static_cast<uint32_t>(combined.size()),
			&offset,
			&error_type
		);

		if (!query) {
			error_offset = offset;
			error_msg = describe_query_error(error_type);

			/* Attribute the failure to a specific pattern. tree-sitter reports a
			   byte offset into the combined source; walk the original sources to
			   find which one contains it. This is a best-effort diagnosis for the
			   caller's benefit and is never used for control flow. */
			uint32_t pattern_start = 0;   /* start of the blamed pattern in `combined` */
			uint32_t blamed = 0;
			for (uint32_t i = 0; i < patterns.size(); ++i) {
				const uint32_t span = static_cast<uint32_t>(patterns[i].size()) + 1; /* + newline */
				if (offset < pattern_start + span) { blamed = i; break; }
				pattern_start += span;
				blamed = i;
			}

			PatternError err;
			err.pattern_index = blamed;
			err.source = patterns[blamed];
			err.message = error_msg;
			/* Re-base the offset onto the blamed pattern so it points at the
			   offending byte within err.source. Guard the subtraction: if the
			   reported offset fell outside every source, keep it as-is rather
			   than letting unsigned arithmetic wrap around. */
			err.offset = offset >= pattern_start ? offset - pattern_start : offset;
			errors.push_back(std::move(err));

			return nullptr;
		}

		return query;
	}

	void Query::rebuild() {
		if (m_query) {
			ts_query_delete(m_query);
			m_query = nullptr;
		}

		m_error_msg.clear();
		m_error_offset = 0;
		m_pattern_errors.clear();

		if (!m_language) {
			m_error_msg = "Language pointer is null";
			return;
		}

		if (m_patterns.empty()) {
			/* An empty query is a legitimate state, not an error. */
			return;
		}

		m_query = compile_patterns(
			m_language, m_patterns, m_error_offset, m_error_msg, m_pattern_errors);
	}

	/* --------------------------------------------------- Pattern management */

	std::optional<uint32_t> Query::add(std::string_view pattern_source) {
		if (!m_language) {
			m_error_msg = "Language pointer is null";
			return std::nullopt;
		}

		/* Validate against a candidate list first so a bad pattern cannot
		   corrupt a query that was previously working. */
		std::vector<std::string> candidate = m_patterns;
		candidate.emplace_back(pattern_source);

		uint32_t candidate_offset = 0;
		std::string candidate_msg;
		std::vector<PatternError> candidate_errors;

		TSQuery* candidate_query = compile_patterns(
			m_language, candidate, candidate_offset, candidate_msg, candidate_errors);

		if (!candidate_query) {
			/* Leave m_patterns and m_query untouched; surface the failure. */
			m_error_msg = candidate_msg;
			m_error_offset = candidate_offset;
			m_pattern_errors = std::move(candidate_errors);
			return std::nullopt;
		}

		if (m_query) ts_query_delete(m_query);

		m_query = candidate_query;
		m_patterns = std::move(candidate);
		m_error_msg.clear();
		m_error_offset = 0;
		m_pattern_errors.clear();

		return static_cast<uint32_t>(m_patterns.size() - 1);
	}

	uint32_t Query::add_many(const std::vector<std::string>& pattern_sources) {
		if (!m_language) {
			m_error_msg = "Language pointer is null";
			return 0;
		}
		if (pattern_sources.empty()) return 0;

		/* Compile as one batch so the set is validated together. This mirrors
		   add()'s all-or-nothing behaviour, which keeps pattern ids stable. */
		std::vector<std::string> candidate = m_patterns;
		candidate.insert(candidate.end(), pattern_sources.begin(), pattern_sources.end());

		uint32_t candidate_offset = 0;
		std::string candidate_msg;
		std::vector<PatternError> candidate_errors;

		TSQuery* candidate_query = compile_patterns(
			m_language, candidate, candidate_offset, candidate_msg, candidate_errors);

		if (!candidate_query) {
			m_error_msg = candidate_msg;
			m_error_offset = candidate_offset;
			m_pattern_errors = std::move(candidate_errors);
			return 0;
		}

		if (m_query) ts_query_delete(m_query);

		m_query = candidate_query;
		m_patterns = std::move(candidate);
		m_error_msg.clear();
		m_error_offset = 0;
		m_pattern_errors.clear();

		return static_cast<uint32_t>(pattern_sources.size());
	}

	bool Query::remove(uint32_t pattern_index) {
		if (!m_language) {
			m_error_msg = "Language pointer is null";
			return false;
		}
		if (pattern_index >= m_patterns.size()) return false;

		std::vector<std::string> candidate = m_patterns;
		candidate.erase(candidate.begin() + pattern_index);

		uint32_t candidate_offset = 0;
		std::string candidate_msg;
		std::vector<PatternError> candidate_errors;

		TSQuery* candidate_query = nullptr;

		if (!candidate.empty()) {
			candidate_query = compile_patterns(
				m_language, candidate, candidate_offset, candidate_msg, candidate_errors);

			if (!candidate_query) {
				m_error_msg = candidate_msg;
				m_error_offset = candidate_offset;
				m_pattern_errors = std::move(candidate_errors);
				return false;
			}
		}

		if (m_query) ts_query_delete(m_query);

		m_query = candidate_query;      /* nullptr when we just removed the last one */
		m_patterns = std::move(candidate);
		m_error_msg.clear();
		m_error_offset = 0;
		m_pattern_errors.clear();

		return true;
	}

	void Query::clear() {
		if (m_query) {
			ts_query_delete(m_query);
			m_query = nullptr;
		}
		m_patterns.clear();
		m_pattern_errors.clear();
		m_error_msg.clear();
		m_error_offset = 0;
	}

	/* ----------------------------------------------------------- Metadata */

	uint32_t Query::pattern_count() const {
		return m_query ? ts_query_pattern_count(m_query) : 0;
	}

	uint32_t Query::capture_count() const {
		return m_query ? ts_query_capture_count(m_query) : 0;
	}

	std::string_view Query::pattern_source(uint32_t pattern_index) const {
		if (pattern_index >= m_patterns.size()) return {};
		return m_patterns[pattern_index];
	}

	std::string_view Query::capture_name(uint32_t capture_id) const {
		if (!m_query) return {};

		/* ts_query_capture_name_for_id asserts on an out-of-range id in debug
		   builds, so the bound is checked before calling into tree-sitter. */
		if (capture_id >= ts_query_capture_count(m_query)) return {};

		uint32_t name_len = 0;
		const char* name_ptr = ts_query_capture_name_for_id(m_query, capture_id, &name_len);
		if (!name_ptr) return {};

		return std::string_view(name_ptr, name_len);
	}

	bool Query::has_capture(std::string_view name) const {
		if (!m_query) return false;

		const uint32_t total = ts_query_capture_count(m_query);
		for (uint32_t id = 0; id < total; ++id) {
			if (capture_name(id) == name) return true;
		}
		return false;
	}

	/* ---------------------------------------------------------- Execution */

	Capture Query::make_capture(const TSQueryCapture& cap, std::string_view source) const {
		Capture out;
		out.capture_id = cap.index;
		out.node = cap.node;

		uint32_t name_len = 0;
		const char* name_ptr = ts_query_capture_name_for_id(m_query, cap.index, &name_len);
		if (name_ptr) out.name.assign(name_ptr, name_len);

		const uint32_t start = ts_node_start_byte(cap.node);
		const uint32_t end = ts_node_end_byte(cap.node);
		out.range = ByteRange{ start, end };

		/* Bounds-check against the caller's source before taking a view: a node
		   can legitimately outrun the buffer if the tree is stale after an edit. */
		if (end > start && end <= source.length()) {
			out.text = source.substr(start, end - start);
		} else {
			out.text = std::string_view{};
		}

		return out;
	}

	bool Query::match_passes(const Match& match, const MatchOptions& options) const {
		if (options.pattern_index && match.pattern_index != *options.pattern_index) {
			return false;
		}

		if (options.capture) {
			/* With a capture filter the caller asked for "only these captures",
			   so drop both non-matching matches and their other captures. */
			for (const Capture& cap : match.captures) {
				if (cap.name == *options.capture) return true;
			}
			return false;
		}

		return true;
	}

	std::vector<Match> Query::matches(TSNode root, std::string_view source) const {
		return matches(root, source, MatchOptions{});
	}

	std::vector<Match> Query::matches(TSNode root, std::string_view source, const MatchOptions& options) const {
		std::vector<Match> results;

		if (!m_query || ts_node_is_null(root)) return results;

		/* ts_query_cursor_new can fail under memory pressure; guard it rather
		   than passing nullptr into the tree-sitter API. */
		TSQueryCursor* cursor = ts_query_cursor_new();
		if (!cursor) return results;

		if (options.byte_range) {
			ts_query_cursor_set_byte_range(
				cursor, options.byte_range->start, options.byte_range->end);
		}

		ts_query_cursor_exec(cursor, m_query, root);

		TSQueryMatch match;

		while (ts_query_cursor_next_match(cursor, &match)) {
			Match current_match;
			current_match.pattern_index = match.pattern_index;
			current_match.pattern_source = pattern_source(match.pattern_index);

			for (uint16_t i = 0; i < match.capture_count; ++i) {
				current_match.captures.push_back(
					make_capture(match.captures[i], source));
			}

			if (!match_passes(current_match, options)) continue;

			/* Narrow the captures when the caller filtered by capture name. */
			if (options.capture) {
				std::vector<Capture> kept;
				for (Capture& cap : current_match.captures) {
					if (cap.name == *options.capture) kept.push_back(std::move(cap));
				}
				current_match.captures = std::move(kept);
			}

			results.push_back(std::move(current_match));

			if (options.limit != 0 && results.size() >= options.limit) break;
		}

		ts_query_cursor_delete(cursor);

		return results;
	}

	std::vector<Capture> Query::captures(TSNode root, std::string_view source) const {
		return captures(root, source, MatchOptions{});
	}

	std::vector<Capture> Query::captures(TSNode root, std::string_view source, const MatchOptions& options) const {
		std::vector<Capture> results;

		if (!m_query || ts_node_is_null(root)) return results;

		TSQueryCursor* cursor = ts_query_cursor_new();
		if (!cursor) return results;

		if (options.byte_range) {
			ts_query_cursor_set_byte_range(
				cursor, options.byte_range->start, options.byte_range->end);
		}

		ts_query_cursor_exec(cursor, m_query, root);

		TSQueryMatch match;
		uint32_t capture_index = 0;

		while (ts_query_cursor_next_capture(cursor, &match, &capture_index)) {
			const TSQueryCapture& cap = match.captures[capture_index];

			if (options.pattern_index && match.pattern_index != *options.pattern_index) {
				continue;
			}

			Capture out = make_capture(cap, source);

			if (options.capture && out.name != *options.capture) continue;

			results.push_back(std::move(out));

			if (options.limit != 0 && results.size() >= options.limit) break;
		}

		ts_query_cursor_delete(cursor);

		return results;
	}
}
