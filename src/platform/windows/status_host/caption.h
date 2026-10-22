#pragma once
#include "protocol.h"
#include <string>

namespace su::status {

// Pure presentation: shared by the status window and caption regression tests.
struct Text {
    std::wstring title;
    std::wstring detail;
};
inline Text caption(Snapshot state, bool chinese) {
    const auto retry = chinese
        ? L"按 Enter 重试，或输入密码" : L"Press Enter to retry, or enter your password";
    const auto no_face = chinese ? L"未检测到人脸" : L"No face detected";
    switch (state.phase) {
    case Phase::waiting: return chinese
        ? Text{L"按 Enter 开始人脸识别", L"也可点击登录箭头，或直接输入密码"}
        : Text{L"Press Enter to recognize your face", L"Or use the sign-in arrow or your password"};
    case Phase::initial_delay:
        if (!state.seconds) return chinese ? Text{L"正在启动人脸识别…", L"请看向摄像头"}
            : Text{L"Starting face recognition…", L"Look toward the camera"};
        return chinese ? Text{std::to_wstring(state.seconds) + L" 秒后开始人脸识别", L"也可直接输入密码"}
            : Text{L"Face recognition starts in " + std::to_wstring(state.seconds) + L"s", L"You can also enter your password"};
    case Phase::recognizing: return chinese ? Text{L"正在识别你…", L"请看向摄像头"}
        : Text{L"Looking for you…", L"Look toward the camera"};
    case Phase::retry_delay:
        if (state.last_no_face) return {no_face, chinese
            ? L"请正对摄像头，" + std::to_wstring(state.seconds) + L" 秒后重试"
            : L"Face the camera; retrying in " + std::to_wstring(state.seconds) + L"s"};
        return chinese ? Text{L"暂未通过验证", std::to_wstring(state.seconds) + L" 秒后重试，也可使用密码"}
        : Text{L"Face not verified yet", L"Retrying in " + std::to_wstring(state.seconds) + L"s; you can use your password"};
    case Phase::ready: return chinese ? Text{L"识别通过", L"正在准备登录"}
        : Text{L"Face verified", L"Preparing to sign in"};
    case Phase::submitting: return chinese ? Text{L"正在登录…", L""} : Text{L"Signing in…", L""};
    case Phase::no_match: return {state.last_no_face ? no_face : (chinese ? L"暂未通过验证" : L"Face not verified"), retry};
    case Phase::timed_out: return {state.last_no_face ? no_face : (chinese ? L"人脸识别已超时" : L"Face recognition timed out"), retry};
    case Phase::unavailable: return chinese ? Text{L"人脸识别暂不可用", L"按 Enter 重试，或使用密码登录"}
        : Text{L"Face recognition is unavailable", L"Press Enter to retry, or use your password"};
    case Phase::login_failed: return chinese ? Text{L"Windows 未接受登录", L"请使用密码，并检查保存的账户凭据"}
        : Text{L"Windows could not sign you in", L"Use your password and check your saved credentials"};
    default: return {};
    }
}

} // namespace su::status
