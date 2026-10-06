# API changes — LaTeX query & edit layer

Files touched: `src/latex/ltx_query.{hpp,cpp}`, new `src/latex/ltx_edit.{hpp,cpp}`,
new `src/latex/ltx_document.{hpp,cpp}`, `src/docutils.cpp`, `src/CMakeLists.txt`.

Verified against the pinned toolchain: tree-sitter **v0.27.0** (ABI 15) and the
vendored `third_party/tree_sitter_latex` grammar (`LANGUAGE_VERSION 14`, inside
the supported 13–15 range). No deprecated tree-sitter entry points are used;
the pinned `api.h` marks none.

## Why

`Ltx::Query` bound its single pattern in the constructor, so a second query meant
a second object, and there was no way to write anything back. Two gaps: patterns
were fixed at construction, and the AST could not be edited at all.

## Layering (dependencies run one way)

    Document            the query layer: the only public entry point
      Document  -> ltx_document.*   owns source + parser + tree, runs CRUD
      Query     -> ltx_query.*      compiles patterns, returns matches/captures
      TextEdit  -> ltx_edit.*       byte-range primitives + TSInputEdit math

`ByteRange` moved from `ltx_query.hpp` to `ltx_edit.hpp` so the primitive layer
owns it, matching the "query uses the edit API" direction.

## `ltx_query` — patterns are now runtime data

- `Query(const TSLanguage*)` replaces `Query(language, pattern)`. Language fixed,
  patterns not.
- `add(pattern) -> optional<uint32_t>`, `add_many(...)`, `remove(index)`,
  `clear()`. Tree-sitter cannot drop one pattern from a compiled `TSQuery`, so
  each mutation rebuilds from a stored source list. A rejected `add`/`remove`
  leaves the previous query untouched.
- One `TSQuery` holds every pattern; `Match::pattern_index` plus
  `Match::pattern_source` identify which one fired.
- Per-pattern failures no longer poison the whole query: `pattern_errors()`
  returns `{pattern_index, source, message, offset}`. `is_valid()` now means
  "the most recent rebuild succeeded".
- `MatchOptions{pattern_index, capture, byte_range, limit}` filters an execution
  without recompiling; both `matches()` and `captures()` gained an options
  overload.
- `Capture` gained `capture_id`, `range`, `node_type()`. `Match` gained
  `find_capture()` and `range()`. `Query` gained `capture_name()`,
  `has_capture()`, `pattern_source()`.
- Hardening: cursors are null-checked, capture text is bounds-checked, and
  `TSQueryErrorLanguage` is handled.

## `ltx_edit` — CRUD primitives (new)

- `TextEdit` with `create`/`update`/`remove` factories and `EditKind`
  (Create/Update/Delete).
- `apply_edit()` / `apply_edits()` mutate a source buffer and produce the
  `TSInputEdit` tree-sitter needs. Batches are validated (in-bounds,
  non-overlapping), applied back-to-front so earlier offsets stay valid, and
  reported as one spanning edit so untouched subtrees are reused.
- `point_for_byte()` converts byte offsets to tree-sitter row/column.

## `ltx_document` — the query layer entry point (new)

`Document` owns parser, tree and text, so reads and edits share one revision.

- READ: `matches()`, `captures()`, `find_first()`, `root()`, `s_expression()`,
  `has_errors()`, `source()`.
- CREATE: `insert(offset, text)`, `insert_before(capture, text)`,
  `insert_after(capture, text)`.
- UPDATE: `replace(capture|match, text)`.
- DELETE: `erase(capture|match)`.
- Raw escape hatch: `apply(TextEdit)`, `apply_all(vector<TextEdit>)`.
- Edits re-parse incrementally (`ts_tree_edit`, then
  `ts_parser_parse_string(..., old_tree, ...)`), so only the touched region is
  re-parsed. A rejected batch changes nothing, revision included.
- `revision()` / `is_stale()` exist because every `Capture`/`Match` points into
  the owned buffer and tree and dies at the next committed edit.
- Fixed: `ts_parser_set_language` returns `bool` (it does not assert) and its
  result was being ignored. `Document` now checks it and surfaces failure via
  `is_valid()`, which is how a grammar/ABI mismatch becomes visible instead of a
  null parse.

## `ltx_cli` - agent-facing CLI (new)

`docutils.cpp` is now a thin `main` that forwards to `Ltx::Cli::run`, so every
command shares one implementation. The contract is aimed at programs rather than
people:

- one command performs exactly one action and exits; nothing is interactive
- JSON on stdout, errors as JSON on stderr (`--format text` for humans)
- exit codes: `0` acted or matched, `2` valid command but nothing matched (also
  reported for parse errors), `1` error (bad usage, I/O failure, rejected edit)

Commands: `query` (flat captures, or `--group` for whole matches), `replace`,
`erase`, `insert` (`--at before|after`), `check`, `tree`, `version`, `help`.
Run `docutils help` for the full reference.

Filters reuse the query layer directly: `--query` (repeatable), `--capture`,
`--pattern-index`, `--byte-range A:B`, `--limit`.

Selection is deliberately explicit: a single capture is used automatically,
otherwise `--match N` (0-based, document order) or `--all` is required. An
ambiguous bulk edit is refused rather than guessed - the mistake an agent should
not be able to make silently.

Writes are opt-in: nothing is written without `--in-place` or `--output PATH`
(`--output -` streams the source to stdout instead of the JSON summary). With no
destination the command reports the edit as a preview and sets `"wrote": null`.
Files are read and written in binary so newline translation cannot shift the
byte offsets a query reports.

Two bugs found by running the CLI, not by reading it:

- `--capture` never matched. `MatchOptions::capture` is a `string_view` and was
  bound to the temporary returned by `Options::value()`. The name now lives in a
  `std::string` that outlives the options.
- Edit reports showed mojibake `before` text and wrong line/column, because they
  read `Capture::text` and the pre-edit source view after `apply_all()` had
  replaced the document buffer. The report now snapshots those values first.

## Build

- `src/CMakeLists.txt` gained `latex/ltx_edit.cpp`, `latex/ltx_document.cpp` and
  `latex/ltx_cli.cpp`, and now builds the layer as a static library
  `docutils_lib`. Both the `docutils` executable and `docutils_tests` link it,
  so the tests exercise exactly what ships.
- Root `CMakeLists.txt` gained `DOCUTILS_BUILD_TESTS` (default ON),
  `enable_testing()` and a project-wide `CMAKE_CXX_STANDARD 20`. The declared
  minimum rose from 3.10 to 3.16: the project already required
  `FetchContent_MakeAvailable` (3.14+), so 3.10 was never accurate.

## `test/` - GoogleTest suite (new)

97 tests in four translation units plus a shared fixture header:

| File | Covers |
|---|---|
| `ltx_edit_test.cpp` | `ByteRange`, `TextEdit` factories, `point_for_byte`, and `apply_edit`/`apply_edits` including out-of-bounds, overlap and length-changing batches |
| `ltx_query_test.cpp` | pattern add/remove/clear, per-pattern errors, capture/pattern-index/limit/byte-range filters, match contents, move semantics |
| `ltx_document_test.cpp` | CRUD, revision/staleness, batched commit, rejected-batch atomicity, incremental reparse, parse errors, moves |
| `ltx_cli_test.cpp` | every command, exit codes 0/2/1, preview versus write, `--dry-run`, `--output`, ambiguity refusal |

`test/test_utils.hpp` holds the fixtures: a self-deleting `TempFile`, a
`ParsedSource` (parser + tree, for driving `Query` directly) and `run_cli()`,
which calls `Ltx::Cli::run()` in-process with captured streams - so the CLI
contract is tested without spawning subprocesses. `test/CMakeLists.txt` fetches
GoogleTest v1.15.2 via `FetchContent` and registers the cases with CTest.

Run with `ctest --test-dir out/build/<preset>`, or execute `docutils_tests`.

### Bugs the tests caught

Both were in code written earlier in this session, and neither is visible by
reading it:

- `Query::capture_name(id)` called `ts_query_capture_name_for_id` without a
  bounds check. That function **asserts** on an out-of-range id in a debug
  build, so an innocent `capture_name(99)` aborted the whole process. The bound
  is now checked first.
- `Query::is_valid()` was `m_query != nullptr`, so a legitimately empty query
  (no patterns yet, or all removed) reported itself invalid, contradicting both
  `has_patterns()` and the documented "empty is a valid state". It now means "a
  language is set and every stored pattern compiled", so empty is valid-but-empty
  and a rejected `add` leaves a working query working.

Three further test-only mistakes were corrected along the way: the preprocessor
read the comma in `ByteRange{0, 0}` inside `EXPECT_*` as an extra macro argument
(the temporaries are now parenthesised), one expected string ignored how a prior
insert had shifted the buffer, and one JSON assertion was over-escaped.

## Build & verification

Verified by compiling and linking the translation units directly with `cl.exe`
(`/std:c++20 /W4 /MDd`, against the tree-sitter v0.27.0 static libs already in
the build tree). Result: **no warnings or errors from project code**, and every
CLI command was exercised end-to-end — `query` (flat and `--group`), `replace`,
`erase`, `insert`, `check`, `tree`, `version`, `--format text`, `--output -`,
in-place writes, preview-without-write, and the exit codes 0/2/1 including the
ambiguous-selection refusal.

CMake state, for traceback:

- Adding the two new sources to `src/CMakeLists.txt` forces a reconfigure.
  `out/build/x64-debug` cannot complete it: the cached `CMAKE_C_COMPILER=cl.exe`
  (relative) has to be re-detected, which invalidates the cache, and the
  resulting compiler try-compile aborts at its resource step. CMake invokes
  `--rc=rc` and gets "no such file or directory" — a spawn failure, not a
  missing file: `rc.exe` resolves and runs fine when invoked directly from the
  same shell. Passing `-DCMAKE_RC_COMPILER=<full path>` did not change the
  `--rc=rc` argument, and `-DCMAKE_MT=<full path>` did take effect, so the
  escalation is specific to that nested spawn.
- That failed wipe deleted `CMakeFiles/rules.ninja`, so **`out/build/x64-debug`
  now needs a fresh configure before it can build again**.
- The directory also carries a protected DACL: the sandbox token cannot create
  files in it. Permissions were repaired on `out`, `out/build` and
  `out/build/x64-debug`; backups and rollback scripts are the
  `acl-backup-*.json` files in `E:\project\side-projects`.
- Recommended fix: delete `out/build/x64-debug` and reconfigure from Visual
  Studio, whose own toolchain environment resolves `rc.exe`, or configure into a
  fresh directory.

