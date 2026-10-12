#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <stdexcept>

namespace {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if(value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\""; size_t slashes = 0;
    for(wchar_t c : value) {
        if(c == L'\\') { ++slashes; continue; }
        if(c == L'"') result.append(slashes * 2 + 1, L'\\'); else result.append(slashes, L'\\');
        slashes = 0; result += c;
    }
    result.append(slashes * 2, L'\\'); return result + L'"';
}
void fail(const wchar_t* message) { throw std::wstring(message); }
void verify(HANDLE file, const std::wstring& expected) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) fail(L"Не удалось проверить SHA-256.");
    auto status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if(status < 0) { BCryptCloseAlgorithmProvider(algorithm, 0); fail(L"Не удалось проверить SHA-256."); }
    std::vector<BYTE> buffer(65536); DWORD count = 0; bool ok = true;
    for(;;) {
        if(!ReadFile(file, buffer.data(), DWORD(buffer.size()), &count, nullptr)) { ok = false; break; }
        if(!count) break;
        if(BCryptHashData(hash, buffer.data(), count, 0) < 0) { ok = false; break; }
    }
    BYTE digest[32]{};
    if(BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) ok = false;
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    const wchar_t* digits = L"0123456789abcdef"; std::wstring actual;
    for(BYTE b : digest) { actual += digits[b >> 4]; actual += digits[b & 15]; }
    if(!ok || actual != expected) fail(L"MSI-файл изменился после загрузки. Обновление отменено.");
}
bool launch(const std::wstring& exe, std::wstring command, PROCESS_INFORMATION& process) {
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    return CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) != FALSE;
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    int count = 0; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if(!args || count != 6) { if(args) LocalFree(args); return 2; }
    const std::wstring parentText = args[1], package = args[2], digest = args[3], app = args[4], data = args[5]; LocalFree(args);
    DWORD exitCode = 1;
    try {
        wchar_t* end = nullptr; const auto pid = wcstoul(parentText.c_str(), &end, 10);
        if(!pid || !end || *end || digest.size() != 64) fail(L"Некорректные параметры обновления.");
        Handle parent(OpenProcess(SYNCHRONIZE, FALSE, pid));
        if(parent.value) {
            if(WaitForSingleObject(parent.value, 60000) != WAIT_OBJECT_0) fail(L"Приложение не завершилось. Закройте его и повторите обновление.");
        } else if(GetLastError() != ERROR_INVALID_PARAMETER) fail(L"Не удалось дождаться завершения приложения.");
        // Deny new launches while MSI replaces the application; the OS removes this lock on exit/crash.
        Handle lock(CreateFileW((data + L"\\update.lock").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
        if(lock.value == INVALID_HANDLE_VALUE) fail(L"Другое обновление уже выполняется.");
        Handle file(CreateFileW(package.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if(file.value == INVALID_HANDLE_VALUE) fail(L"Не удалось открыть MSI-пакет.");
        verify(file.value, digest);
        wchar_t system[MAX_PATH]{}; if(!GetSystemDirectoryW(system, MAX_PATH)) fail(L"Не найден Windows Installer.");
        const std::wstring exe = std::wstring(system) + L"\\msiexec.exe";
        const auto log = package + L".log";
        const auto command = quote(exe) + L" /i " + quote(package) + L" /passive /norestart REBOOT=ReallySuppress /L*v " + quote(log);
        PROCESS_INFORMATION process{};
        if(!launch(exe, command, process)) fail(L"Не удалось запустить Windows Installer.");
        Handle running(process.hProcess), thread(process.hThread);
        if(WaitForSingleObject(running.value, INFINITE) != WAIT_OBJECT_0 || !GetExitCodeProcess(running.value, &exitCode)) fail(L"Не удалось получить результат установки.");
        if(exitCode != ERROR_SUCCESS && exitCode != ERROR_SUCCESS_REBOOT_REQUIRED)
            throw std::wstring(L"Обновление не установлено. Код Windows Installer: ") + std::to_wstring(exitCode) + L".\nЖурнал: " + log;
        if(exitCode == ERROR_SUCCESS_REBOOT_REQUIRED)
            MessageBoxW(nullptr, L"Обновление установлено. Для завершения требуется перезагрузка Windows.", L"TeamSpeak Audio Control", MB_OK | MB_ICONINFORMATION);
    } catch(const std::wstring& message) {
        MessageBoxW(nullptr, message.c_str(), L"Обновление TeamSpeak Audio Control", MB_OK | MB_ICONERROR);
    } catch(...) {
        MessageBoxW(nullptr, L"Не удалось завершить обновление.", L"TeamSpeak Audio Control", MB_OK | MB_ICONERROR);
    }
    // Restart the new version on success, or the existing/rolled-back version on installation failure.
    PROCESS_INFORMATION process{};
    if(launch(app, quote(app) + L" --data-dir " + quote(data), process)) { CloseHandle(process.hThread); CloseHandle(process.hProcess); }
    else MessageBoxW(nullptr, L"Запустите TeamSpeak Audio Control через меню Пуск.", L"Обновление", MB_OK | MB_ICONINFORMATION);
    if(exitCode == ERROR_SUCCESS || exitCode == ERROR_SUCCESS_REBOOT_REQUIRED) DeleteFileW(package.c_str());
    return int(exitCode);
}
