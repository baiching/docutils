#include "ltx_query.hpp"
#include <stdexcept>

namespace Ltx {

	Query::Query(const TSLanguage* langugage, std::string_view pattern) {
		if (!langugage)
		{
			m_error_msg = "Language pointer is null";
			return;
		}

		uint32_t error_offset = 0;
		TSQueryError error_type = TSQueryErrorNone;

		m_query = ts_query_new(
			langugage,
			pattern.data(),
			static_cast<uint32_t>(pattern.length()),
			&error_offset,
			&error_type
		);

		/* errror handling */
		if (!m_query) {
			m_error_offset = error_offset;

			switch (error_type) {

			case TSQueryErrorSyntax:
				m_error_msg = "Syntax error in query pattern.";
				break;
			case TSQueryErrorNodeType:
				m_error_msg = "Invalid node type in query pattern.";
				break;
			case TSQueryErrorField:
				m_error_msg = "Invalid field name in query pattern.";
				break;
			case TSQueryErrorCapture:
				m_error_msg = "Invalid capture name in query pattern.";
				break;
			case TSQueryErrorStructure:
				m_error_msg = "Structure mismatch in query pattern.";
				break;
			default:
				m_error_msg = "Unknown query compilation error.";
				break;
			}
		}
	}

	Query::~Query() {
		if (m_query) {
			ts_query_delete(m_query);
			m_query = nullptr;
		}
	}

	/* enabling move sementics */
	Query::Query(Query&& other) noexcept
		: m_query(other.m_query), m_error_msg(std::move(other.m_error_msg)), m_error_offset(other.m_error_offset)
	{
		other.m_query = nullptr;
	}

	Query& Query::operator=(Query&& other) noexcept {
		if (this != &other) {
			if (m_query) ts_query_delete(m_query);
			m_query = other.m_query;
			m_error_msg = std::move(other.m_error_msg);
			m_error_offset = other.m_error_offset;
			other.m_query = nullptr;
		}
		return *this;
	}

	/* for operators */
	Query& Query::operator=(Query&& other) noexcept {
		if (this != &other) {
			if (m_query) ts_query_delete(m_query);
			m_query = other.m_query;
			m_error_msg = std::move(other.m_error_msg);
			m_error_offset = other.m_error_offset;
			other.m_query = nullptr;
		}
		return *this;
	}
}