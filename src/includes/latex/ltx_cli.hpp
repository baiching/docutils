#pragma once

/* Agent-facing command line interface for the LaTeX query + edit layer.
 *
 * Design contract, relied on by callers that are programs rather than people:
 *   - one command performs exactly one action and exits; nothing is interactive
 *   - results go to stdout as JSON (use --format text for humans)
 *   - writes happen only where you point them (--in-place / --output). With no
 *     destination the command still reports what it would do and writes nothing
 *   - exit codes: 0 acted or matched, 2 valid command but nothing matched,
 *     1 error (bad usage, I/O failure, rejected edit)
 */

namespace Ltx {
	namespace Cli {
		/* Returns the process exit code. */
		int run(int argc, char** argv);
	}
}
