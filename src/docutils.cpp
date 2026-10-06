// docutils.cpp : agent-facing CLI entry point.
//
// The query + edit layer lives under latex/. This file only forwards to it, so
// every command shares one implementation. Run `docutils help` for the command
// reference; CHANGES.md records the API history.

#include "latex/ltx_cli.hpp"

int main(int argc, char** argv) {
	return Ltx::Cli::run(argc, argv);
}
