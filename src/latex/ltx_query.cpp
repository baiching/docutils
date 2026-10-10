#include "../includes/latex/ltx_query.hpp"

#include <utility>

namespace Ltx {

	namespace {

		/* File-local: an implementation detail of compiling, kept out of the class
		   so it can be called from the constructor without touching privates. */
		std::string describe_query_error(TSQueryError error_type) {
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

		   Returns nullptr and fills `errors` when ANY pattern fails. A Query is
		   immutable, so there is no previously working query to preserve here: the
		   whole batch is either accepted or the Query stays inert. */
		TSQuery* compile_patterns(
			const TSLanguage* language,
			const std::vector<std::string>& patterns,
			std::vector<PatternError>& errors,
			std::string& error_message)
		{
			errors.clear();
			error_message.clear();

			if (!language) {
				error_message = "Language pointer is null";
				return nullptr;
			}

			if (patterns.empty()) return nullptr;

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

			if (query) return query;

			error_message = describe_query_error(error_type);

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
			err.message = error_message;
			/* Re-base the offset onto the blamed pattern so it points at the
			   offending byte within err.source. Guard the subtraction: if the
			   reported offset fell outside every source, keep it as-is rather
			   than letting unsigned arithmetic wrap around. */
			err.offset = offset >= pattern_start ? offset - pattern_start : offset;
			errors.push_back(std::move(err));

			return nullptr;
		}

	} // namespace

	/* ------------------------------------------------------------ Lifetime */

	/* Patterns are taken once and never changed afterwards: a Query is fixed at
	   construction, so pattern_source() can never be invalidated by the Query
	   itself. Rebuilding means building a new Query. */
	Query::Query(const TSLanguage* language, std::vector<std::string> patterns)
		: m_language(language), m_patterns(std::move(patterns))
	{
		m_query = compile_patterns(m_language, m_patterns, m_errors, m_error_message);
	}

	Query::~Query() {
		if (m_query) {
			ts_query_delete(m_query);
			m_query = nullptr;
		}
	}

	Query::Query(Query&& other) noexcept
		: m_language(other.m_language),
		  m_query(other.m_query),
		  m_patterns(std::move(other.m_patterns)),
		  m_errors(std::move(other.m_errors)),
		  m_error_message(std::move(other.m_error_message))
	{
		other.m_query = nullptr;
		other.m_language = nullptr;
	}

	Query& Query::operator=(Query&& other) noexcept {
		if (this != &other) {
			if (m_query) ts_query_delete(m_query);

			m_language = other.m_language;
			m_query = other.m_query;
			m_patterns = std::move(other.m_patterns);
			m_errors = std::move(other.m_errors);
			m_error_message = std::move(other.m_error_message);

			other.m_query = nullptr;
			other.m_language = nullptr;
		}
		return *this;
	}

	/* ----------------------------------------------------------- Metadata */

	/* pattern_count() is inline in the header: it reads m_patterns, which stays
	   valid whether or not compilation succeeded. */

	uint32_t Query::capture_count() const {
		return m_query ? ts_query_capture_count(m_query) : 0;
	}

	std::string_view Query::pattern_source(uint32_t index) const {
		if (index >= m_patterns.size()) return {};
		return m_patterns[index];
	}

	/* ---------------------------------------------------------- Execution */

	Capture Query::make_capture(const TSQueryCapture& raw, SourceView view) const {
		Capture out;

		uint32_t name_len = 0;
		const char* name_ptr = ts_query_capture_name_for_id(m_query, raw.index, &name_len);
		if (name_ptr) out.name.assign(name_ptr, name_len);

		/* ts_node_type returns a static string owned by the language, but Capture
		   owns everything it reports, so it is copied rather than borrowed. */
		const char* type_ptr = ts_node_type(raw.node);
		if (type_ptr) out.node_type.assign(type_ptr);

		const uint32_t start = ts_node_start_byte(raw.node);
		const uint32_t end = ts_node_end_byte(raw.node);
		out.range = ByteRange{ start, end };

		/* Bounds-check against the view's text before copying: a node can
		   legitimately outrun the buffer if the tree is stale after an edit. */
		if (end > start && end <= view.text.length()) {
			out.text.assign(view.text.substr(start, end - start));
		}

		return out;
	}

	std::vector<Capture> Query::captures(SourceView view, const QueryOptions& options) const {
		std::vector<Capture> results;

		if (!m_query || !view.valid()) return results;

		/* ts_query_cursor_new can fail under memory pressure; guard it rather
		   than passing nullptr into the tree-sitter API. */
		TSQueryCursor* cursor = ts_query_cursor_new();
		if (!cursor) return results;

		if (options.byte_range) {
			ts_query_cursor_set_byte_range(
				cursor, options.byte_range->start, options.byte_range->end);
		}

		ts_query_cursor_exec(cursor, m_query, view.root);

		TSQueryMatch match;
		uint32_t capture_index = 0;

		/* Flat captures in document order. There is no grouped variant: an
		   endpoint wants locations, not rule matches. */
		while (ts_query_cursor_next_capture(cursor, &match, &capture_index)) {
			if (options.pattern_index && match.pattern_index != *options.pattern_index) {
				continue;
			}

			Capture out = make_capture(match.captures[capture_index], view);
			out.pattern_index = match.pattern_index;

			if (options.capture && out.name != *options.capture) continue;

			results.push_back(std::move(out));

			if (options.limit != 0 && results.size() >= options.limit) break;
		}

		ts_query_cursor_delete(cursor);

		return results;
	}

} // namespace Ltx