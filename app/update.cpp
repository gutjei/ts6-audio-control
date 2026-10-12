#include "update.h"
#include <QtWidgets>
#include <QThread>
#include <QVersionNumber>
#include <windows.h>
#include <winhttp.h>
#include <msi.h>
#include <msiquery.h>
#include <functional>
#include <stdexcept>

namespace {
constexpr auto assetName = "TeamSpeakAudioControl-windows-x64.msi";
constexpr auto upgradeCode = "{E81AE8B1-1D50-4D29-91AD-40BAA937128B}";
void fail(const QString& message) { throw std::runtime_error(message.toUtf8().constData()); }
struct Internet {
    HINTERNET handle;
    explicit Internet(HINTERNET value) : handle(value) { if(!handle) fail("Не удалось открыть HTTPS-соединение."); }
    ~Internet() { WinHttpCloseHandle(handle); }
};
void request(const QString& address, qint64 limit, const std::function<void(const QByteArray&)>& receive) {
    QUrl url(address);
    if(url.scheme() != "https" || !url.userInfo().isEmpty() || (url.port(443) != 443)) fail("Недопустимый адрес обновления.");
    Internet session(WinHttpOpen(L"TS6AudioControl/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0));
    WinHttpSetTimeouts(session.handle, 10000, 10000, 15000, 15000);
    const auto host = url.host().toStdWString();
    Internet connection(WinHttpConnect(session.handle, host.c_str(), 443, 0));
    const auto path = (url.path(QUrl::FullyEncoded) + (url.hasQuery() ? "?" + url.query(QUrl::FullyEncoded) : "")).toStdWString();
    Internet query(WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(query.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    if(!WinHttpSendRequest(query.handle, L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n", DWORD(-1), nullptr, 0, 0, 0)
        || !WinHttpReceiveResponse(query.handle, nullptr)) fail("Сеть недоступна или HTTPS-запрос завершился ошибкой. Повторите позже.");
    DWORD code = 0, size = sizeof(code);
    if(!WinHttpQueryHeaders(query.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &size, nullptr)) fail("Не удалось прочитать ответ сервера.");
    if(code == 404) fail("Опубликованных обновлений пока нет.");
    if(code == 403 || code == 429) fail("GitHub ограничил частоту запросов. Повторите позже.");
    if(code != 200) fail(QString("Ошибка сервера обновлений: HTTP %1.").arg(code));
    qint64 total = 0; QElapsedTimer timer; timer.start();
    for(;;) {
        if(QThread::currentThread()->isInterruptionRequested()) fail("Загрузка отменена.");
        if(timer.elapsed() > 300000) fail("Истекло время загрузки обновления.");
        QByteArray chunk(65536, Qt::Uninitialized); DWORD read = 0;
        if(!WinHttpReadData(query.handle, chunk.data(), DWORD(chunk.size()), &read)) fail("Загрузка прервана. Повторите позже.");
        if(!read) break;
        total += read; if(total > limit) fail("Ответ сервера превышает допустимый размер.");
        chunk.resize(read); receive(chunk);
    }
}
QString msiProperty(MSIHANDLE database, const wchar_t* query) {
    MSIHANDLE view = 0, record = 0;
    if(MsiDatabaseOpenViewW(database, query, &view) != ERROR_SUCCESS) fail("Повреждён MSI-пакет.");
    const auto result = MsiViewExecute(view, 0) == ERROR_SUCCESS ? MsiViewFetch(view, &record) : ERROR_INVALID_DATA;
    wchar_t value[256]{}; DWORD count = 255;
    const auto read = result == ERROR_SUCCESS ? MsiRecordGetStringW(record, 1, value, &count) : ERROR_INVALID_DATA;
    if(record) MsiCloseHandle(record);
    MsiCloseHandle(view);
    if(read != ERROR_SUCCESS) fail("Не удалось проверить MSI-пакет.");
    return QString::fromWCharArray(value);
}
void validateMsi(const QString& path, const QString& version) {
    MSIHANDLE database = 0;
    if(MsiOpenDatabaseW(path.toStdWString().c_str(), MSIDBOPEN_READONLY, &database) != ERROR_SUCCESS) fail("Загруженный файл не является MSI-пакетом.");
    try {
        if(msiProperty(database, L"SELECT `Value` FROM `Property` WHERE `Property` = 'UpgradeCode'").compare(upgradeCode, Qt::CaseInsensitive)
            || msiProperty(database, L"SELECT `Value` FROM `Property` WHERE `Property` = 'ProductVersion'") != version)
            fail("MSI-пакет не соответствует приложению или версии релиза.");
    } catch(...) { MsiCloseHandle(database); throw; }
    MsiCloseHandle(database);
}
}

ReleaseUpdate parseRelease(const QByteArray& json) {
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(json, &error);
    if(error.error != QJsonParseError::NoError || !doc.isObject()) fail("Некорректный ответ GitHub.");
    const auto object = doc.object();
    if(!object.contains("draft") || !object.contains("prerelease") || object["draft"].toBool(true) || object["prerelease"].toBool(true)) fail("Релиз не является стабильным.");
    const auto tag = object["tag_name"].toString();
    static const QRegularExpression versionPattern("^v(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$");
    const auto match = versionPattern.match(tag);
    if(!match.hasMatch() || match.captured(1).size() > 3 || match.captured(2).size() > 3 || match.captured(3).size() > 5
        || match.captured(1).toUInt() > 255 || match.captured(2).toUInt() > 255 || match.captured(3).toUInt() > 65535)
        fail("Версия релиза не поддерживается установщиком.");
    ReleaseUpdate result; result.version = tag.mid(1);
    if(QVersionNumber::fromString(result.version) <= QVersionNumber::fromString(QCoreApplication::applicationVersion())) return result;
    for(const auto& entry : object["assets"].toArray()) {
        const auto asset = entry.toObject(); if(asset["name"].toString() != assetName) continue;
        if(!result.url.isEmpty()) fail("Релиз содержит неоднозначные файлы обновления.");
        result.url = asset["browser_download_url"].toString(); result.digest = asset["digest"].toString().toLower(); result.size = asset["size"].toInteger();
        const auto expected = QString("https://github.com/gutjei/ts6-audio-control/releases/download/%1/%2").arg(tag, assetName);
        if(result.url != expected || !QRegularExpression("^sha256:[0-9a-f]{64}$").match(result.digest).hasMatch() || result.size <= 0 || result.size > 200 * 1024 * 1024)
            fail("У релиза нет корректного MSI-файла или контрольной суммы SHA-256.");
        result.digest.remove(0, 7);
    }
    if(result.url.isEmpty()) fail("Новая версия опубликована без MSI-установщика.");
    return result;
}
bool installedWithMsi() {
    if(!qEnvironmentVariableIsEmpty("TS6_PORTABLE_EXE")) return false;
    wchar_t directory[32768]{}; DWORD bytes = sizeof(directory);
    if(RegGetValueW(HKEY_CURRENT_USER, L"Software\\TS6AudioControl\\Installer", L"InstallDir", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                   nullptr, directory, &bytes) != ERROR_SUCCESS) return false;
    const auto path = QString::fromWCharArray(directory);
    return !path.isEmpty() && QFileInfo(path + "/TeamSpeakAudioControl.exe").canonicalFilePath().compare(QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath(), Qt::CaseInsensitive) == 0;
}
bool startUpdateHelper(const QString& msi, const QString& digest, const QString& dataDirectory, QString& error) {
    const auto helper = QFileInfo(msi).absolutePath() + "/audio_updater.exe";
    if(!QFile::copy(QCoreApplication::applicationDirPath() + "/audio_updater.exe", helper)) { error = "Не удалось подготовить помощник обновления."; return false; }
    if(!QProcess::startDetached(helper, {QString::number(GetCurrentProcessId()), QDir::toNativeSeparators(msi), digest,
            QDir::toNativeSeparators(QCoreApplication::applicationFilePath()), QDir::toNativeSeparators(dataDirectory)}, QFileInfo(msi).absolutePath())) {
        error = "Не удалось запустить установку обновления."; return false;
    }
    return true;
}
// A closing settings window disconnects the signals and cancels the request without
// waiting on the UI thread for a network timeout. The worker owns its remaining lifetime.
UpdateWorker::UpdateWorker() {
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] { requestInterruption(); wait(); });
}
UpdatePanel::UpdatePanel(QWidget* parent) : QWidget(parent) {
    auto layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel("Версия " + QCoreApplication::applicationVersion()));
    status = new QLabel("Проверка стабильных выпусков на GitHub Releases."); status->setWordWrap(true); status->setTextFormat(Qt::PlainText); layout->addWidget(status);
    check = new QPushButton("Проверить обновление"); check->setObjectName("checkUpdate"); layout->addWidget(check);
    install = new QPushButton("Обновить и перезапустить"); install->setObjectName("installUpdate"); install->setEnabled(false); layout->addWidget(install);
    if(!installedWithMsi()) {
        auto note = new QLabel("Установка обновлений из приложения доступна в MSI-версии. Для portable скачайте новую версию со страницы релизов."); note->setWordWrap(true); layout->addWidget(note);
        auto releases = new QPushButton("Открыть релизы"); layout->addWidget(releases);
        connect(releases, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl("https://github.com/gutjei/ts6-audio-control/releases")); });
    }
    layout->addStretch(); connect(check, &QPushButton::clicked, this, &UpdatePanel::checkRelease); connect(install, &QPushButton::clicked, this, &UpdatePanel::download);
}
UpdatePanel::~UpdatePanel() { if(worker) worker->requestInterruption(); }
void UpdatePanel::checkRelease() {
    if(worker) return;
    check->setEnabled(false); install->setEnabled(false); status->setText("Проверяю обновления…"); release = {};
    auto task = new UpdateWorker; worker = task;
    task->operation = [](UpdateWorker* job) {
        try {
            QByteArray json; request("https://api.github.com/repos/gutjei/ts6-audio-control/releases/latest", 1024 * 1024, [&json](const QByteArray& part) { json += part; });
            const auto result = parseRelease(json);
            emit job->releaseFound(result.version, result.url, result.digest, result.size);
        } catch(const std::exception& e) { emit job->statusChanged(QString::fromUtf8(e.what())); }
    };
    connect(task, &UpdateWorker::statusChanged, status, &QLabel::setText);
    connect(task, &UpdateWorker::releaseFound, this, [this](const QString& version, const QString& url, const QString& digest, qint64 size) {
        release = {version, url, digest, size};
        const bool newer = QVersionNumber::fromString(version) > QVersionNumber::fromString(QCoreApplication::applicationVersion());
        status->setText(newer ? "Доступна версия " + version : "Установлена актуальная версия.");
        install->setEnabled(newer && installedWithMsi());
    });
    connect(task, &QThread::finished, this, [this] { worker = nullptr; check->setEnabled(true); });
    connect(task, &QThread::finished, task, &QObject::deleteLater); task->start();
}
void UpdatePanel::download() {
    if(worker || release.url.isEmpty() || !installedWithMsi()) return;
    if(QMessageBox::question(this, "Обновление", "Приложение остановит захват звука, установит обновление и запустится снова. После обновления перезапустите TeamSpeak. Продолжить?") != QMessageBox::Yes) return;
    check->setEnabled(false); install->setEnabled(false); status->setText("Скачиваю MSI…");
    const auto selected = release;
    auto task = new UpdateWorker; worker = task;
    task->operation = [selected](UpdateWorker* job) {
        QString directory;
        try {
            QTemporaryDir temporary(QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/TS6-update-XXXXXX");
            if(!temporary.isValid()) fail("Не удалось создать каталог обновления.");
            directory = temporary.path(); const auto path = directory + "/update.msi";
            QSaveFile file(path); if(!file.open(QIODevice::WriteOnly)) fail("Не удалось создать файл обновления.");
            QCryptographicHash hash(QCryptographicHash::Sha256); qint64 received = 0;
            request(selected.url, selected.size, [&](const QByteArray& part) {
                if(file.write(part) != part.size()) fail("Не удалось сохранить обновление. Проверьте свободное место.");
                hash.addData(part); received += part.size();
                emit job->statusChanged(QString("Скачиваю MSI… %1%").arg(received * 100 / selected.size));
            });
            if(received != selected.size || hash.result().toHex() != selected.digest.toLatin1()) fail("Контрольная сумма обновления не совпала. Установка отменена.");
            if(!file.commit()) fail("Не удалось сохранить MSI-пакет.");
            validateMsi(path, selected.version);
            if(QThread::currentThread()->isInterruptionRequested()) fail("Загрузка отменена.");
            temporary.setAutoRemove(false);
            emit job->packageReady(path, selected.digest);
        } catch(const std::exception& e) { emit job->statusChanged(QString::fromUtf8(e.what())); }
    };
    connect(task, &UpdateWorker::statusChanged, status, &QLabel::setText);
    connect(task, &UpdateWorker::packageReady, this, [this](const QString& path, const QString& digest) {
        status->setText("Обновление проверено. Завершаю работу…"); emit installReady(path, digest);
    });
    connect(task, &QThread::finished, this, [this] { worker = nullptr; check->setEnabled(true); install->setEnabled(installedWithMsi()); });
    connect(task, &QThread::finished, task, &QObject::deleteLater); task->start();
}
