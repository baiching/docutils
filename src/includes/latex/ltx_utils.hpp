#pragma once

/* This one will contain helper functions like printing entire tree, individual nodes and its elements */

#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace Ltx {

	/* Line/column for a byte offset, 0-based, derived straight from the text.
	   Needed wherever a TSInputEdit has to be filled in but there is no Buffer
	   to ask and no tree to walk. Offsets past the end clamp to the last byte. */
	TSPoint point_for_byte(std::string_view source, uint32_t byte_offset);

	class Utils
	{
	public:
		Utils(TSTree* tree, std::string_view source);
		~Utils() = default;

	public:
		void print_node(TSNode node, uint32_t depth = 0) const;
		void print_tree() const;

		TSNode find_pos(uint32_t byte_offset) const;
		TSNode find_pos(uint32_t line, uint32_t column) const;

	private:

		TSTree* m_tree{ nullptr };
		std::string_view m_source;

	};
}