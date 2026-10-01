#pragma once
#include "job.h"

namespace xl {

// Opens the Spotbuilder window with `job` filled in (remembered settings when `use_saved`),
// and returns when it closes.
int run_gui(const Job& job, bool use_saved);

} // namespace xl
