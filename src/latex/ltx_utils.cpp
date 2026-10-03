#include "ltx_utils.hpp"

#include <string>
#include <iostream>
#include <string_view>
#include <tree_sitter/api.h>

namespace Ltx {

	Utils::Utils(TSTree* tree, std::string_view source) : m_tree(tree), m_source(source) {

	}

	void Utils::print_node(TSNode node, uint32_t depth) const {

		if (ts_node_is_null(node)) return;

		std::string indent(depth * 2, ' ');
		const char* type = ts_node_type(node);
		bool is_named = ts_node_is_named(node); /* To check if this particular node is named */

		uint32_t start_byte = ts_node_start_byte(node);
		uint32_t end_byte = ts_node_end_byte(node);
		uint32_t child_count = ts_node_child_count(node);

		std::cout << indent << (is_named ? "[Named] " : "[Anonymous] ") << type;

		if (child_count == 0 && end_byte > start_byte && end_byte <= m_source.length()) {
			std::string text(m_source.substr(start_byte, end_byte - start_byte));
			size_t pos = 0;

			while ((pos = text.find('\n', pos)) != std::string::npos)
			{
				text.replace(pos, 1, "\\n");
				pos += 2;
			}
			std::cout << " -> \"" << text << "\"";
		}
		std::cout << "\n";

		/* print for very child node */
		for (uint32_t i = 0; i < child_count; ++i) {
			TSNode child = ts_node_child(node, i);
			print_node(child, depth + 1);
		}
	}

	void Utils::print_tree() const {
		if (!m_tree) return; /* if its null ; return */
		TSNode root = ts_tree_root_node(m_tree);
		
		print_node(root);
	}

	TSNode Utils::find_pos(uint32_t byte_offset) const {
		if (!m_tree)
		{
			return {};
		}

		TSNode root = ts_tree_root_node(m_tree);
		return ts_node_descendant_for_byte_range(root, byte_offset, byte_offset);
	}

	TSNode Utils::find_pos(uint32_t line, uint32_t column) const {
		if (!m_tree) return {};

		TSNode root = ts_tree_root_node(m_tree);
		TSPoint point{ line, column };
		return ts_node_descendant_for_point_range(root, point, point);
	}
}