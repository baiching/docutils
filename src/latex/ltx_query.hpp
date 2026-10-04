#pragma once

#include <tree_sitter/api.h>

#include <string>
#include <vector>
#include <string_view>

namespace Ltx {
	/* captures a single match @cap_name (eg., @section_title) */
	struct Capture
	{
		std::string name;
		TSNode node;
		std::string_view text;
	};

	/* a full rule match containing a pattern index and all associated Capture items */
	struct Match {
		uint32_t pattern_index{ 0 };
		std::vector<Capture> captures;
	};

	class Query {
	public:
		Query(const TSLanguage* language, std::string_view pattern);
		~Query();

		/* Prevent unsafe copying(RAII for TSQuery* handle) */
		Query(const Query&) = delete;
		Query& operator=(const Query&) = delete;

		/* Enable move semantics */
		Query(Query&& other) noexcept;
		Query& operator=(Query&& other) noexcept;

	public:
		/* Query Status */ 
		bool is_valid() const { return m_query != nullptr; }
		std::string_view error_message() const { return m_error_msg; }
		uint32_t error_offset() const { return m_error_offset; }

		/* Query Metadata */
		uint32_t pattern_count() const;
		uint32_t capture_count() const;


		/* Query Execution */
		// Executes query against a root TSNode and returns all matching patterns
		std::vector<Match> matches(TSNode root, std::string_view source) const;

		// Executes query against a root TSNode and returns all individual captures
		std::vector<Capture> captures(TSNode root, std::string_view source) const;


	private:
		TSQuery* m_query{ nullptr };
		std::string m_error_msg;
		uint32_t m_error_offset{ 0 };
	};
}