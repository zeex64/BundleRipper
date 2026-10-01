// Reads JSON text into a Json value (json.h).
#pragma once
#include "json.h"

#include <string>

namespace xl {

// False (with `error` saying where) when `text` is not valid JSON.
bool parse_json(const std::string& text, Json& out, std::string& error);

// Pretty-printed JSON (two-space indent), for files people may read.
std::string dump_pretty(const Json& j);

} // namespace xl
