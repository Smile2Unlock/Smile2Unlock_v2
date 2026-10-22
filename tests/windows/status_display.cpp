#include "platform/windows/status_host/caption.h"
#include <cstdlib>
#include <iostream>

namespace {
using namespace su::status;

void check(bool condition) { if (!condition) std::abort(); }

void retry_captions() {
    for (const auto chinese : {false, true}) {
        const auto retry = chinese ? L"按 Enter 重试，或输入密码"
            : L"Press Enter to retry, or enter your password";
        for (const auto automatic : {false, true}) {
            for (const auto no_face : {false, true}) {
                for (const auto phase : {Phase::no_match, Phase::timed_out}) {
                    const auto copy = caption({phase, automatic, 0, no_face}, chinese);
                    check(copy.detail == retry);
                    const auto expected = no_face ? (chinese ? L"未检测到人脸" : L"No face detected")
                        : phase == Phase::no_match ? (chinese ? L"暂未通过验证" : L"Face not verified")
                        : (chinese ? L"人脸识别已超时" : L"Face recognition timed out");
                    check(copy.title == expected);
                }
                const auto unavailable = caption({Phase::unavailable, automatic, 0, no_face}, chinese);
                check(unavailable.detail == (chinese ? L"按 Enter 重试，或使用密码登录"
                    : L"Press Enter to retry, or use your password"));
                const auto failed = caption({Phase::login_failed, automatic, 0, no_face}, chinese);
                check(failed.title == (chinese ? L"Windows 未接受登录" : L"Windows could not sign you in"));
                check(failed.detail == (chinese ? L"请使用密码，并检查保存的账户凭据"
                    : L"Use your password and check your saved credentials"));
            }
        }
    }
}

void countdown_captions() {
    for (const auto chinese : {false, true}) {
        for (const auto automatic : {false, true}) {
            for (const auto seconds : {0U, 3U, 65535U}) {
                const auto countdown = std::to_wstring(seconds);
                const auto no_face = caption({Phase::retry_delay, automatic, seconds, true}, chinese);
                check(no_face.title == (chinese ? L"未检测到人脸" : L"No face detected"));
                check(no_face.detail == (chinese ? L"请正对摄像头，" + countdown + L" 秒后重试"
                    : L"Face the camera; retrying in " + countdown + L"s"));
                const auto mismatch = caption({Phase::retry_delay, automatic, seconds, false}, chinese);
                check(mismatch.title == (chinese ? L"暂未通过验证" : L"Face not verified yet"));
                check(mismatch.detail == (chinese ? countdown + L" 秒后重试，也可使用密码"
                    : L"Retrying in " + countdown + L"s; you can use your password"));
            }
        }
    }
}

void outcome_bit_round_trip() {
    const auto retry = decode(0x00000534); // Rust: RetryDelay | automatic | no-face | 5s.
    check(retry.phase == Phase::retry_delay && retry.automatic && retry.last_no_face && retry.seconds == 5);
    check(encode(retry) == 0x00000534);
    const auto stopped = decode(0x38); // Rust: TimedOut | automatic | no-face.
    check(stopped.phase == Phase::timed_out && stopped.last_no_face && stopped.seconds == 0);
    check(decode(0x27).phase == Phase::no_match && decode(0x27).last_no_face);
    check(!decode(0x17).last_no_face);
    // Paint-cache keys must change even if the phase and countdown do not.
    check(encode({Phase::retry_delay, true, 5, true}) != encode({Phase::retry_delay, true, 5, false}));
    check(decode(0x45).phase == Phase::hidden);
    check(decode(0x85).phase == Phase::hidden);
}

} // namespace

int main() {
    using namespace su::status;
    retry_captions();
    countdown_captions();
    outcome_bit_round_trip();
    // Real wire examples from the producer, including the automatic bit and
    // full countdown range. Corrupted/reserved bits must never paint success.
    const auto delay = decode(0x00000312);
    check(delay.phase == Phase::initial_delay && delay.automatic && delay.seconds == 3);
    check(decode(0x00ffff14).seconds == 65535);
    check(decode(0x01000005).phase == Phase::hidden);
    check(decode(0x00000025).phase == Phase::ready && decode(0x00000025).last_no_face);
    check(decode(0x0000000f).phase == Phase::hidden);
    check(decode(5).phase == Phase::ready);
    check(decode(6).phase == Phase::submitting);
    // Dark and light backgrounds choose opposite foregrounds; a slow change
    // near the contrast crossover must preserve the previous choice.
    check(!use_dark_foreground(0.01, true, true));
    check(use_dark_foreground(0.95, false, true));
    for (double sample : {0.16, 0.18, 0.20, 0.17}) {
        check(use_dark_foreground(sample, true, false));
        check(!use_dark_foreground(sample, false, false));
    }
    check(!use_dark_foreground(0.13, true, false));
    check(use_dark_foreground(0.23, false, false));
    std::cout << "status protocol, captions and contrast tests passed\n";
}
