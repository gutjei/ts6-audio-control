#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cerrno>
#include <cstdlib>
#include <string>
// No audio endpoint, interpreter, GUI toolkit or child processes in the clock.
int wmain(int argc, wchar_t** argv) {
    DWORD parentPid = 0;
    const wchar_t* ready = nullptr;
    for(int i = 1; i < argc; i += 2) {
        if(i + 1 >= argc) return 2;
        if(wcscmp(argv[i], L"--ready-file") == 0 && !ready) ready = argv[i + 1];
        else if(wcscmp(argv[i], L"--parent") == 0 && !parentPid) {
            wchar_t* end = nullptr; errno = 0;
            parentPid = wcstoul(argv[i + 1], &end, 10);
            if(errno || !parentPid || *end || parentPid == GetCurrentProcessId()) return 2;
        } else return 2;
    }
    HANDLE parent = parentPid ? OpenProcess(SYNCHRONIZE, FALSE, parentPid) : nullptr;
    if(parentPid && !parent) return 3;
    if(ready) {
        std::wstring temporary = std::wstring(ready) + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if(file == INVALID_HANDLE_VALUE) return 1;
        auto pid = std::to_string(GetCurrentProcessId()); DWORD written = 0;
        BOOL ok = WriteFile(file, pid.data(), static_cast<DWORD>(pid.size()), &written, nullptr);
        CloseHandle(file);
        if(!ok || written != pid.size() || !MoveFileExW(temporary.c_str(), ready, MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str()); return 2;
        }
    }
    if(parent) { WaitForSingleObject(parent, INFINITE); CloseHandle(parent); }
    else Sleep(INFINITE);
    return 0;
}
