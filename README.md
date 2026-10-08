# A unified interface for safe document editing

`docutils` queries and edits LaTeX structurally. A tree-sitter query layer is the
only door into a document: you locate nodes by query, and every edit is anchored
to something that query matched. No regex, no full-document rewrites.

It ships as a CLI designed for agents to drive in a loop, plus a C++ library for
embedding.

## Build

Needs CMake 3.16+ and a C++20 toolchain. tree-sitter v0.27.0 and GoogleTest
v1.15.2 are fetched automatically; the LaTeX grammar is vendored in
`third_party/`. 

## CLI contract

- **One command performs one action and exits.** Nothing is interactive, nothing
  is retained between invocations.
- **stdout is the data channel**: JSON, or the document itself with `--output -`.
- **stderr carries errors**, also as JSON.
- Unknown options fail loudly rather than being ignored.

| Exit | Meaning |
|---|---|
| `0` | action performed, or query matched |
| `2` | valid command, nothing matched; for `check`, parse errors found |
| `1` | error: bad usage, I/O failure, ambiguous selection, rejected edit |

Branch on the exit code; parse stdout only when you need the details.

## Commands

| Command | Action |
|---|---|
| `query` | Run patterns, report captures (or whole matches with `--group`) |
| `replace` | Replace selected capture(s) with `--text` |
| `erase` | Remove selected capture(s) |
| `insert` | Insert `--text` at a capture boundary, `--at before\|after` |
| `check` | Parse and report syntax errors |
| `tree` | Print the syntax tree as an S-expression |
| `version`, `help` | Introspection |

## Options

| Option | Effect |
|---|---|
| `--file PATH` | Input; alternatively the first positional after the command |
| `--query PATTERN` | tree-sitter query source; repeatable, all patterns compile into one pass |
| `--capture NAME` | Keep only captures with this name |
| `--pattern-index N` | Keep only matches from pattern `N` |
| `--byte-range A:B` | Restrict the search to a byte range |
| `--limit N` | Stop after `N` results (`0` = unlimited) |
| `--format json\|text` | JSON (default) or a human table |
| `--match N` \| `--all` | Which of several matches to edit |
| `--text TEXT`, `--at before\|after` | Edit payload and anchor for `insert` |
| `--in-place` \| `--output PATH` | Where to write (`-` streams to stdout) |
| `--dry-run` | Never write, even if a destination was given |

## Query patterns

Patterns are [tree-sitter queries](https://tree-sitter.github.io/tree-sitter/using-parsers/queries/1-syntax.html)
against `tree-sitter-latex`. Node names and fields come from the grammar, so
capture a real node rather than guessing. Verified starters:

```scheme
(section text: (curly_group (text) @title))              ; section title text
(label_definition) @def                                   ; whole \label{...}
(label_definition name: (curly_group_label (label) @key)) ; just the label key
(generic_environment end: (end) @env_end)                 ; \end{document}
```

Explore with `docutils tree --file paper.tex` when you need the shape.

## Agent workflow

Find, look, edit, verify. Each step is one invocation.

```sh
# 1. Locate. Exit 2 means "nothing matched", not a failure.
docutils query --file paper.tex \
  --query '(section text: (curly_group (text) @title))' --capture title
# {"command":"query",...,"matched":2,"captures":[
#   {"name":"title","capture_id":0,"node_type":"text",
#    "start_byte":52,"end_byte":64,"line":3,"column":10,"text":"Introduction"}, ...]}

# 2. Preview a single edit. No destination, so nothing is written.
docutils replace --file paper.tex \
  --query '(section text: (curly_group (text) @title))' \
  --capture title --match 0 --text 'Introduction and Motivation'
# {"command":"replace",...,"selected":1,"wrote":null,"revision":1,"parse_errors":false,
#  "edits":[{"start_byte":52,"end_byte":64,"line":3,"column":10,
#            "before":"Introduction","after":"Introduction and Motivation"}]}

# 3. Commit it.
docutils replace --file paper.tex \
  --query '(section text: (curly_group (text) @title))' \
  --capture title --match 0 --text 'Introduction and Motivation' --in-place

# 4. Verify the document still parses.
docutils check --file paper.tex
```

Offsets are 0-based and half-open; `line`/`column` are 1-based. Files are read
and written in binary, so byte offsets in the JSON always address exactly the
bytes you asked for.

### Reading the result

Every edit reports exactly what changed, against the revision it was computed
from:

```json
{"command":"replace","file":"paper.tex","matched":2,"selected":1,
 "wrote":"paper.tex","revision":1,"parse_errors":false,
 "edits":[{"start_byte":52,"end_byte":64,"line":3,"column":10,
           "before":"Introduction","after":"Intro"}]}
```

`wrote` is `null` for a preview. Always re-query after a write: a committed edit
invalidates every earlier capture, and the query layer will not silently reuse a
stale one.

## Safety model

- **Ambiguity is refused, not guessed.** One matched capture is used
  automatically; otherwise you must pass `--match N` or `--all`. A command that
  would rewrite several sections by accident exits `1` instead.
- **Writes are opt-in.** Without `--in-place` or `--output`, the command reports
  what it *would* do and changes nothing. `--dry-run` overrides an explicit
  destination, so `--dry-run --in-place` can never write.
- **Batches are atomic.** `--all` validates every range before touching the
  document; a rejected batch leaves the file, and the revision, untouched.
- **Damage is visible.** Every edit re-parses incrementally and reports
  `parse_errors`, so a change that breaks the document says so immediately
  instead of failing later.

## Library

The CLI is a thin shell over the same three layers:

| Header | Role |
|---|---|
| `latex/ltx_edit.hpp` | `ByteRange`, `TextEdit`, `apply_edit(s)`; byte arithmetic and `TSInputEdit` construction |
| `latex/ltx_query.hpp` | `Query` (patterns added at runtime), `Match`, `Capture`, `MatchOptions` filters |
| `latex/ltx_document.hpp` | `Document`: owns text, parser and tree; read + CRUD in one revision |

```cpp
Ltx::Document doc(source, tree_sitter_latex());
Ltx::Query query(doc.language());
query.add("(section text: (curly_group (text) @title))");

Ltx::MatchOptions titles_only;
titles_only.capture = "title";

// Collect the edits first, then commit them together. Every range is then
// expressed against one revision: committing an edit invalidates the captures
// taken before it, so mutating inside the loop would leave the rest dangling.
std::vector<Ltx::TextEdit> edits;
for (const Ltx::Capture& title : doc.captures(query, titles_only)) {
    edits.push_back(Ltx::TextEdit::update(title.range, "New title"));
}
doc.apply_all(std::move(edits));  // validated, applied back-to-front, one reparse
```

Link `docutils_lib`; it carries the include paths and the tree-sitter
dependencies.

## Tests

97 GoogleTest cases across four layers — edit arithmetic, query compilation and
filters, document CRUD and reparse behaviour, and the CLI contract including
exit codes and write safety. `test/test_utils.hpp` holds the fixtures; the CLI
is driven in-process, so the suite needs no subprocesses.
