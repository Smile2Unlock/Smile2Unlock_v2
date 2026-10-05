#include "app/download_progress.h"

#include <array>
#include <iostream>
#include <limits>

int main() {
    using su::app::archive_download_progress;
    if (archive_download_progress(1, 0)) {
        return 1;
    }
    const auto halfway = archive_download_progress(50'000'000, 100'000'000);
    if (!halfway || halfway->done != 4750 || halfway->total != 10000) {
        std::cerr << "halfway download must display 47.5%\n";
        return 1;
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto complete = archive_download_progress(maximum, maximum);
    const auto overrun = archive_download_progress(maximum, 1);
    if (!complete || complete->done != 9500 || !overrun || overrun->done != 9500) {
        std::cerr << "download byte counts overflowed or exceeded the archive phase\n";
        return 1;
    }
    auto previous = std::uint64_t{};
    for (const auto done : std::array<std::uint64_t, 6>{0, 1, 25, 50, 99, 100}) {
        const auto progress = archive_download_progress(done, 100);
        if (!progress || progress->done < previous || progress->done > 9500) {
            return 1;
        }
        previous = progress->done;
    }
    std::cout << "download progress units, bounds and monotonicity: ok\n";
}
