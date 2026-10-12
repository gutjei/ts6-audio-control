#include "window.h"
#include "runtime.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <stdexcept>
int diagnose(const QString& output, const QString& directory);
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("TeamSpeakAudioControl");
    app.setApplicationVersion("native-qt");
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.setApplicationDescription("TeamSpeak Audio Control — C++ / Qt Widgets");
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({"data-dir", "Settings and native cache directory", "directory"});
    parser.addOption({"diagnose", "Run self-diagnostics against a child process and save JSON", "file"});
    parser.process(app);
    QString directory = parser.value("data-dir");
    if(directory.isEmpty()) directory = qEnvironmentVariable("LOCALAPPDATA") + "/TS6AudioControl";
    directory = QDir(directory).absolutePath();
    if(parser.isSet("diagnose")) return diagnose(parser.value("diagnose"), directory);
    try {
        if(!QDir().mkpath(directory)) throw std::runtime_error("Не удалось создать каталог настроек.");
        QLockFile lock(directory + "/controller.lock");
        if(!lock.tryLock()) throw std::runtime_error("Панель уже запущена с этим каталогом настроек.");
        Settings settings = Settings::defaults(); QString error;
        const auto path = directory + "/settings.json";
        const auto portable = QFileInfo(qEnvironmentVariable("TS6_PORTABLE_EXE", QCoreApplication::applicationFilePath())).absolutePath() + "/settings.json";
        try {
            if(QFileInfo::exists(path)) settings = Settings::load(path);
            else if(QFileInfo::exists(portable)) {
                settings = Settings::load(portable); settings.save(path);
            }
        } catch(const std::exception& e) { error = "Не удалось прочитать настройки: " + QString::fromUtf8(e.what()) + " Используются значения по умолчанию."; }
        Window window(settings, directory, error); window.show();
        return app.exec();
    } catch(const std::exception& e) {
        QMessageBox::critical(nullptr, "TeamSpeak Audio Control", QString::fromUtf8(e.what())); return 1;
    }
}
