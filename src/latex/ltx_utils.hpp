#pragma once

/* This one will contain helper functions like printing entire tree, individual nodes and its elements */

#include <string>
#include <string_view>
#include <tree_sitter/api.h>

namespace Ltx {
	class Utils
	{
	public:
		Utils(TSTree* tree, std::string_view source);
		~Utils() = default;

	public:
		void print_node(TSNode node, int depth = 0) const;
		void print_tree() const;

		TSNode find_pos(uint32_t byte_offset) const;
		TSNode find_pos(uint32_t line, uint32_t column) const;

	private:

		TSTree* m_tree{ nullptr };
		std::string_view m_source;

	};
}