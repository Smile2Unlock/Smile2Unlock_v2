#ifdef NDEBUG
#undef NDEBUG
#endif
#include "recognizer/camera/capture_mode.h"
#include "recognizer/latest_frame.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <future>
#include <iostream>
#include <thread>

using su::recognizer::detail::CaptureMode;
using su::recognizer::detail::capture_mode_rank;

int main() {
    const auto choose = [](auto modes) {
        return *std::min_element(modes.begin(), modes.end(), [](auto a, auto b) {
            return capture_mode_rank(a) < capture_mode_rank(b);
        });
    };
    const auto mjpeg = CaptureMode{1280, 720, 30, 1, true};
    const auto slow_yuy2 = CaptureMode{1280, 720, 5, 1, false};
    const auto fast_yuy2 = CaptureMode{1280, 720, 30, 1, false};
    const auto uhd = CaptureMode{3840, 2160, 30, 1, true};
    const auto vga = CaptureMode{640, 480, 30, 1, false};
    assert(choose(std::array{slow_yuy2, uhd, fast_yuy2, mjpeg}).mjpeg);
    assert(choose(std::array{uhd, mjpeg}).width == 1280);
    assert(choose(std::array{slow_yuy2, vga}).width == 640);
    assert(choose(std::array{uhd, vga}).width == 640);
    const auto fractional = CaptureMode{1280, 720, 30000, 1001, true};
    assert(fractional.fps() > 29.9 && fractional.fps() < 30.0);
    assert(choose(std::array{slow_yuy2, fractional}).rate_denominator == 1001);
    assert(choose(std::array{fast_yuy2, fractional}).mjpeg);
    assert(choose(std::array{fast_yuy2, CaptureMode{960, 540, 30, 1, true}}).mjpeg);
    assert(CaptureMode({640, 480, 0, 0, false}).fps() == 0.0);
    assert(!CaptureMode({0, 480, 30, 1, true}).valid());
    assert(!CaptureMode({641, 480, 30, 1, false}).valid());
    assert(CaptureMode({641, 480, 30, 1, true}).valid());

    // Hold inference on its first frame while capture replaces pending
    // frames. No queue growth, no repeated inference of the same sequence.
    auto queue = su::recognizer::detail::LatestFrameQueue<int>{};
    std::promise<void> first_consumed;
    std::promise<void> resume;
    auto resume_future = resume.get_future();
    queue.publish(std::make_shared<const int>(1));
    auto consumer = std::async(std::launch::async, [&] {
        std::uint64_t consumed = 0;
        assert(*queue.wait_next(consumed) == 1);
        first_consumed.set_value();
        resume_future.wait();
        assert(*queue.wait_next(consumed) == 1000);
        assert(consumed == 1000);
        return consumed;
    });
    first_consumed.get_future().wait();
    auto discarded = std::make_shared<const int>(2);
    const auto weak = std::weak_ptr<const int>(discarded);
    queue.publish(std::move(discarded));
    for (auto i = 3; i <= 1000; ++i) {
        queue.publish(std::make_shared<const int>(i));
    }
    assert(weak.expired());
    resume.set_value();
    auto consumed = consumer.get();
    auto waiting = std::async(std::launch::async, [&] { return queue.wait_next(consumed); });
    assert(waiting.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout);
    queue.close();
    assert(!waiting.get());
    queue.publish(std::make_shared<const int>(1001));
    assert(!queue.wait_next(consumed));
    std::cout << "PASS: native capture cadence/size/format selection and bounded inference mailbox\n";
}
