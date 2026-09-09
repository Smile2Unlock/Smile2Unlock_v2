#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace su::recognizer::detail {

// SeetaFace's default video policy, with a bounded window and a clarity gate.
// The upstream PredictVideo retains N+1 scores but divides by N.
class LivenessWindow {
public:
    void reset() { *this = {}; }

    float push(float clarity, float reality) {
        if (!std::isfinite(clarity) || !std::isfinite(reality)
            || clarity < 0.3F || reality < 0.0F || reality > 1.0F) {
            reset();
            return 0.0F;
        }
        scores_[next_] = reality;
        next_ = (next_ + 1) % scores_.size();
        if (count_ < scores_.size()) {
            ++count_;
        }
        if (count_ < scores_.size()) {
            return 0.0F;
        }
        auto total = 0.0;
        for (const auto score : scores_) {
            total += score;
        }
        return total / scores_.size() >= 0.8 ? reality : 0.0F;
    }

private:
    std::array<float, 10> scores_{};
    std::size_t count_ = 0;
    std::size_t next_ = 0;
};

} // namespace su::recognizer::detail
