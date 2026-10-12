#pragma once
#include <QWidget>
#include <QString>
#include <QThread>
#include <functional>
class QLabel; class QPushButton;
struct ReleaseUpdate {
    QString version, url, digest;
    qint64 size = 0;
};
ReleaseUpdate parseRelease(const QByteArray& json);
bool installedWithMsi();
bool startUpdateHelper(const QString& msi, const QString& digest, const QString& dataDirectory, QString& error);
class UpdateWorker final : public QThread {
    Q_OBJECT
public:
    UpdateWorker();
    std::function<void(UpdateWorker*)> operation;
    void run() override { operation(this); }
signals:
    void statusChanged(QString message);
    void releaseFound(QString version, QString url, QString digest, qint64 size);
    void packageReady(QString path, QString digest);
};
class UpdatePanel final : public QWidget {
    Q_OBJECT
    QLabel* status;
    QPushButton *check, *install;
    QThread* worker = nullptr;
    ReleaseUpdate release;
    void checkRelease();
    void download();
public:
    explicit UpdatePanel(QWidget* parent = nullptr);
    ~UpdatePanel() override;
signals:
    void installReady(QString package, QString digest);
};
