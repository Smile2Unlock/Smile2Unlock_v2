#pragma once
#include "core.h"
#include <chrono>
namespace su::diagnostics {
// Absolute executable, argv only, no shell; captures stdout/stderr within a deadline.
std::expected<std::string, Error>
run_command(const std::vector<std::string>& arguments, std::size_t cap = output_limit,
            std::chrono::seconds timeout = std::chrono::seconds{30});
} // namespace su::diagnostics
