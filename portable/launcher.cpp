#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <setupapi.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace fs = std::filesystem;
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if(value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
};
static void require(bool ok, const char* message) { if(!ok) throw std::runtime_error(message); }
static std::wstring wide(const std::string& value) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
    require(size > 0, "Invalid UTF-8 path"); std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), result.data(), size); return result;
}
static std::string digest(const void* data, size_t size) {
    require(size <= MAXDWORD, "Payload too large");
    BYTE hash[32];
    require(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data, ULONG(size), hash, sizeof(hash)) >= 0, "SHA-256 failed");
    static const char hex[] = "0123456789abcdef"; std::string result;
    for(auto byte : hash) { result += hex[byte >> 4]; result += hex[byte & 15]; } return result;
}
static std::string resource(int id) {
    auto module = GetModuleHandleW(nullptr);
    auto info = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    require(info != nullptr, "Missing embedded payload");
    auto bytes = LockResource(LoadResource(module, info)); DWORD size = SizeofResource(module, info);
    require(bytes && size, "Invalid embedded payload"); return {static_cast<const char*>(bytes), size};
}
static fs::path safeRelative(const std::string& name) {
    auto path = fs::path(wide(name));
    require(!path.empty() && !path.has_root_directory() && !path.has_root_name() && name.find(':') == std::string::npos, "Unsafe payload path");
    for(const auto& part : path) require(part != L".." && part != L".", "Unsafe payload path");
    return path;
}
struct Entry { fs::path relative; std::string hash; };
using Manifest = std::map<std::wstring, Entry>;
static Manifest parseManifest(const std::string& text) {
    Manifest entries; std::istringstream lines(text); std::string line; std::set<fs::path> names;
    while(std::getline(lines, line)) {
        if(!line.empty() && line.back() == '\r') line.pop_back();
        auto first = line.find(' '), second = line.find(' ', first + 1);
        require(first != std::string::npos && second != std::string::npos, "Invalid payload manifest");
        auto id = wide(line.substr(0, first)); auto hash = line.substr(first + 1, second - first - 1);
        auto path = safeRelative(line.substr(second + 1));
        require(hash.size() == 64 && hash.find_first_not_of("0123456789abcdef") == std::string::npos, "Invalid payload hash");
        require(names.insert(path).second && entries.emplace(id, Entry{path, hash}).second, "Duplicate payload file");
    }
    require(!entries.empty(), "Empty payload"); return entries;
}
static bool validDirectory(const fs::path& directory, const Manifest& entries) {
    if(!fs::is_directory(directory)) return false;
    if(GetFileAttributesW(directory.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) return false;
    size_t count = 0;
    for(const auto& item : fs::recursive_directory_iterator(directory)) {
        if(GetFileAttributesW(item.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) return false;
        if(item.is_regular_file()) ++count;
    }
    if(count != entries.size()) return false;
    for(const auto& [id, entry] : entries) {
        std::ifstream stream(directory / entry.relative, std::ios::binary);
        if(!stream) return false;
        const std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        if(stream.bad() || digest(bytes.data(), bytes.size()) != entry.hash) return false;
    }
    return true;
}
struct Extraction {
    fs::path directory; const Manifest& entries; std::set<std::wstring> seen;
    static UINT CALLBACK callback(PVOID context, UINT notification, UINT_PTR param1, UINT_PTR) noexcept {
        auto& self = *static_cast<Extraction*>(context);
        try {
            if(notification == SPFILENOTIFY_FILEINCABINET) {
                auto info = reinterpret_cast<FILE_IN_CABINET_INFO_W*>(param1);
                const auto found = self.entries.find(info->NameInCabinet);
                require(found != self.entries.end() && self.seen.insert(info->NameInCabinet).second, "Unexpected cabinet entry");
                const auto target = self.directory / found->second.relative;
                fs::create_directories(target.parent_path());
                require(target.native().size() < MAX_PATH, "Cache path too long");
                wcscpy_s(info->FullTargetName, target.c_str()); return FILEOP_DOIT;
            }
            if(notification == SPFILENOTIFY_FILEEXTRACTED) return reinterpret_cast<FILEPATHS_W*>(param1)->Win32Error;
            if(notification == SPFILENOTIFY_NEEDNEWCABINET) return ERROR_INVALID_DATA;
            return NO_ERROR;
        } catch(...) { SetLastError(ERROR_INVALID_DATA); return notification == SPFILENOTIFY_FILEINCABINET ? FILEOP_ABORT : ERROR_INVALID_DATA; }
    }
};
static fs::path prepare(const std::string& manifestText, const Manifest& entries) {
    PWSTR local = nullptr;
    require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)), "Local AppData unavailable");
    fs::path root = fs::path(local) / L"TS6AudioControl" / L"portable"; CoTaskMemFree(local);
    fs::create_directories(root);
    const auto version = wide(digest(manifestText.data(), manifestText.size())); const auto destination = root / version;
    const auto identity = root.wstring() + version;
    const auto mutexName = L"Local\\TS6Portable-" + wide(digest(identity.data(), identity.size() * sizeof(wchar_t)));
    Handle mutex(CreateMutexW(nullptr, FALSE, mutexName.c_str())); require(mutex.value != nullptr, "Cannot lock portable cache");
    auto wait = WaitForSingleObject(mutex.value, 60000);
    require(wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED, "Portable cache is busy");
    struct Unlock { HANDLE h; ~Unlock() { ReleaseMutex(h); } } unlock{mutex.value};
    if(fs::exists(destination)) {
        require(validDirectory(destination, entries), "Portable cache is damaged. Close the application and delete its version folder under %LOCALAPPDATA%\\TS6AudioControl\\portable, then try again.");
        return destination;
    }
    GUID guid; require(SUCCEEDED(CoCreateGuid(&guid)), "Cannot create extraction directory");
    wchar_t name[40]; StringFromGUID2(guid, name, 40); auto staging = root / (std::wstring(L"stage-") + name);
    require(fs::create_directory(staging), "Cannot create extraction directory");
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); } } cleanup{staging};
    const auto cabinet = staging / L"payload.cab";
    { const auto bytes = resource(101); std::ofstream file(cabinet, std::ios::binary); file.write(bytes.data(), bytes.size()); file.close(); require(!file.fail(), "Cannot write embedded cabinet"); }
    Extraction state{staging, entries, {}};
    require(SetupIterateCabinetW(cabinet.c_str(), 0, Extraction::callback, &state) != FALSE, "Cannot extract embedded cabinet");
    fs::remove(cabinet); require(validDirectory(staging, entries), "Extracted payload failed SHA-256 verification");
    require(MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "Cannot publish portable cache");
    return destination;
}
static std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\""; size_t slashes = 0;
    for(auto ch : value) {
        if(ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\'); slashes = 0; result += ch;
    }
    result.append(slashes * 2, L'\\'); return result + L'"';
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    bool diagnostic = false; fs::path diagnosticFile;
    try {
        SetDllDirectoryW(L"");
        int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        require(argv != nullptr, "Cannot read command line");
        struct FreeArgs { LPWSTR* p; ~FreeArgs() { LocalFree(p); } } args{argv};
        for(int i = 1; i < argc; ++i) if(std::wstring(argv[i]) == L"--diagnose") {
            diagnostic = true; if(i + 1 < argc) diagnosticFile = argv[i + 1];
        }
        const auto manifest = resource(102); const auto directory = prepare(manifest, parseManifest(manifest));
        std::vector<wchar_t> ownPath(32768);
        const auto length = GetModuleFileNameW(nullptr, ownPath.data(), DWORD(ownPath.size()));
        require(length && length < ownPath.size(), "Cannot resolve launcher path");
        require(SetEnvironmentVariableW(L"TS6_PORTABLE_EXE", ownPath.data()) != FALSE, "Cannot configure launcher path");
        const auto executable = directory / L"TeamSpeakAudioControl.exe"; auto command = quote(executable.wstring());
        for(int i = 1; i < argc; ++i) command += L" " + quote(argv[i]);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
        require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) != FALSE, "Cannot start application");
        Handle child(process.hProcess), thread(process.hThread);
        require(WaitForSingleObject(child.value, INFINITE) == WAIT_OBJECT_0, "Cannot wait for application");
        DWORD code = 1; require(GetExitCodeProcess(child.value, &code) != FALSE, "Cannot read application result"); return int(code);
    } catch(const std::exception& e) {
        if(diagnostic && !diagnosticFile.empty()) {
            std::ofstream report(diagnosticFile, std::ios::binary);
            report << "{\"ok\":false,\"runtime\":\"portable launcher\",\"error\":\"";
            for(const char* p = e.what(); *p; ++p) {
                if(*p == '"' || *p == '\\') report << '\\';
                report << (static_cast<unsigned char>(*p) < 32 ? ' ' : *p);
            }
            report << "\"}\n";
        }
        std::wstring message = L"Portable application could not start.";
        try { message = wide(e.what()); } catch(...) {}
        if(!diagnostic) MessageBoxW(nullptr, message.c_str(), L"TeamSpeak Audio Control — Portable", MB_OK | MB_ICONERROR);
        OutputDebugStringW(message.c_str()); return 2;
    }
}
