#include "runtime.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <stdexcept>
QString applicationDirectory() { return QCoreApplication::applicationDirPath(); }
QString windowsError(DWORD error) {
    switch(error) {
        case ERROR_ACCESS_DENIED: return "Нет доступа. Запустите панель и TeamSpeak с одинаковыми правами.";
        case ERROR_PIPE_BUSY: return "К TeamSpeak уже подключена другая панель.";
        case ERROR_REVISION_MISMATCH: return "Загружена другая версия микшера. Перезапустите TeamSpeak.";
        case ERROR_BUSY: return "Обнаружена Frida. Завершите эксперимент и перезапустите TeamSpeak.";
        case ERROR_MOD_NOT_FOUND: return "Аудиомодуль TeamSpeak ещё не загружен или отсутствует DLL.";
        case ERROR_NOT_FOUND: return "Запустите TeamSpeak и повторите подключение.";
        case ERROR_MORE_DATA: return "Запустите только один экземпляр TeamSpeak.";
    }
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    QString text = message ? QString::fromWCharArray(message).trimmed() : "Ошибка Windows";
    LocalFree(message); return text + QString(" (%1)").arg(error);
}
void checked(DWORD error) { if(error) throw std::runtime_error(windowsError(error).toUtf8().constData()); }
QString cachedMixer(const QString& directory) {
    QFile source(applicationDirectory() + "/audio_mixer.dll");
    if(!source.open(QIODevice::ReadOnly)) throw std::runtime_error(source.errorString().toUtf8().constData());
    const auto bytes = source.readAll();
    const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    const QString destination = directory + "/native/" + digest + "/audio_mixer.dll";
    if(!QDir().mkpath(QFileInfo(destination).absolutePath())) throw std::runtime_error("Не удалось создать каталог микшера.");
    QFile existing(destination);
    if(existing.exists()) {
        if(!existing.open(QIODevice::ReadOnly) || existing.readAll() != bytes) throw std::runtime_error("Повреждён кеш микшера.");
    } else {
        QSaveFile output(destination);
        if(!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
            throw std::runtime_error(output.errorString().toUtf8().constData());
    }
    return QDir::toNativeSeparators(destination);
}
void ClockProcess::start() {
    stop();
    auto command = ("\"" + QDir::toNativeSeparators(applicationDirectory() + "/audio_clock.exe") +
        "\" --parent " + QString::number(GetCurrentProcessId())).toStdWString();
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    if(!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) checked(GetLastError());
    CloseHandle(process.hThread); handle = process.hProcess; pid = process.dwProcessId;
}
void ClockProcess::stop() {
    if(handle) { if(WaitForSingleObject(handle, 0) == WAIT_TIMEOUT) TerminateProcess(handle, 0); WaitForSingleObject(handle, 5000); CloseHandle(handle); }
    handle = nullptr; pid = 0;
}
static const QString runKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
bool startupEnabled() { return !QSettings(runKey, QSettings::NativeFormat).value("TeamSpeakAudioControl").toString().isEmpty(); }
void setStartup(bool enabled) {
    QSettings registry(runKey, QSettings::NativeFormat);
    if(enabled) {
        const auto executable = qEnvironmentVariable("TS6_PORTABLE_EXE", QCoreApplication::applicationFilePath());
        const auto command = '"' + QDir::toNativeSeparators(executable) + '"';
        if(command.size() > 260) throw std::runtime_error("Путь слишком длинный для автозапуска Windows.");
        registry.setValue("TeamSpeakAudioControl", command);
    } else registry.remove("TeamSpeakAudioControl");
    registry.sync(); if(registry.status() != QSettings::NoError) throw std::runtime_error("Не удалось изменить автозапуск Windows.");
}
