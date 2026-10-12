#pragma once
#include "backend.h"
#include <QMainWindow>
#include <QTimer>
#include <memory>
class QCheckBox; class QSlider; class QSpinBox; class QLabel; class QTableWidget;
class QTableWidgetItem; class QPushButton; class QLineEdit; class QSystemTrayIcon;
class Window final : public QMainWindow {
    Q_OBJECT
    struct Row {
        QCheckBox* check; QSlider* slider; QSpinBox* spin; QLabel* name; QLabel* reason;
        QTableWidgetItem* anchor; QWidget* cells[4];
    };
    Settings settings;
    QString settingsFile;
    QString updatePackage, updateDigest;
    std::unique_ptr<Backend> backend;
    QMap<QString, Row> rows;
    ViewState snapshot;
    bool ready = false, connected = false, busy = false, closing = false, stopped = false, exitRequested = false;
    QTimer rulesTimer, volumeTimer, autoTimer;
    QMap<QString, int> pendingVolumes;
    QLabel *status, *stream, *summary, *notice, *ruleError, *empty;
    QTableWidget* table = nullptr;
    QPushButton* connectButton;
    QLineEdit *include, *exclude;
    QSystemTrayIcon* tray = nullptr;
    bool commit(QJsonObject candidate);
    void saveVolumes();
    void applyRules();
    void applyTheme();
    void showSettings();
    void refreshRows();
    void updateConnection();
    void scheduleAutomatic();
    void toggleConnection();
    void restoreWindow();
protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
public:
    Window(Settings settings, QString directory, const QString& initialError = {});
    ~Window() override;
};
