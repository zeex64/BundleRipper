#pragma once
#include "scene.h"

namespace xl {

// Decodes, converts and PNG-encodes every job, in parallel.
void run_image_jobs(const Database& db, std::vector<ImageJob>& jobs, int max_size);

} // namespace xl
