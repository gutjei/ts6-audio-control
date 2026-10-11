#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <string>

namespace transport {
constexpr uint32_t magic = 0x36534154, version = 1;
enum Command : uint32_t { Hello = 1, Configure, Heartbeat, Stats, Shutdown, SelfTest };
struct Request {
    uint32_t magic = transport::magic, version = transport::version;
    uint32_t command = 0, count = 0, silentPid = 0, diagnostic = 0;
    DWORD pids[64]{};
    float gains[64]{};
};
struct Response {
    uint32_t magic = transport::magic, version = transport::version;
    uint32_t error = 0, reserved = 0;
    uint64_t stats[16]{};
};
static_assert(sizeof(Request) == 536 && sizeof(Response) == 144, "IPC ABI");
inline std::wstring pipeName(DWORD pid) {
    return L"\\\\.\\pipe\\TS6AudioControl.Native.v1." + std::to_wstring(pid);
}
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if(value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    operator HANDLE() const { return value; }
};
// Every operation has a deadline. Cancel and drain before releasing OVERLAPPED.
inline DWORD transfer(HANDLE pipe, void* data, DWORD size, bool writing, DWORD timeout = 2500) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if(!event.value) return GetLastError();
    OVERLAPPED ov{}; ov.hEvent = event;
    DWORD done = 0;
    BOOL ok = writing ? WriteFile(pipe, data, size, &done, &ov) : ReadFile(pipe, data, size, &done, &ov);
    if(!ok) {
        DWORD error = GetLastError();
        if(error != ERROR_IO_PENDING) return error;
        DWORD wait = WaitForSingleObject(event, timeout);
        if(wait != WAIT_OBJECT_0) {
            error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            CancelIoEx(pipe, &ov); GetOverlappedResult(pipe, &ov, &done, TRUE);
            return error;
        }
        if(!GetOverlappedResult(pipe, &ov, &done, FALSE)) return GetLastError();
    }
    return done == size ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}
}
