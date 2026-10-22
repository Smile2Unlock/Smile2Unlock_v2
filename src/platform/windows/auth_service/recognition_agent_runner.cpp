#include "server_helpers.h"
#include "recognition_agent_protocol.h"
#include "recognition_result.h"
#include <bcrypt.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <array>
#include <filesystem>

namespace su::windows::auth_service::detail {
namespace {
using AgentRequest = smile2unlock::recognition_agent_ipc::Request;
using AgentResponse = smile2unlock::recognition_agent_ipc::Response;
using Result = std::expected<void, DWORD>;

bool write_exact(HANDLE handle, const void* input, DWORD size) {
    const auto* bytes = static_cast<const std::byte*>(input);
    DWORD total = 0;
    while (total < size) {
        DWORD written = 0;
        if (!WriteFile(handle, bytes + total, size - total, &written, nullptr) || !written)
            return false;
        total += written;
    }
    return true;
}
std::filesystem::path agent_path() {
    auto path = std::wstring(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
        return {};
    path.resize(length);
    return std::filesystem::path{path}.parent_path() / L"Smile2UnlockRecognitionAgent.exe";
}
struct ProcessAttributes {
    std::vector<std::byte> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
    ~ProcessAttributes() {
        if (list)
            DeleteProcThreadAttributeList(list);
    }
    Result initialize(std::span<HANDLE> handles) {
        SIZE_T bytes = 0;
        (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        storage.resize(bytes);
        auto* candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(candidate, 1, 0, &bytes))
            return std::unexpected(GetLastError());
        list = candidate;
        if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                       handles.size_bytes(), nullptr, nullptr))
            return std::unexpected(GetLastError());
        return {};
    }
};
struct AgentRun {
    HANDLE stop;
    HANDLE client;
    std::uint32_t requested_session;
    const RecognitionSettings& settings;
    RequestLog& trace;
    ScopedHandle child_read{}, service_write{}, service_read{}, child_write{}, null_error{},
        session{}, process{};
    AgentRequest request{};
    AgentResponse response{};
    ~AgentRun() {
        SecureZeroMemory(&request, sizeof(request));
        SecureZeroMemory(&response, sizeof(response));
    }
    Result pipes() {
        auto inheritable = SECURITY_ATTRIBUTES{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE cr = nullptr, sw = nullptr, sr = nullptr, cw = nullptr;
        const auto created = CreatePipe(&cr, &sw, &inheritable, sizeof(AgentRequest)) &&
                             CreatePipe(&sr, &cw, &inheritable, sizeof(AgentResponse));
        const auto error = GetLastError();
        child_read.reset(cr);
        service_write.reset(sw);
        service_read.reset(sr);
        child_write.reset(cw);
        if (!created)
            return std::unexpected(error);
        if (!SetHandleInformation(sw, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(sr, HANDLE_FLAG_INHERIT, 0))
            return std::unexpected(GetLastError());
        null_error.reset(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (null_error.get() == INVALID_HANDLE_VALUE)
            return std::unexpected(GetLastError());
        return {};
    }
    Result token() {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(),
                              TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY, &raw))
            return std::unexpected(GetLastError());
        const auto parent = ScopedHandle{raw};
        if (!DuplicateTokenEx(raw,
                              TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY |
                                  TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                              nullptr, SecurityImpersonation, TokenPrimary, &raw))
            return std::unexpected(GetLastError());
        session.reset(raw);
        auto id = requested_session ? requested_session : WTSGetActiveConsoleSessionId();
        if (!id || id == 0xFFFF'FFFF)
            return std::unexpected(ERROR_NO_SUCH_LOGON_SESSION);
        if (!SetTokenInformation(raw, TokenSessionId, &id, sizeof(id)))
            return std::unexpected(GetLastError());
        return {};
    }
    Result launch() {
        const auto path = agent_path();
        const auto attributes = GetFileAttributesW(path.c_str());
        if (path.empty() || attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            return std::unexpected(ERROR_FILE_NOT_FOUND);
        if (const auto ready = pipes(); !ready)
            return ready;
        if (const auto ready = token(); !ready)
            return ready;
        auto inherited = std::array{child_read.get(), child_write.get(), null_error.get()};
        auto process_attributes = ProcessAttributes{};
        if (const auto ready = process_attributes.initialize(inherited); !ready)
            return ready;
        auto startup = STARTUPINFOEXW{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = child_read.get();
        startup.StartupInfo.hStdOutput = child_write.get();
        startup.StartupInfo.hStdError = null_error.get();
        startup.lpAttributeList = process_attributes.list;
        auto command = L"\"" + path.wstring() + L"\"";
        void* raw_environment = nullptr;
        if (!CreateEnvironmentBlock(&raw_environment, session.get(), FALSE))
            return std::unexpected(GetLastError());
        const auto environment = std::unique_ptr<void, decltype(&DestroyEnvironmentBlock)>{
            raw_environment, &DestroyEnvironmentBlock};
        auto created = PROCESS_INFORMATION{};
        const auto directory = path.parent_path();
        if (!CreateProcessAsUserW(
                session.get(), path.c_str(), command.data(), nullptr, nullptr, TRUE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                environment.get(), directory.c_str(), &startup.StartupInfo, &created))
            return std::unexpected(GetLastError());
        process.reset(created.hProcess);
        const auto thread = ScopedHandle{created.hThread};
        child_read.reset();
        child_write.reset();
        null_error.reset();
        return {};
    }
    void terminate(DWORD code) {
        trace.causes.push_back(log::field("exit_code", code));
        (void)TerminateProcess(process.get(), code);
        (void)WaitForSingleObject(process.get(), 3000);
    }
    Result begin() {
        if (BCryptGenRandom(nullptr, request.nonce.data(), static_cast<ULONG>(request.nonce.size()),
                            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            terminate(ERROR_GEN_FAILURE);
            return std::unexpected(ERROR_GEN_FAILURE);
        }
        request.camera_index = settings.camera_index;
        request.timeout_ms = std::clamp<std::uint32_t>(settings.timeout_ms, 5'000, 30'000);
        request.liveness_threshold =
            settings.liveness_detection ? settings.liveness_threshold : 0.0F;
        if (!write_exact(service_write.get(), &request, sizeof(request))) {
            const auto error = GetLastError();
            terminate(ERROR_WRITE_FAULT);
            return std::unexpected(error);
        }
        service_write.reset();
        return {};
    }
    std::expected<DWORD, DWORD> read_available(DWORD available, std::size_t received) {
        const auto count =
            static_cast<DWORD>(std::min<std::size_t>(available, sizeof(response) - received));
        DWORD bytes = 0;
        if (!ReadFile(service_read.get(), reinterpret_cast<std::byte*>(&response) + received, count,
                      &bytes, nullptr) ||
            !bytes) {
            const auto error = GetLastError();
            terminate(error);
            return std::unexpected(error);
        }
        return bytes;
    }
    Result receive() {
        auto received = std::size_t{0};
        const auto deadline = GetTickCount64() + request.timeout_ms + 3000;
        const auto watch_client = client && client != INVALID_HANDLE_VALUE;
        while (received < sizeof(response)) {
            if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 ||
                (watch_client && WaitForSingleObject(client, 0) == WAIT_OBJECT_0)) {
                terminate(ERROR_CANCELLED);
                return std::unexpected(ERROR_CANCELLED);
            }
            DWORD available = 0;
            if (!PeekNamedPipe(service_read.get(), nullptr, 0, nullptr, &available, nullptr)) {
                const auto error = GetLastError();
                terminate(error);
                return std::unexpected(error);
            }
            if (available) {
                const auto read = read_available(available, received);
                if (!read)
                    return std::unexpected(read.error());
                received += *read;
                continue;
            }
            if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
                DWORD code = 0;
                if (GetExitCodeProcess(process.get(), &code))
                    trace.causes.push_back(log::field("exit_code", code));
                return std::unexpected(ERROR_HANDLE_EOF);
            }
            const auto now = GetTickCount64();
            if (now >= deadline) {
                terminate(ERROR_TIMEOUT);
                return std::unexpected(ERROR_TIMEOUT);
            }
            const auto slice = static_cast<DWORD>(std::min<ULONGLONG>(25, deadline - now));
            const HANDLE events[]{stop, client};
            if (watch_client)
                (void)WaitForMultipleObjects(2, events, FALSE, slice);
            else
                (void)WaitForSingleObject(stop, slice);
        }
        return {};
    }
    std::expected<std::vector<float>, DWORD> finish() {
        DWORD code = STILL_ACTIVE;
        if (WaitForSingleObject(process.get(), 3000) != WAIT_OBJECT_0 ||
            !GetExitCodeProcess(process.get(), &code)) {
            terminate(ERROR_GEN_FAILURE);
            return std::unexpected(ERROR_GEN_FAILURE);
        }
        trace.cause(response.status);
        trace.causes.push_back(log::field("exit_code", code));
        if (const auto error = validate_agent_result(request, response, code);
            error != ERROR_SUCCESS)
            return std::unexpected(error);
        return std::vector<float>{response.feature.begin(),
                                  response.feature.begin() + response.feature_count};
    }
};
} // namespace

std::expected<std::vector<float>, DWORD> run_recognition_agent(HANDLE stop, HANDLE client,
                                                               std::uint32_t session,
                                                               const RecognitionSettings& settings,
                                                               RequestLog& trace) {
    auto run = AgentRun{stop, client, session, settings, trace};
    if (const auto ready = run.launch(); !ready)
        return std::unexpected(ready.error());
    if (const auto ready = run.begin(); !ready)
        return std::unexpected(ready.error());
    if (const auto ready = run.receive(); !ready)
        return std::unexpected(ready.error());
    return run.finish();
}
} // namespace su::windows::auth_service::detail
