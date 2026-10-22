#pragma once
#include "common/diagnostics/core.h"
#include <filesystem>
namespace su::diagnostics {
std::expected<std::filesystem::path, Error> collect_windows_system();
std::vector<Section> windows_system_records();
}
