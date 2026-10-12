#pragma once
#include "settings.h"
#include <QObject>
#include <atomic>
#include <mutex>
#include <thread>
struct Inventory;
struct ViewState {
    QVector<Process> processes;
    QMap<QString, int> audio;
    Selection selection;
};
Q_DECLARE_METATYPE(ViewState)
class Backend final : public QObject {
    Q_OBJECT
    Inventory* inventory = nullptr;
    std::thread worker;
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    Settings settings;
    quint64 revision = 0;
    int command = 0;
    QString directory;
    void run();
public:
    Backend(const Settings& settings, QString dataDirectory);
    ~Backend();
    void start();
    void updateSettings(const Settings& settings);
    void connectTarget(bool automatic = false);
    void disconnectTarget();
    void stop();
    void refresh();
signals:
    void snapshot(ViewState state);
    void connection(bool connected, bool busy, QString message);
    void statistics(QVector<qulonglong> counters);
    void error(QString message);
    void stopped();
};
