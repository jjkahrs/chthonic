// Scripted integration scenarios against a running stack. Each prints its checks and returns pass/fail.
#pragma once
#include "harness.h"

namespace chthonic::bot {

// Runs o.scenario; returns the process exit code (0 = PASS).
int run_scenario(const Options& o);

} // namespace chthonic::bot
