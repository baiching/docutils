#pragma once

/* Shared helpers for the docutils test suite.
 *
 * Tests run entirely in-process: the CLI is exercised by calling
 * Ltx::Cli::run() with constructed arguments and capturing the streams. That
 * keeps the suite fast and avoids spawning subprocesses. */

#include "latex/ltx_cli.hpp"

#include <tree_sitter/api.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

/* Provided by the vendored tree-sitter-latex grammar. */
extern "C" const TSLanguage* tree_sitter_latex();

namespace ltx_test {

	inline const TSLanguage* latex() { return tree_sitter_latex(); }

	/* A parsed tree plus the source it came from, for tests that drive
	   Ltx::Query directly rather than through a Document. */
	class ParsedSource
	{
	public:
		explicit ParsedSource(std::string source) : m_source(std::move(source)) {
			m_parser = ts_parser_new();
			if (!m_parser) return;

			if (!ts_parser_set_language(m_parser, latex())) {
				ts_parser_delete(m_parser);
				m_parser = nullptr;
				return;
			}

			m_tree = ts_parser_parse_string(
				m_parser, nullptr, m_source.data(),
				static_cast<uint32_t>(m_source.size()));
		}

		~ParsedSource() {
			if (m_tree) ts_tree_delete(m_tree);
			if (m_parser) ts_parser_delete(m_parser);
		}

		ParsedSource(const ParsedSource&) = delete;
		ParsedSource& operator=(const ParsedSource&) = delete;

		bool valid() const { return m_tree != nullptr; }
		TSNode root() const { return m_tree ? ts_tree_root_node(m_tree) : TSNode{}; }
		std::string_view source() const { return m_source; }

	private:
		std::string m_source;
		TSParser* m_parser{ nullptr };
		TSTree* m_tree{ nullptr };
	};

	/* A scratch file that removes itself, so each test starts from a known
	   state and leaves nothing behind in the temp directory. */
	class TempFile
	{
	public:
		TempFile(const std::string& name, const std::string& contents) {
			static uint32_t counter = 0;
			const std::filesystem::path dir = std::filesystem::temp_directory_path();
			m_path = (dir / ("docutils_test_" + std::to_string(++counter) + "_" + name)).string();
			write(contents);
		}

		~TempFile() {
			std::error_code ignored;
			std::filesystem::remove(m_path, ignored);
		}

		TempFile(const TempFile&) = delete;
		TempFile& operator=(const TempFile&) = delete;

		const std::string& path() const { return m_path; }

		void write(const std::string& contents) const {
			std::ofstream out(m_path, std::ios::binary | std::ios::trunc);
			out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		}

		std::string read() const {
			std::ifstream in(m_path, std::ios::binary);
			std::ostringstream buffer;
			buffer << in.rdbuf();
			return buffer.str();
		}

	private:
		std::string m_path;
	};

	/* Restores a stream buffer even when the guarded call throws. */
	class StreamSwap
	{
	public:
		StreamSwap(std::ostream& stream, std::streambuf* replacement)
			: m_stream(stream), m_original(stream.rdbuf()) {
			m_stream.rdbuf(replacement);
		}

		~StreamSwap() { m_stream.rdbuf(m_original); }

		StreamSwap(const StreamSwap&) = delete;
		StreamSwap& operator=(const StreamSwap&) = delete;

	private:
		std::ostream& m_stream;
		std::streambuf* m_original;
	};

	struct CliResult
	{
		int exit_code{ 0 };
		std::string out;
		std::string err;
	};

	/* Runs the CLI in-process with `args` following the program name. */
	inline CliResult run_cli(std::vector<std::string> args) {
		std::vector<std::string> storage;
		storage.reserve(args.size() + 1);
		storage.emplace_back("docutils");
		for (std::string& arg : args) storage.push_back(std::move(arg));

		std::vector<char*> argv;
		argv.reserve(storage.size());
		for (std::string& arg : storage) argv.push_back(arg.data());

		std::ostringstream out;
		std::ostringstream err;
		CliResult result;

		{
			StreamSwap swap_out(std::cout, out.rdbuf());
			StreamSwap swap_err(std::cerr, err.rdbuf());
			result.exit_code = Ltx::Cli::run(static_cast<int>(argv.size()), argv.data());
		}

		result.out = out.str();
		result.err = err.str();
		return result;
	}

	inline bool contains(const std::string& haystack, const std::string& needle) {
		return haystack.find(needle) != std::string::npos;
	}

} // namespace ltx_test
