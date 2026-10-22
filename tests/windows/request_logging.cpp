#include "request_log.h"
#undef NDEBUG
#include <cassert>

using namespace su::windows::auth_service;
using smile2unlock::logon_secret_ipc::Status;
using smile2unlock::logon_secret_ipc::Operation;

int main() {
    const auto fields = completion_fields(42, Operation::kAuthenticateAndPrepare,
                                          Status::kAuthenticationFailed, 135, "S-1-5-18");
    const auto record = su::log::Record{su::log::Timestamp{}, 0, su::log::Level::info,
                                       "test", "request.completed", 1, 2, fields};
    const auto output = su::log::format_record(record);
    assert(output.contains("event=request.completed request_id=42 op=authenticate"));
    assert(output.contains("status=authentication_failed duration_ms=135 sid=S-1-5-18"));
    assert(output.find('\n') == output.size() - 1);
    assert(operation_name(Operation::kStore) == "store");
    assert(operation_name(static_cast<Operation>(0xffff)) == "unknown");
    assert(status_name(Status::kUnavailable) == "unavailable");
    assert(status_name(static_cast<Status>(0xffff)) == "unknown");
}
