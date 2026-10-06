#include "ltx_cli.hpp"

#include "ltx_document.hpp"
#include "ltx_edit.hpp"
#include "ltx_query.hpp"

#include <tree_sitter/api.h>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

extern "C" const TSLanguage* tree_sitter_latex();

namespace Ltx {
	namespace Cli {
		namespace {

			constexpr int kOk = 0;
			constexpr int kError = 1;
			constexpr int kNoMatch = 2;

			constexpr const char* kTool = "docutils";
			constexpr const char* kVersion = "0.1.0";

			/* ------------------------------------------------------------ json */

			std::string json_escape(std::string_view in) {
				std::string out;
				out.reserve(in.size() + 8);

				for (const char ch : in) {
					switch (ch) {
					case '"':  out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n";  break;
					case '\r': out += "\\r";  break;
					case '\t': out += "\\t";  break;
					default:
						if (static_cast<unsigned char>(ch) < 0x20) {
							char buf[8];
							std::snprintf(buf, sizeof(buf), "\\u%04x",
								static_cast<unsigned>(static_cast<unsigned char>(ch)));
							out += buf;
						} else {
							out += ch;
						}
						break;
					}
				}
				return out;
			}

			std::string quoted(std::string_view s) {
				return "\"" + json_escape(s) + "\"";
			}

			/* --------------------------------------------------------- options */

			/* Flags that consume the following argument. Everything else that
			   starts with '-' is boolean, which keeps parsing unambiguous. */
			bool takes_value(const std::string& key) {
				return key == "--file" || key == "--query" || key == "--capture" ||
					key == "--text" || key == "--match" || key == "--limit" ||
					key == "--pattern-index" || key == "--byte-range" ||
					key == "--output" || key == "--format" || key == "--at";
			}

			/* Aliases are normalised so the rest of the code sees one spelling. */
			bool normalise(const std::string& arg, std::string& key) {
				if (arg == "-f") { key = "--file"; return true; }
				if (arg == "-q") { key = "--query"; return true; }
				if (arg == "-h") { key = "--help"; return true; }
				if (arg == "-V") { key = "--version"; return true; }
				return false;
			}

			bool is_known(const std::string& key) {
				if (takes_value(key)) return true;
				return key == "--in-place" || key == "--dry-run" || key == "--all" ||
					key == "--group" || key == "--help" || key == "--version";
			}

			struct Options
			{
				std::vector<std::pair<std::string, std::string>> items;
				std::vector<std::string> positional;

				bool has(const std::string& key) const {
					for (const auto& kv : items) if (kv.first == key) return true;
					return false;
				}

				std::string value(const std::string& key, const std::string& fallback = "") const {
					for (const auto& kv : items) if (kv.first == key) return kv.second;
					return fallback;
				}

				std::vector<std::string> values(const std::string& key) const {
					std::vector<std::string> out;
					for (const auto& kv : items) if (kv.first == key) out.push_back(kv.second);
					return out;
				}
			};

			bool parse_args(int argc, char** argv, Options& opts, std::string& err) {
				for (int i = 1; i < argc; ++i) {
					std::string arg = argv[i];

					if (arg.size() > 1 && arg[0] == '-') {
						std::string key;
						if (!normalise(arg, key)) key = arg;

						if (!is_known(key)) {
							err = "unknown option '" + arg + "'";
							return false;
						}

						if (takes_value(key)) {
							if (i + 1 >= argc) {
								err = "option '" + arg + "' requires a value";
								return false;
							}
							opts.items.emplace_back(key, argv[++i]);
						} else {
							opts.items.emplace_back(key, std::string());
						}
					} else {
						opts.positional.push_back(arg);
					}
				}
				return true;
			}

			bool is_text_format(const Options& opts) {
				return opts.value("--format", "json") == "text";
			}

			/* ------------------------------------------------------------ misc */

			bool parse_u32(std::string_view text, uint32_t& out) {
				if (text.empty()) return false;

				uint32_t value = 0;
				const char* first = text.data();
				const char* last = text.data() + text.size();

				const std::from_chars_result res = std::from_chars(first, last, value);
				if (res.ec != std::errc() || res.ptr != last) return false;

				out = value;
				return true;
			}

			void print_json(const std::string& body) {
				std::cout << body << "\n";
			}

			/* Errors go to stderr so stdout stays a clean data channel. */
			int fail(const std::string& code, const std::string& message, bool text_format) {
				if (text_format) {
					std::cerr << "error: " << code << ": " << message << "\n";
				} else {
					std::cerr << "{\"error\":{\"code\":" << quoted(code)
						<< ",\"message\":" << quoted(message) << "}}\n";
				}
				return kError;
			}

			int fail(const std::string& code, const std::string& message, const Options& opts) {
				return fail(code, message, is_text_format(opts));
			}

			bool read_file(const std::string& path, std::string& out, std::string& err) {
				/* Binary: the document is a byte buffer, and newline translation
				   would silently shift every offset a query reports. */
				std::ifstream in(path, std::ios::binary);
				if (!in) {
					err = "cannot open '" + path + "' for reading";
					return false;
				}

				std::ostringstream buffer;
				buffer << in.rdbuf();

				if (in.bad()) {
					err = "failed while reading '" + path + "'";
					return false;
				}

				out = buffer.str();
				return true;
			}

			bool write_file(const std::string& path, const std::string& data, std::string& err) {
				std::ofstream out(path, std::ios::binary | std::ios::trunc);
				if (!out) {
					err = "cannot open '" + path + "' for writing";
					return false;
				}

				out.write(data.data(), static_cast<std::streamsize>(data.size()));
				out.flush();

				if (!out) {
					err = "failed while writing '" + path + "'";
					return false;
				}
				return true;
			}

			/* The file is either --file or the first positional after the command. */
			bool resolve_file(const Options& opts, std::string& path, std::string& err) {
				if (opts.has("--file")) {
					path = opts.value("--file");
					return true;
				}
				if (opts.positional.size() >= 2) {
					path = opts.positional[1];
					return true;
				}
				err = "no input file; pass --file <path>";
				return false;
			}

			/* Line/column are reported 1-based, the way editors and agents count;
			   byte offsets stay 0-based and half-open. */
			uint32_t line_of(std::string_view source, uint32_t byte) {
				return Ltx::point_for_byte(source, byte).row + 1;
			}

			uint32_t column_of(std::string_view source, uint32_t byte) {
				return Ltx::point_for_byte(source, byte).column + 1;
			}

			std::string capture_json(const Capture& cap, std::string_view source) {
				std::ostringstream out;
				out << "{\"name\":" << quoted(cap.name)
					<< ",\"capture_id\":" << cap.capture_id
					<< ",\"node_type\":" << quoted(cap.node_type())
					<< ",\"start_byte\":" << cap.range.start
					<< ",\"end_byte\":" << cap.range.end
					<< ",\"line\":" << line_of(source, cap.range.start)
					<< ",\"column\":" << column_of(source, cap.range.start)
					<< ",\"text\":" << quoted(cap.text)
					<< "}";
				return out.str();
			}

			void print_capture_text(const Capture& cap, std::string_view source) {
				std::cout << cap.name << "\t" << cap.node_type()
					<< "\t" << cap.range.start << ".." << cap.range.end
					<< "\t" << line_of(source, cap.range.start) << ":" << column_of(source, cap.range.start)
					<< "\t" << quoted(cap.text) << "\n";
			}

			uint32_t count_error_nodes(TSNode node) {
				if (ts_node_is_null(node)) return 0;

				uint32_t count = (ts_node_is_error(node) || ts_node_is_missing(node)) ? 1u : 0u;
				const uint32_t children = ts_node_child_count(node);
				for (uint32_t i = 0; i < children; ++i) {
					count += count_error_nodes(ts_node_child(node, i));
				}
				return count;
			}

			/* ----------------------------------------------------------- query */

			/* Builds the query from every --query value and applies the filters
			   that the query layer supports natively. */
			/* `capture_storage` must outlive every use of `options`: the capture
			   filter is a string_view, and binding it to the temporary returned by
			   Options::value() would leave the view dangling. */
			bool build_query(
				const Options& opts,
				Query& query,
				MatchOptions& options,
				std::string& capture_storage,
				std::string& err)
			{
				const std::vector<std::string> patterns = opts.values("--query");
				if (patterns.empty()) {
					err = "no query given; pass at least one --query '<pattern>'";
					return false;
				}

				for (const std::string& pattern : patterns) {
					if (!query.add(pattern)) {
						std::ostringstream detail;
						detail << "pattern failed to compile: " << query.error_message();
						const std::vector<PatternError>& errors = query.pattern_errors();
						if (!errors.empty()) {
							detail << " (pattern " << errors.front().pattern_index
								<< " at offset " << errors.front().offset << ")";
						}
						err = detail.str();
						return false;
					}
				}

				if (opts.has("--capture")) {
					capture_storage = opts.value("--capture");
					options.capture = capture_storage;
				}

				if (opts.has("--pattern-index")) {
					uint32_t index = 0;
					if (!parse_u32(opts.value("--pattern-index"), index)) {
						err = "--pattern-index expects an integer";
						return false;
					}
					if (index >= query.pattern_count()) {
						err = "--pattern-index " + std::to_string(index) + " is out of range ("
							+ std::to_string(query.pattern_count()) + " pattern(s))";
						return false;
					}
					options.pattern_index = index;
				}

				if (opts.has("--limit")) {
					uint32_t limit = 0;
					if (!parse_u32(opts.value("--limit"), limit)) {
						err = "--limit expects an integer";
						return false;
					}
					options.limit = limit;
				}

				if (opts.has("--byte-range")) {
					const std::string spec = opts.value("--byte-range");
					const size_t colon = spec.find(':');
					if (colon == std::string::npos) {
						err = "--byte-range expects START:END";
						return false;
					}

					uint32_t start = 0;
					uint32_t end = 0;
					if (!parse_u32(std::string_view(spec).substr(0, colon), start) ||
						!parse_u32(std::string_view(spec).substr(colon + 1), end)) {
						err = "--byte-range expects START:END as integers";
						return false;
					}
					if (start > end) {
						err = "--byte-range START must not exceed END";
						return false;
					}
					options.byte_range = ByteRange{ start, end };
				}

				return true;
			}

			/* Loads the language, then the file, then parses into a document.
			   Returns nullopt and fills `err` on any failure. */
			std::optional<Document> open_document(
				const Options& opts,
				std::string& path,
				std::string& err)
			{
				const TSLanguage* language = tree_sitter_latex();
				if (!language) {
					err = "failed to load the tree-sitter LaTeX grammar";
					return std::nullopt;
				}

				if (!resolve_file(opts, path, err)) return std::nullopt;

				std::string source;
				if (!read_file(path, source, err)) return std::nullopt;

				Document document(std::move(source), language);
				if (!document.is_valid()) {
					err = "failed to parse '" + path + "' (grammar/ABI mismatch?)";
					return std::nullopt;
				}
				return std::optional<Document>(std::move(document));
			}

			/* Shared by replace/erase/insert so all three select identically. */
			bool pick_targets(
				const std::vector<Capture>& found,
				const Options& opts,
				std::vector<Capture>& targets,
				int& early_exit,
				std::string& err)
			{
				early_exit = kOk;

				if (found.empty()) {
					err = "no capture matched";
					early_exit = kNoMatch;
					return false;
				}

				if (opts.has("--all")) {
					targets = found;
					return true;
				}

				if (opts.has("--match")) {
					uint32_t index = 0;
					if (!parse_u32(opts.value("--match"), index)) {
						err = "--match expects an integer";
						return false;
					}
					if (index >= found.size()) {
						err = "--match " + std::to_string(index) + " is out of range ("
							+ std::to_string(found.size()) + " match(es))";
						return false;
					}
					targets.push_back(found[index]);
					return true;
				}

				if (found.size() == 1) {
					targets.push_back(found.front());
					return true;
				}

				/* Refuse to guess: an ambiguous bulk edit is exactly the mistake
				   an agent should not be allowed to make silently. */
				err = std::to_string(found.size())
					+ " captures matched; narrow with --capture, or choose --match N or --all";
				return false;
			}

			/* -------------------------------------------------------- commands */

			int cmd_query(const Options& opts) {
				std::string path;
				std::string err;

				std::optional<Document> opened = open_document(opts, path, err);
				if (!opened) return fail("input_error", err, opts);
				Document& document = *opened;

				Query query(document.language());
				MatchOptions options;
				std::string capture_name;
				if (!build_query(opts, query, options, capture_name, err)) {
					return fail("query_error", err, opts);
				}

				const std::string_view source = document.source();
				const bool grouped = opts.has("--group");
				const bool text = is_text_format(opts);

				std::ostringstream body;
				uint32_t matched = 0;

				if (grouped) {
					const std::vector<Match> matches = document.matches(query, options);
					matched = static_cast<uint32_t>(matches.size());

					if (!text) {
						body << "{\"command\":\"query\",\"file\":" << quoted(path)
							<< ",\"parse_errors\":" << (document.has_errors() ? "true" : "false")
							<< ",\"matched\":" << matched
							<< ",\"matches\":[";

						bool first_match = true;
						for (const Match& match : matches) {
							if (!first_match) body << ",";
							first_match = false;

							body << "{\"pattern_index\":" << match.pattern_index
								<< ",\"pattern_source\":" << quoted(match.pattern_source)
								<< ",\"start_byte\":" << match.range().start
								<< ",\"end_byte\":" << match.range().end
								<< ",\"captures\":[";

							bool first_capture = true;
							for (const Capture& cap : match.captures) {
								if (!first_capture) body << ",";
								first_capture = false;
								body << capture_json(cap, source);
							}
							body << "]}";
						}
						body << "]}";
					} else {
						for (const Match& match : matches) {
							std::cout << "match " << match.pattern_index << "\n";
							for (const Capture& cap : match.captures) {
								std::cout << "  ";
								print_capture_text(cap, source);
							}
						}
					}
				} else {
					const std::vector<Capture> captures = document.captures(query, options);
					matched = static_cast<uint32_t>(captures.size());

					if (!text) {
						body << "{\"command\":\"query\",\"file\":" << quoted(path)
							<< ",\"parse_errors\":" << (document.has_errors() ? "true" : "false")
							<< ",\"pattern_count\":" << query.pattern_count()
							<< ",\"capture_count\":" << query.capture_count()
							<< ",\"matched\":" << matched
							<< ",\"captures\":[";

						bool first = true;
						for (const Capture& cap : captures) {
							if (!first) body << ",";
							first = false;
							body << capture_json(cap, source);
						}
						body << "]}";
					} else {
						for (const Capture& cap : captures) print_capture_text(cap, source);
					}
				}

				if (!text) print_json(body.str());
				return matched > 0 ? kOk : kNoMatch;
			}

			/* The single implementation behind replace/erase/insert. */
			int run_edit(const Options& opts, const std::string& command) {
				const bool wants_text = command != "erase";
				if (wants_text && !opts.has("--text")) {
					return fail("usage_error", command + " requires --text", opts);
				}

				std::string at = opts.value("--at", "after");
				if (command == "insert" && at != "before" && at != "after") {
					return fail("usage_error", "--at expects 'before' or 'after'", opts);
				}

				std::string path;
				std::string err;
				std::optional<Document> opened = open_document(opts, path, err);
				if (!opened) return fail("input_error", err, opts);
				Document& document = *opened;

				Query query(document.language());
				MatchOptions options;
				std::string capture_name;
				if (!build_query(opts, query, options, capture_name, err)) {
					return fail("query_error", err, opts);
				}

				const std::string_view source = document.source();
				const std::vector<Capture> found = document.captures(query, options);

				std::vector<Capture> targets;
				int early_exit = kOk;
				if (!pick_targets(found, opts, targets, early_exit, err)) {
					return fail(early_exit == kNoMatch ? "no_match" : "usage_error", err, opts);
				}

				/* Every edit is expressed against the current revision, then
				   committed together, so multiple targets cannot invalidate each
				   other's offsets. */
				const std::string inserted = opts.value("--text");

				/* Snapshot what the report needs BEFORE applying: apply_all()
				   replaces the document's buffer, which invalidates every
				   string_view into the old text (Capture::text included). */
				struct Planned
				{
					ByteRange range;
					uint32_t line{ 0 };
					uint32_t column{ 0 };
					std::string before;
					std::string after;
				};

				std::vector<TextEdit> edits;
				std::vector<Planned> planned;
				edits.reserve(targets.size());
				planned.reserve(targets.size());

				for (const Capture& cap : targets) {
					Planned entry;

					if (command == "replace") {
						entry.range = cap.range;
						entry.before.assign(cap.text);
						entry.after = inserted;
						edits.push_back(TextEdit::update(cap.range, inserted));
					} else if (command == "erase") {
						entry.range = cap.range;
						entry.before.assign(cap.text);
						edits.push_back(TextEdit::remove(cap.range));
					} else {
						const uint32_t offset = (at == "before") ? cap.range.start : cap.range.end;
						/* An insertion replaces no existing bytes. */
						entry.range = ByteRange{ offset, offset };
						entry.after = inserted;
						edits.push_back(TextEdit::create(offset, inserted));
					}

					entry.line = line_of(source, entry.range.start);
					entry.column = column_of(source, entry.range.start);
					planned.push_back(std::move(entry));
				}

				if (!document.apply_all(std::move(edits))) {
					return fail("edit_rejected", "edits were out of bounds or overlapping", opts);
				}

				/* Writing is opt-in, and --dry-run overrides an explicit
				   destination so a preview can never write by accident. */
				std::string destination;
				if (!opts.has("--dry-run")) {
					if (opts.has("--in-place")) {
						destination = path;
					} else if (opts.has("--output")) {
						destination = opts.value("--output");
					}
				}

				const bool to_stdout = destination == "-";
				std::string wrote;

				if (!destination.empty() && !to_stdout) {
					if (!write_file(destination, std::string(document.source()), err)) {
						return fail("write_error", err, opts);
					}
					wrote = destination;
				}

				if (to_stdout) {
#ifdef _WIN32
					_setmode(_fileno(stdout), _O_BINARY);
#endif
					std::cout.write(document.source().data(),
						static_cast<std::streamsize>(document.source().size()));
					return kOk;
				}

				if (is_text_format(opts)) {
					std::cout << command << ": " << targets.size() << " edit(s), revision "
						<< document.revision()
						<< (wrote.empty() ? " (preview, nothing written)" : " -> " + wrote)
						<< (document.has_errors() ? ", PARSE ERRORS" : "") << "\n";
					return kOk;
				}

				std::ostringstream body;
				body << "{\"command\":" << quoted(command)
					<< ",\"file\":" << quoted(path)
					<< ",\"matched\":" << found.size()
					<< ",\"selected\":" << targets.size()
					<< ",\"wrote\":" << (wrote.empty() ? "null" : quoted(wrote))
					<< ",\"revision\":" << document.revision()
					<< ",\"parse_errors\":" << (document.has_errors() ? "true" : "false")
					<< ",\"edits\":[";

				bool first = true;
				for (const Planned& entry : planned) {
					if (!first) body << ",";
					first = false;

					body << "{\"start_byte\":" << entry.range.start
						<< ",\"end_byte\":" << entry.range.end
						<< ",\"line\":" << entry.line
						<< ",\"column\":" << entry.column
						<< ",\"before\":" << quoted(entry.before)
						<< ",\"after\":" << quoted(entry.after)
						<< "}";
				}
				body << "]}";

				print_json(body.str());
				return kOk;
			}

			int cmd_check(const Options& opts) {
				std::string path;
				std::string err;
				std::optional<Document> opened = open_document(opts, path, err);
				if (!opened) return fail("input_error", err, opts);
				Document& document = *opened;

				const bool has_errors = document.has_errors();
				const uint32_t error_nodes = count_error_nodes(document.root());

				if (is_text_format(opts)) {
					std::cout << path << ": " << (has_errors ? "PARSE ERRORS" : "ok")
						<< " (" << error_nodes << " error node(s))\n";
				} else {
					std::ostringstream body;
					body << "{\"command\":\"check\",\"file\":" << quoted(path)
						<< ",\"valid\":true"
						<< ",\"parse_errors\":" << (has_errors ? "true" : "false")
						<< ",\"error_nodes\":" << error_nodes
						<< ",\"root_type\":" << quoted(ts_node_type(document.root()))
						<< "}";
					print_json(body.str());
				}

				return has_errors ? kNoMatch : kOk;
			}

			int cmd_tree(const Options& opts) {
				std::string path;
				std::string err;
				std::optional<Document> opened = open_document(opts, path, err);
				if (!opened) return fail("input_error", err, opts);
				Document& document = *opened;

				const std::string sexpr = document.s_expression();
				const bool has_errors = document.has_errors();

				if (is_text_format(opts)) {
					std::cout << sexpr << "\n";
				} else {
					std::ostringstream body;
					body << "{\"command\":\"tree\",\"file\":" << quoted(path)
						<< ",\"parse_errors\":" << (has_errors ? "true" : "false")
						<< ",\"s_expression\":" << quoted(sexpr)
						<< "}";
					print_json(body.str());
				}

				return has_errors ? kNoMatch : kOk;
			}

			int cmd_version(const Options& opts) {
				if (is_text_format(opts)) {
					std::cout << kTool << " " << kVersion << "\n";
				} else {
					std::ostringstream body;
					body << "{\"tool\":" << quoted(kTool)
						<< ",\"version\":" << quoted(kVersion)
						<< ",\"tree_sitter_api_version\":" << TREE_SITTER_LANGUAGE_VERSION
						<< "}";
					print_json(body.str());
				}
				return kOk;
			}

			int cmd_help(const Options& opts) {
				std::cout <<
					"docutils " << kVersion << " - query and edit LaTeX from a query layer\n"
					"\n"
					"USAGE\n"
					"  docutils <command> [--file PATH] [options]\n"
					"\n"
					"COMMANDS (one command performs one action and exits)\n"
					"  query    Run patterns and report captures\n"
					"  replace  Replace the selected capture(s) with --text\n"
					"  erase    Remove the selected capture(s)\n"
					"  insert   Insert --text at a capture boundary (--at before|after)\n"
					"  check    Parse and report syntax errors\n"
					"  tree     Print the syntax tree as an S-expression\n"
					"  version  Print tool and tree-sitter versions\n"
					"  help     Print this text\n"
					"\n"
					"COMMON OPTIONS\n"
					"  --file PATH          Input file (or the first positional after the command)\n"
					"  --query PATTERN      Repeatable; tree-sitter query source\n"
					"  --capture NAME       Only captures with this name\n"
					"  --pattern-index N    Only matches from this pattern\n"
					"  --byte-range A:B     Restrict the search to a byte range\n"
					"  --limit N            Stop after N results\n"
					"  --group              query: report whole matches instead of flat captures\n"
					"  --format json|text   Output format (default json)\n"
					"\n"
					"EDIT SELECTION\n"
					"  With exactly one capture matched it is used automatically. Otherwise pass\n"
					"  --match N (0-based, document order) or --all.\n"
					"\n"
					"WRITING\n"
					"  Edits write nothing unless told where: --in-place (same file) or\n"
					"  --output PATH (use - for stdout, which suppresses the JSON summary).\n"
					"  With no destination the command reports the edit as a preview.\n"
					"  --dry-run is accepted for clarity and also writes nothing.\n"
					"\n"
					"EXIT CODES\n"
					"  0  action performed, or query matched\n"
					"  2  valid command, nothing matched (query/check) or parse errors found\n"
					"  1  error: bad usage, I/O failure, or rejected edit\n"
					"\n"
					"EXAMPLES\n"
					"  docutils query --file paper.tex \\\n"
					"      --query '(section text: (curly_group (text) @title))'\n"
					"  docutils replace --file paper.tex --capture title --match 0 \\\n"
					"      --query '(section text: (curly_group (text) @title))' \\\n"
					"      --text 'Introduction' --in-place\n"
					"  docutils erase --file paper.tex --all \\\n"
					"      --query '(label_definition) @def' --capture def --in-place\n"
					"  docutils check --file paper.tex\n";

				(void)opts;
				return kOk;
			}

		} // namespace

		int run(int argc, char** argv) {
#ifdef _WIN32
			/* Keep stdout byte-exact for JSON and for --output - pipelines. */
			_setmode(_fileno(stdout), _O_BINARY);
#endif

			Options opts;
			std::string err;
			if (!parse_args(argc, argv, opts, err)) {
				return fail("usage_error", err, false);
			}

			if (opts.positional.empty()) {
				const int code = cmd_help(opts);
				return code == kOk ? kError : code; /* bare invocation is a usage error */
			}

			const std::string command = opts.positional[0];

			if (opts.has("--version") || command == "version") return cmd_version(opts);
			if (opts.has("--help") || command == "help") return cmd_help(opts);

			if (command == "query")   return cmd_query(opts);
			if (command == "replace") return run_edit(opts, "replace");
			if (command == "erase")   return run_edit(opts, "erase");
			if (command == "insert")  return run_edit(opts, "insert");
			if (command == "check")   return cmd_check(opts);
			if (command == "tree")    return cmd_tree(opts);

			return fail("unknown_command", "unknown command '" + command + "'; try 'docutils help'", opts);
		}

	} // namespace Cli
} // namespace Ltx
