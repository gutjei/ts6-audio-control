#pragma once
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVector>
struct Process { quint32 pid = 0, parent = 0; QString name; };
struct Settings {
    QJsonObject json;
    QRegularExpression include, exclude;
    QMap<QString, bool> overrides;
    QMap<QString, double> volumes;
    bool automatic = false, tray = false;
    bool showExcluded = true;
    QString theme = "system";
    static Settings defaults();
    static Settings parse(const QJsonObject& json);
    static Settings load(const QString& path);
    void save(const QString& path) const;
    bool selected(const QString& name) const;
    bool denied(const QString& name) const;
};
struct Selection { QVector<quint32> pids; QVector<float> gains; QMap<QString, QStringList> blocked; };
Selection resolve(const QVector<Process>& processes, const Settings& settings, const QSet<quint32>& own,
    const QSet<quint32>& audioPids);
