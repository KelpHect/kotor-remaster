#pragma once

#include "app/Args.h"

namespace ke {

// Runs the scaffold. Returns a process exit code:
//   0 success, 2 usage, 3 SDL init, 4 window creation, 5 backend init, 6 frame fatal,
//   7 self-test failure.
int runApplication(const Options& options);

} // namespace ke
