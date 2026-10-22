#include "request_log.h"
#undef NDEBUG
#include <cassert>
#include <string>

using namespace su::windows::auth_service;
using smile2unlock::logon_secret_ipc::Status;

int main() {
    auto output = std::string{};
    const auto logger = su::log::Logger{
        "test", su::log::Level::info,
        [&](su::log::Level, std::string_view line) -> std::expected<void, su::log::SinkError> {
            output += line;
            return {};
        }};
    {
        auto trace = RequestLog{logger.with({su::log::field("request_id", 42),
                                            su::log::field("op", "authenticate")})};
        trace.completed(Status::kAuthenticationFailed);
        trace.completed(Status::kOk);
    }
    assert(output.contains("event=request.completed"));
    assert(output.contains("request_id=42 op=authenticate status=authentication_failed"));
    assert(output.contains("duration_ms="));
    assert(output.find('\n') == output.size() - 1);
    output.clear();
    try {
        const auto trace = RequestLog{logger};
        throw 42;
    } catch (int) {
    }
    assert(output.contains("status=unavailable"));
    assert(output.find('\n') == output.size() - 1);
}
