// Load mode: many bot sessions in one process, each running a behaviour (idle, wander, chat, speedhack, malformed).
#pragma once
#include "harness.h"

namespace chthonic::bot {

// Returns the process exit code: non-zero on any unexpected disconnect, error, or a behaviour that didn't do
// what it should (e.g. a speedhack bot that was never corrected).
int run_load(const Options& o);

} // namespace chthonic::bot
