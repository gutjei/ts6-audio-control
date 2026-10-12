#include "settings.h"
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <stdexcept>
static void invalid(const QString& message) { throw std::runtime_error(message.toUtf8().constData()); }
static QRegularExpression expression(const QString& text) {
    QRegularExpression result(text, QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    if(!result.isValid()) invalid("Ошибка правила: " + result.errorString());
    return result;
}
Settings Settings::defaults() { return parse({{"include", "^firefox\\.exe$"}, {"exclude", "^teamspeak\\.exe$"}}); }
Settings Settings::parse(const QJsonObject& source) {
    Settings s; s.json = source;
    for(const auto* key : {"include", "exclude"}) if(!source[key].isString()) invalid("Правила должны быть строками.");
    s.include = expression(source["include"].toString()); s.exclude = expression(source["exclude"].toString());
    for(const auto* key : {"auto_connect", "minimize_to_tray"})
        if(source.contains(key) && !source[key].isBool()) invalid("Настройка должна быть true/false: " + QString(key));
    s.automatic = source["auto_connect"].toBool(); s.tray = source["minimize_to_tray"].toBool();
    if(source.contains("theme")) {
        s.theme = source["theme"].toString();
        if(s.theme != "system" && s.theme != "dark" && s.theme != "light") invalid("Неизвестная тема оформления.");
    }
    for(const auto* key : {"overrides", "volumes"}) {
        if(source.contains(key) && !source[key].isObject()) invalid("Настройки приложений должны быть объектом.");
        const auto entries = source[key].toObject(); QJsonObject normalized;
        for(auto i = entries.begin(); i != entries.end(); ++i) {
            const auto name = i.key().toLower();
            if(name.isEmpty() || name.contains('/') || name.contains('\\')) invalid("Используйте имя exe, а не путь.");
            if(QString(key) == "overrides") {
                if(!i.value().isBool()) invalid("Ручной выбор должен быть true/false.");
                s.overrides[name] = i.value().toBool();
            } else {
                const auto value = i.value().toDouble(-1);
                if(!i.value().isDouble() || !std::isfinite(value) || value < 0 || value > 200) invalid("Громкость должна быть от 0 до 200%.");
                s.volumes[name] = value;
            }
            normalized[name] = i.value();
        }
        s.json[key] = normalized;
    }
    return s;
}
Settings Settings::load(const QString& path) {
    QFile file(path); if(!file.open(QIODevice::ReadOnly)) invalid(file.errorString());
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    if(error.error != QJsonParseError::NoError || !doc.isObject()) invalid("Неверный JSON настроек: " + error.errorString());
    return parse(doc.object());
}
void Settings::save(const QString& path) const {
    QSaveFile file(path); if(!file.open(QIODevice::WriteOnly)) invalid(file.errorString());
    const auto data = QJsonDocument(json).toJson(QJsonDocument::Indented);
    if(file.write(data) != data.size() || !file.commit()) invalid(file.errorString());
}
bool Settings::denied(const QString& name) const {
    return name.compare("teamspeak.exe", Qt::CaseInsensitive) == 0 ||
        (!exclude.pattern().isEmpty() && exclude.match(name).hasMatch());
}
bool Settings::selected(const QString& name) const {
    if(denied(name)) return false;
    const auto key = name.toLower();
    if(overrides.contains(key)) return overrides[key];
    return !include.pattern().isEmpty() && include.match(name).hasMatch();
}
Selection resolve(const QVector<Process>& rows, const Settings& s, const QSet<quint32>& own) {
    QMap<quint32, Process> byPid; QSet<quint32> chosen;
    for(const auto& p : rows) { byPid[p.pid] = p; if(!own.contains(p.pid) && s.selected(p.name)) chosen.insert(p.pid); }
    auto ancestors = [&](quint32 pid) {
        QSet<quint32> seen{pid}, result;
        while(byPid.contains(pid)) {
            pid = byPid[pid].parent;
            if(!pid || seen.contains(pid)) break;
            seen.insert(pid); result.insert(pid);
        }
        return result;
    };
    Selection result;
    auto sorted = chosen.values(); std::sort(sorted.begin(), sorted.end());
    for(auto root : sorted) {
        if(ancestors(root).intersects(chosen)) continue;
        QSet<QString> conflicts;
        for(const auto& p : rows) if(ancestors(p.pid).contains(root)) {
            const auto name = p.name.toLower();
            if(s.denied(name) || own.contains(p.pid) || (s.overrides.contains(name) && !s.overrides[name])) conflicts.insert(name);
        }
        if(!conflicts.isEmpty()) { auto names = conflicts.values(); std::sort(names.begin(), names.end()); result.blocked[byPid[root].name.toLower()] = names; }
        else { result.pids.append(root); result.gains.append(float(s.volumes.value(byPid[root].name.toLower(), 100) / 100)); }
    }
    if(result.pids.size() > 64) { result.pids.clear(); result.gains.clear(); result.blocked["*"] = {"Выбрано больше 64 деревьев процессов"}; }
    return result;
}
