#pragma once
#include "common/diagnostics/core.h"
namespace su::diagnostics {
std::expected<std::string, Error> collect_linux_system();
}
