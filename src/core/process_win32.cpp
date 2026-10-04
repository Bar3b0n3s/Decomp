#ifdef _WIN32

#include "core/process.hpp"
#include "core/strings.hpp"

#include <windows.h>

#include <cwchar>
#include <map>
#include <memory>
#include <thread>

namespace decomp {
namespace {

struct CaseInsensitiveLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
};

// Builds a sorted, double-NUL-terminated UTF-16 environment block (CREATE_UNICODE_ENVIRONMENT).
std::wstring build_env_block(const ProcessSpec& spec) {
    std::map<std::wstring, std::wstring, CaseInsensitiveLess> env;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += std::wcslen(p) + 1) {
            std::wstring_view entry(p);
            auto eq = entry.find(L'=', 1);  // per-drive entries look like "=C:=C:\dir"
            if (eq == std::wstring_view::npos) continue;
            env[std::wstring(entry.substr(0, eq))] = std::wstring(entry.substr(eq + 1));
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& [key, value] : spec.env) {
        auto wkey = utf8_to_wide(key);
        if (value) env[wkey] = utf8_to_wide(*value);
        else env.erase(wkey);
    }
    std::wstring block;
    for (const auto& [k, v] : env) {
        block += k;
        block += L'=';
        block += v;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

class OwnedHandle {
public:
    OwnedHandle() = default;
    explicit OwnedHandle(HANDLE h) : h_(h) {}
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    ~OwnedHandle() { reset(); }
    HANDLE get() const { return h_; }
    HANDLE* out() { return &h_; }
    void reset() {
        if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = nullptr;
    }

private:
    HANDLE h_ = nullptr;
};

std::string last_error_message() {
    DWORD code = GetLastError();
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::string msg = text ? wide_to_utf8(text) : std::string();
    if (text) LocalFree(text);
    return std::format("error {}: {}", code, std::string(trim(msg)));
}

void read_all(HANDLE h, std::string& out) {
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
}

} // namespace

Result<ProcessResult> run_process(const ProcessSpec& spec) {
    if (spec.argv.empty()) return make_error(ErrorCode::invalid_argument, "run_process: empty argv");

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    OwnedHandle out_r, out_w, err_r, err_w, in_r, in_w;
    if (!CreatePipe(out_r.out(), out_w.out(), &sa, 0) || !CreatePipe(err_r.out(), err_w.out(), &sa, 0) ||
        !CreatePipe(in_r.out(), in_w.out(), &sa, 0))
        return make_error(ErrorCode::process, "CreatePipe failed: {}", last_error_message());
    SetHandleInformation(out_r.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_w.get(), HANDLE_FLAG_INHERIT, 0);

    // Only the three child pipe ends are inherited, so concurrent spawns don't leak handles into each other.
    HANDLE inherit[3] = {in_r.get(), out_w.get(), err_w.get()};
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    auto attr_storage = std::make_unique<std::byte[]>(attr_size);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.get());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size))
        return make_error(ErrorCode::process, "InitializeProcThreadAttributeList failed: {}", last_error_message());
    struct AttrGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST a;
        ~AttrGuard() { DeleteProcThreadAttributeList(a); }
    } attr_guard{attrs};
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr))
        return make_error(ErrorCode::process, "UpdateProcThreadAttribute failed: {}", last_error_message());

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_r.get();
    si.StartupInfo.hStdOutput = out_w.get();
    si.StartupInfo.hStdError = err_w.get();
    si.lpAttributeList = attrs;

    std::wstring cmd = utf8_to_wide(build_windows_command_line(spec.argv));
    std::wstring env = build_env_block(spec);
    std::wstring cwd = spec.cwd.empty() ? std::wstring() : spec.cwd.wstring();

    OwnedHandle job(CreateJobObjectW(nullptr, nullptr));
    if (job.get()) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    auto start = std::chrono::steady_clock::now();
    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, flags, env.data(),
                        cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi))
        return make_error(ErrorCode::process, "cannot start '{}': {}", spec.argv[0], last_error_message());
    OwnedHandle process(pi.hProcess), thread(pi.hThread);
    if (job.get()) AssignProcessToJobObject(job.get(), process.get());
    ResumeThread(thread.get());

    // Close our copies of the child's ends so reads see EOF when the child exits.
    out_w.reset();
    err_w.reset();
    in_r.reset();

    if (!spec.stdin_data.empty()) {
        DWORD written = 0;
        WriteFile(in_w.get(), spec.stdin_data.data(), static_cast<DWORD>(spec.stdin_data.size()), &written, nullptr);
    }
    in_w.reset();

    ProcessResult result;
    std::thread out_reader([&] { read_all(out_r.get(), result.out); });
    std::thread err_reader([&] { read_all(err_r.get(), result.err); });

    // Waits are sliced so that cancellation is noticed within about 100 ms.
    while (WaitForSingleObject(process.get(), 100) == WAIT_TIMEOUT) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        const bool expired = spec.timeout.count() > 0 && elapsed >= spec.timeout;
        const bool cancel = !expired && spec.cancelled && spec.cancelled();
        if (!expired && !cancel) continue;
        (expired ? result.timed_out : result.cancelled) = true;
        if (job.get()) TerminateJobObject(job.get(), 1);
        else TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), 5000);
        break;
    }
    // Kill anything the child left behind that still holds our pipes open.
    if (job.get()) TerminateJobObject(job.get(), 0);
    out_reader.join();
    err_reader.join();

    DWORD code = 0;
    GetExitCodeProcess(process.get(), &code);
    result.exit_code = static_cast<int>(code);
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    return result;
}

} // namespace decomp

#endif // _WIN32
