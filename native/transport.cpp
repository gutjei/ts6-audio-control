#include "transport_protocol.h"
#include <tlhelp32.h>
#include <algorithm>
#include <memory>
#include <vector>
using namespace transport;
#define API extern "C" __declspec(dllexport)

static std::vector<MODULEENTRY32W> modules(DWORD pid) {
    for(unsigned retry = 0; retry < 100; ++retry) {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        if(snapshot.value == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if(error != ERROR_BAD_LENGTH) throw error;
        } else {
            MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
            std::vector<MODULEENTRY32W> result;
            if(Module32FirstW(snapshot, &entry)) {
                do { result.push_back(entry); } while(Module32NextW(snapshot, &entry));
                return result;
            }
            DWORD error = GetLastError();
            if(error != ERROR_NO_MORE_FILES && error != ERROR_BAD_LENGTH) throw error;
        }
        // A just-created diagnostic worker may not have reached its loader yet.
        Sleep(10);
    }
    throw DWORD(ERROR_TIMEOUT);
}
static uintptr_t remoteModule(DWORD pid, const wchar_t* name, const wchar_t* exactPath = nullptr) {
    for(const auto& module : modules(pid)) if(_wcsicmp(module.szModule, name) == 0) {
        if(exactPath && _wcsicmp(module.szExePath, exactPath) != 0) throw DWORD(ERROR_REVISION_MISMATCH);
        return reinterpret_cast<uintptr_t>(module.modBaseAddr);
    }
    return 0;
}
static DWORD remoteCall(HANDLE process, uintptr_t address, void* argument, DWORD& result) {
    Handle thread(CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(address), argument, 0, nullptr));
    if(!thread.value) return GetLastError();
    DWORD wait = WaitForSingleObject(thread, 10000);
    if(wait != WAIT_OBJECT_0) return wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    return GetExitCodeThread(thread, &result) ? ERROR_SUCCESS : GetLastError();
}
static DWORD inject(HANDLE process, DWORD pid, const wchar_t* path) {
    const wchar_t* basename = wcsrchr(path, L'\\'); basename = basename ? basename + 1 : path;
    uintptr_t base = remoteModule(pid, basename, path);
    if(!base) {
        // LoadLibraryW may be forwarded to KernelBase. Resolve its owning module
        // and use that module's base in the target, never a local ASLR address.
        auto load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HMODULE owner = nullptr;
        if(!load || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(load), &owner)) return ERROR_PROC_NOT_FOUND;
        wchar_t ownerPath[MAX_PATH]{};
        if(!GetModuleFileNameW(owner, ownerPath, MAX_PATH)) return GetLastError();
        const wchar_t* ownerName = wcsrchr(ownerPath, L'\\');
        uintptr_t remoteOwner = remoteModule(pid, ownerName ? ownerName + 1 : ownerPath);
        if(!remoteOwner) return ERROR_MOD_NOT_FOUND;
        const SIZE_T size = (wcslen(path) + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(process, nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if(!remote) return GetLastError();
        SIZE_T written = 0;
        DWORD result = 0, error = ERROR_SUCCESS;
        if(!WriteProcessMemory(process, remote, path, size, &written) || written != size) error = GetLastError();
        else error = remoteCall(process, remoteOwner + reinterpret_cast<uintptr_t>(load) - reinterpret_cast<uintptr_t>(owner), remote, result);
        // A timed-out remote thread may still read its argument. Leave that small
        // allocation resident until target exit rather than creating a UAF.
        if(error != ERROR_TIMEOUT) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        if(error) return error;
        base = remoteModule(pid, basename, path); // Thread exit code truncates HMODULE on x64.
        if(!base) return ERROR_DLL_INIT_FAILED;
    }
    HMODULE local = LoadLibraryExW(path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if(!local) return GetLastError();
    auto start = GetProcAddress(local, "RouterAgentStart");
    uintptr_t offset = reinterpret_cast<uintptr_t>(start) - reinterpret_cast<uintptr_t>(local);
    FreeLibrary(local);
    if(!start) return ERROR_PROC_NOT_FOUND;
    DWORD result = 0;
    DWORD error = remoteCall(process, base + offset, nullptr, result);
    return error ? error : result;
}
struct Connection {
    Handle process, pipe;
    explicit Connection(HANDLE p) : process(p), pipe(INVALID_HANDLE_VALUE) {}
};
static DWORD request(Connection* connection, Request& data, Response& reply) {
    if(!connection || connection->pipe.value == INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
    DWORD error = transfer(connection->pipe, &data, sizeof(data), true);
    if(!error) error = transfer(connection->pipe, &reply, sizeof(reply), false);
    if(!error && (reply.magic != magic || reply.version != version)) error = ERROR_REVISION_MISMATCH;
    if(!error) error = reply.error;
    if(error) { CloseHandle(connection->pipe.value); connection->pipe.value = INVALID_HANDLE_VALUE; }
    return error;
}
API DWORD TransportFindTarget(DWORD* pid) {
    if(!pid) return ERROR_INVALID_PARAMETER;
    *pid = 0;
    try {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if(snapshot.value == INVALID_HANDLE_VALUE) return GetLastError();
        PROCESSENTRY32W p{}; p.dwSize = sizeof(p);
        if(!Process32FirstW(snapshot, &p)) return GetLastError();
        std::vector<PROCESSENTRY32W> candidates;
        do { if(_wcsicmp(p.szExeFile, L"TeamSpeak.exe") == 0) candidates.push_back(p); } while(Process32NextW(snapshot, &p));
        for(const auto& candidate : candidates) {
            bool child = std::any_of(candidates.begin(), candidates.end(), [&](const auto& other) { return other.th32ProcessID == candidate.th32ParentProcessID; });
            if(child) continue;
            if(*pid) return ERROR_MORE_DATA;
            *pid = candidate.th32ProcessID;
        }
        return *pid ? ERROR_SUCCESS : ERROR_NOT_FOUND;
    } catch(...) { return ERROR_OUTOFMEMORY; }
}
API DWORD TransportOpen(DWORD pid, const wchar_t* path, DWORD silentPid, BOOL diagnostic, void** out) {
    if(!path || !out || !pid || !silentPid || pid == GetCurrentProcessId()) return ERROR_INVALID_PARAMETER;
    *out = nullptr;
    try {
        HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
        if(!process) return GetLastError();
        auto connection = std::make_unique<Connection>(process);
        USHORT machine = 0, native = 0;
        if(!IsWow64Process2(process, &machine, &native)) return GetLastError();
        if(machine != IMAGE_FILE_MACHINE_UNKNOWN || native != IMAGE_FILE_MACHINE_AMD64) return ERROR_BAD_EXE_FORMAT;
        if(!diagnostic && !remoteModule(pid, L"TeamSpeak.dll")) return ERROR_MOD_NOT_FOUND;
        for(const auto& module : modules(pid)) {
            std::wstring name = module.szModule;
            std::transform(name.begin(), name.end(), name.begin(), towlower);
            if(name.find(L"frida") != std::wstring::npos) return ERROR_BUSY;
        }
        DWORD error = inject(process, pid, path);
        if(error) return error;
        auto name = pipeName(pid);
        connection->pipe.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if(connection->pipe.value == INVALID_HANDLE_VALUE) return GetLastError();
        ULONG server = 0;
        if(!GetNamedPipeServerProcessId(connection->pipe, &server) || server != pid) return ERROR_ACCESS_DENIED;
        DWORD mode = PIPE_READMODE_MESSAGE;
        if(!SetNamedPipeHandleState(connection->pipe, &mode, nullptr, nullptr)) return GetLastError();
        Request hello; hello.command = Hello; hello.silentPid = silentPid; hello.diagnostic = diagnostic ? 1u : 0u;
        Response reply;
        error = request(connection.get(), hello, reply);
        if(error) return error;
        *out = connection.release(); return ERROR_SUCCESS;
    } catch(DWORD error) { return error; }
    catch(...) { return ERROR_OUTOFMEMORY; }
}
API DWORD TransportRequest(void* handle, Request* data, Response* reply) {
    if(!data || !reply) return ERROR_INVALID_PARAMETER;
    return request(static_cast<Connection*>(handle), *data, *reply);
}
API BOOL TransportAlive(void* handle) {
    auto connection = static_cast<Connection*>(handle);
    return connection && connection->pipe.value != INVALID_HANDLE_VALUE && WaitForSingleObject(connection->process, 0) == WAIT_TIMEOUT;
}
API void TransportClose(void* handle) { delete static_cast<Connection*>(handle); }
