#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace su::app {

struct DownloadProgress {
    std::uint64_t done;
    std::uint64_t total;
};

// Reserve 5% for checksum verification and extraction. Both callback values
// use basis points, preserving fractional percentages without multiplying
// a potentially large byte count by 95.
[[nodiscard]] constexpr std::optional<DownloadProgress>
archive_download_progress(std::uint64_t done, std::uint64_t total) noexcept {
    if (total == 0) {
        return std::nullopt;
    }
    const auto fraction = std::min(static_cast<long double>(done) / total, 1.0L);
    return DownloadProgress{
        .done = static_cast<std::uint64_t>(fraction * 9500),
        .total = 10000,
    };
}

} // namespace su::app
