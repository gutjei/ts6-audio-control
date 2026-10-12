#include "window.h"
#include "runtime.h"
#include "update.h"
#include <QtWidgets>
#include <algorithm>
#include <cmath>
class ResetSlider : public QSlider {
public:
    ResetSlider() : QSlider(Qt::Horizontal) { setRange(0, 200); }
    void mouseDoubleClickEvent(QMouseEvent* event) override { setValue(100); event->accept(); }
};
static QIcon applicationIcon() {
    QPixmap pixmap(64, 64); pixmap.fill(Qt::transparent);
    QPainter p(&pixmap); p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen);
    p.setBrush(QColor("#2864a6")); p.drawRoundedRect(1, 1, 62, 62, 14, 14);
    p.setBrush(Qt::white);
    for(int i = 0; i < 5; ++i) { int h = (i == 2 ? 38 : (i % 2 ? 26 : 14)); p.drawRoundedRect(12 + i * 9, (64 - h) / 2, 5, h, 2, 2); }
    return QIcon(pixmap);
}
static QLabel* label(const QString& text, const char* object = "") { auto p = new QLabel(text); p->setObjectName(object); return p; }
Window::Window(Settings value, QString directory, const QString& initialError)
    : settings(std::move(value)), settingsFile(directory + "/settings.json") {
    qApp->setStyle(QStyleFactory::create("Fusion"));
    setWindowTitle("TeamSpeak · Звук трансляции"); resize(940, 620); setMinimumSize(780, 520);
    setWindowIcon(applicationIcon());
    auto page = new QWidget; page->setObjectName("page"); setCentralWidget(page);
    auto layout = new QVBoxLayout(page); layout->setContentsMargins(24, 20, 24, 20); layout->setSpacing(8);
    auto header = new QHBoxLayout;
    header->addWidget(label("Звук трансляции", "title")); header->addStretch();
    status = label("Не подключено", "status"); header->addWidget(status);
    connectButton = new QPushButton("Подключить"); connectButton->setObjectName("primary");
    header->addWidget(connectButton);
    auto gear = new QToolButton; gear->setText(QString::fromUtf8("⚙")); gear->setFixedSize(40, 40);
    gear->setToolTip("Настройки"); gear->setAccessibleName("Настройки"); header->addWidget(gear);
    layout->addLayout(header);
    stream = label("", "muted"); stream->setWordWrap(true); stream->hide(); layout->addWidget(stream);
    auto toolbar = new QHBoxLayout; toolbar->addWidget(label("Приложения", "section")); toolbar->addStretch();
    summary = label("Загрузка…", "muted"); toolbar->addWidget(summary); layout->addLayout(toolbar);
    table = new QTableWidget(0, 4); table->setHorizontalHeaderLabels({"Захват", "Приложение", "Громкость", "Ограничение"});
    table->setColumnHidden(3, true); table->verticalHeader()->hide(); table->verticalHeader()->setDefaultSectionSize(52);
    table->setShowGrid(false); table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->setFocusPolicy(Qt::NoFocus);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed); table->setColumnWidth(0, 76);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    layout->addWidget(table, 1);
    empty = label("Загрузка аудиосессий…", "muted"); empty->setAlignment(Qt::AlignCenter); empty->setWordWrap(true); layout->addWidget(empty);
    auto rulesToggle = new QToolButton; rulesToggle->setText("Автоматические правила"); rulesToggle->setCheckable(true);
    rulesToggle->setChecked(true); rulesToggle->setArrowType(Qt::DownArrow); rulesToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    layout->addWidget(rulesToggle, 0, Qt::AlignLeft);
    auto rules = new QFrame; rules->setObjectName("rules"); auto grid = new QGridLayout(rules);
    grid->setContentsMargins(16, 12, 16, 12); grid->setHorizontalSpacing(16);
    include = new QLineEdit(settings.json["include"].toString()); exclude = new QLineEdit(settings.json["exclude"].toString());
    int column = 0;
    for(auto field : {include, exclude}) {
        auto caption = label(column ? "Всегда исключать" : "Автоматически включать", "muted"); caption->setBuddy(field);
        field->setAccessibleName(caption->text()); field->setPlaceholderText("Регулярное выражение по имени exe");
        field->setToolTip("Без учёта регистра. Пустое поле ни с чем не совпадает.");
        grid->addWidget(caption, 0, column); grid->addWidget(field, 1, column); grid->setColumnStretch(column++, 1);
        connect(field, &QLineEdit::textChanged, this, [this] { rulesTimer.start(); });
    }
    auto hint = label("Исключения важнее ручного выбора. TeamSpeak исключён всегда.", "muted"); hint->setWordWrap(true); grid->addWidget(hint, 2, 0);
    auto reset = new QPushButton("Вернуть выбор к правилам"); grid->addWidget(reset, 2, 1, Qt::AlignRight);
    ruleError = label("", "error"); ruleError->setWordWrap(true); ruleError->hide(); grid->addWidget(ruleError, 3, 0, 1, 2);
    layout->addWidget(rules);
    notice = label(initialError, "error"); notice->setWordWrap(true); notice->setVisible(!initialError.isEmpty()); layout->addWidget(notice);
    backendNotice = label("", "error"); backendNotice->setWordWrap(true); backendNotice->hide(); layout->addWidget(backendNotice);
    connect(rulesToggle, &QToolButton::toggled, this, [rules, rulesToggle](bool visible) { rules->setVisible(visible); rulesToggle->setArrowType(visible ? Qt::DownArrow : Qt::RightArrow); });
    connect(reset, &QPushButton::clicked, this, [this] { auto next = settings.json; next["overrides"] = QJsonObject{}; commit(next); });
    connect(gear, &QToolButton::clicked, this, &Window::showSettings);
    connect(connectButton, &QPushButton::clicked, this, &Window::toggleConnection);
    rulesTimer.setSingleShot(true); rulesTimer.setInterval(350); connect(&rulesTimer, &QTimer::timeout, this, &Window::applyRules);
    volumeTimer.setSingleShot(true); volumeTimer.setInterval(120); connect(&volumeTimer, &QTimer::timeout, this, &Window::saveVolumes);
    autoTimer.setSingleShot(true); connect(&autoTimer, &QTimer::timeout, this, [this] {
        if(settings.automatic && !connected && !busy && !closing) { busy = true; updateConnection(); backend->connectTarget(true); }
    });
    tray = new QSystemTrayIcon(windowIcon(), this); tray->setToolTip(windowTitle());
    auto menu = new QMenu(this); menu->addAction("Открыть", this, &Window::restoreWindow);
    menu->addAction("Выйти", this, [this] { exitRequested = true; close(); }); tray->setContextMenu(menu);
    connect(tray, &QSystemTrayIcon::activated, this, [this](auto reason) {
        if(reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) restoreWindow();
    });
    if(QSystemTrayIcon::isSystemTrayAvailable()) tray->show();
    applyTheme(); connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [this] { if(settings.theme == "system") applyTheme(); });
    backend = std::make_unique<Backend>(settings, directory);
    connect(backend.get(), &Backend::snapshot, this, [this](ViewState state) { snapshot = std::move(state); ready = true; refreshRows(); });
    connect(backend.get(), &Backend::connection, this, [this](bool active, bool working, const QString& message) {
        connected = active; busy = working; status->setText(working ? "Подключение…" : active ? "Подключено" : "Не подключено");
        if(active) { backendNotice->clear(); backendNotice->hide(); }
        stream->setText(message); stream->show(); updateConnection(); scheduleAutomatic();
    });
    connect(backend.get(), &Backend::error, this, [this](const QString& message) { backendNotice->setText(message); backendNotice->show(); });
    connect(backend.get(), &Backend::statistics, this, [this](const QVector<qulonglong>& stats) {
        if(stats.size() != 16) return;
        if(stats[4]) { backendNotice->setText(QString("Ошибки микшера: %1; код 0x%2").arg(stats[4]).arg(stats[5], 8, 16, QChar('0'))); backendNotice->show(); }
        if(stats[0] == 0) { stream->setText("Начните трансляцию со звуком после подключения панели. Уже идущую перезапустите."); stream->show(); }
        else { stream->setText("Поток подключён"); stream->hide(); }
    });
    connect(backend.get(), &Backend::stopped, this, [this] {
        stopped = true; tray->hide();
        if(!updatePackage.isEmpty()) {
            QString error;
            if(!startUpdateHelper(updatePackage, updateDigest, QFileInfo(settingsFile).absolutePath(), error))
                QMessageBox::critical(this, "Обновление", error + " Запустите приложение и повторите попытку.");
        }
        close(); qApp->quit();
    });
    backend->start(); updateConnection(); scheduleAutomatic();
}
Window::~Window() = default;
bool Window::commit(QJsonObject candidate) {
    try {
        auto volumes = candidate["volumes"].toObject();
        for(auto i = pendingVolumes.begin(); i != pendingVolumes.end(); ++i) volumes[i.key()] = i.value();
        candidate["volumes"] = volumes;
        auto clean = Settings::parse(candidate); clean.save(settingsFile); settings = std::move(clean);
        pendingVolumes.clear(); volumeTimer.stop(); backend->updateSettings(settings); refreshRows(); return true;
    } catch(const std::exception& e) { notice->setText("Настройки не сохранены: " + QString::fromUtf8(e.what())); notice->show(); return false; }
}
void Window::saveVolumes() { commit(settings.json); }
void Window::applyRules() {
    rulesTimer.stop(); auto next = settings.json; next["include"] = include->text(); next["exclude"] = exclude->text();
    try { Settings::parse(next); } catch(const std::exception& e) { ruleError->setText(QString::fromUtf8(e.what()) + " Действуют последние сохранённые правила."); ruleError->show(); return; }
    if(commit(next)) ruleError->hide();
}
void Window::applyTheme() {
    const bool dark = settings.theme == "dark" || (settings.theme == "system" && qApp->styleHints()->colorScheme() == Qt::ColorScheme::Dark);
    QPalette palette = qApp->style()->standardPalette();
    if(dark) {
        for(auto role : {QPalette::Window, QPalette::Button, QPalette::Base}) palette.setColor(role, QColor("#292d33"));
        for(auto role : {QPalette::WindowText, QPalette::ButtonText, QPalette::Text}) palette.setColor(role, QColor("#e4e7eb"));
        palette.setColor(QPalette::Highlight, QColor("#6b9ed8")); palette.setColor(QPalette::HighlightedText, QColor("#202328"));
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#9099a5"));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#9099a5"));
    }
    qApp->setPalette(palette);
    QFile file(":/app/style.qss"); if(!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Не найден встроенный стиль интерфейса."); QString style = QString::fromUtf8(file.readAll());
    if(dark) {
        const QMap<QString, QString> colors{{"#242930","#e4e7eb"},{"#f6f7f8","#202328"},{"#626b76","#aab2bd"},
            {"#ffffff","#292d33"},{"#d9dde3","#454b55"},{"#526171","#bac3cf"},{"#e5e8ec","#3b414a"},
            {"#2864a6","#6b9ed8"},{"#c7cdd5","#555e6b"},{"#edf3fa","#303f52"},{"#26704d","#79c5a0"},
            {"#9a4c13","#efb77b"},{"#edf1f5","#363d46"},{"#f0f2f4","#25292f"},{"#8a929c","#9099a5"},
            {"#d5dbe3","#505966"},{"#557ca7","#82a9d5"},{"#bcc4ce","#626d7b"}};
        for(auto i = colors.begin(); i != colors.end(); ++i) style.replace(i.key(), i.value());
    }
    setProperty("darkTheme", dark); setStyleSheet(style); refreshRows();
}
void Window::refreshRows() {
    if(!ready) return;
    for(const auto& name : rows.keys()) if(!snapshot.audio.contains(name)) { table->removeRow(rows[name].anchor->row()); rows.remove(name); }
    for(const auto& name : snapshot.audio.keys()) if(!rows.contains(name)) {
        const int row = table->rowCount(); table->insertRow(row); Row widgets{};
        for(int c = 0; c < 4; ++c) table->setItem(row, c, new QTableWidgetItem);
        widgets.anchor = table->item(row, 0);
        for(int c = 0; c < 4; ++c) { widgets.cells[c] = new QWidget; widgets.cells[c]->setObjectName("captureCell"); widgets.cells[c]->setAttribute(Qt::WA_StyledBackground); table->setCellWidget(row, c, widgets.cells[c]); }
        auto checkboxLayout = new QHBoxLayout(widgets.cells[0]); checkboxLayout->setAlignment(Qt::AlignCenter);
        widgets.check = new QCheckBox; widgets.check->setAccessibleName("Захватывать " + name); checkboxLayout->addWidget(widgets.check);
        auto nameLayout = new QHBoxLayout(widgets.cells[1]); widgets.name = new QLabel(name); widgets.name->setToolTip(name); nameLayout->addWidget(widgets.name);
        auto volumeLayout = new QHBoxLayout(widgets.cells[2]); widgets.slider = new ResetSlider;
        widgets.slider->setMinimumHeight(24);
        widgets.slider->setAccessibleName("Громкость " + name); widgets.slider->setToolTip("0–200%. Двойной клик — 100%.");
        widgets.spin = new QSpinBox; widgets.spin->setRange(0,200); widgets.spin->setSuffix("%"); widgets.spin->setFixedWidth(78);
        widgets.spin->setButtonSymbols(QAbstractSpinBox::NoButtons); widgets.spin->setKeyboardTracking(false);
        widgets.spin->setAccessibleName("Точная громкость " + name); volumeLayout->addWidget(widgets.slider, 1); volumeLayout->addWidget(widgets.spin);
        auto reasonLayout = new QHBoxLayout(widgets.cells[3]); widgets.reason = new QLabel; widgets.reason->setWordWrap(true); reasonLayout->addWidget(widgets.reason);
        connect(widgets.check, &QCheckBox::clicked, this, [this, name](bool selected) {
            auto next = settings.json; auto overrides = next["overrides"].toObject(); overrides[name] = selected; next["overrides"] = overrides;
            commit(next); refreshRows();
        });
        auto edit = [this, name](int value) {
            const auto row = rows.value(name); QSignalBlocker a(row.slider), b(row.spin);
            row.slider->setValue(value); row.spin->setValue(value); pendingVolumes[name] = value; volumeTimer.start();
        };
        connect(widgets.slider, &QSlider::valueChanged, this, edit); connect(widgets.spin, &QSpinBox::valueChanged, this, edit);
        rows[name] = widgets;
    }
    int selected = 0, blocked = 0, visible = 0; const bool dark = property("darkTheme").toBool();
    for(auto i = rows.begin(); i != rows.end(); ++i) {
        const auto name = i.key(); auto& row = i.value(); const bool active = settings.selected(name); selected += active;
        const bool denied = settings.denied(name);
        // The anchor moves with its cell widgets when the table is sorted.
        // Keep excluded applications last, then sort each group by exe name.
        row.anchor->setText((denied ? QStringLiteral("1:") : QStringLiteral("0:")) + name);
        visible += settings.showExcluded || !denied;
        const auto conflicts = snapshot.selection.blocked.value(name, snapshot.selection.blocked.value("*"));
        const bool conflict = active && !conflicts.isEmpty(); blocked += conflict;
        QString reason = conflict ? "Ограничение: " + conflicts.join(", ") : settings.denied(name) ? "Исключено" : active ? "Выбрано" : "Не выбрано";
        QSignalBlocker a(row.check), b(row.slider), c(row.spin);
        row.check->setChecked(active); row.check->setEnabled(!settings.denied(name)); row.check->setToolTip(reason);
        row.name->setText((snapshot.audio.value(name) == 1 ? QString::fromUtf8("●  ") : QString::fromUtf8("○  ")) + name);
        row.reason->setText(reason); row.reason->setToolTip(reason);
        const int value = pendingVolumes.value(name, qRound(settings.volumes.value(name,100)));
        row.slider->setValue(value); row.spin->setValue(value);
        const auto background = conflict ? (dark ? "#493b2d" : "#fff4e5") : active ? (dark ? "#303f52" : "#edf3fa") : (dark ? "#292d33" : "#ffffff");
        for(auto cell : row.cells) cell->setStyleSheet(QString("QWidget#captureCell { background: %1; }").arg(background));
    }
    table->sortItems(0, Qt::AscendingOrder);
    for(auto i = rows.cbegin(); i != rows.cend(); ++i)
        table->setRowHidden(i.value().anchor->row(), !settings.showExcluded && settings.denied(i.key()));
    table->setColumnHidden(3, !blocked); summary->setText(QString("%1 всего · %2 выбрано").arg(visible).arg(selected) + (blocked ? QString(" · %1 заблокировано").arg(blocked) : ""));
    empty->setText(rows.isEmpty() ? "Нет приложений со звуком. Запустите воспроизведение, чтобы они появились здесь."
        : "Все приложения исключены. Включите «Показывать исключённые процессы» в настройках, чтобы увидеть их.");
    empty->setVisible(visible == 0);
}
void Window::showSettings() {
    QDialog dialog(this); dialog.setWindowTitle("Настройки"); dialog.resize(530, 410); auto layout = new QVBoxLayout(&dialog);
    auto tabs = new QTabWidget; layout->addWidget(tabs); auto general = new QWidget; auto form = new QVBoxLayout(general);
    auto automatic = new QCheckBox("Автоматически подключаться к TeamSpeak"); automatic->setChecked(settings.automatic); form->addWidget(automatic);
    auto hide = new QCheckBox("Сворачивать в трей при закрытии и сворачивании"); hide->setChecked(settings.tray); form->addWidget(hide);
    auto startup = new QCheckBox("Запускать вместе с Windows"); startup->setChecked(startupEnabled()); form->addWidget(startup);
    auto showExcluded = new QCheckBox("Показывать исключённые процессы"); showExcluded->setChecked(settings.showExcluded); form->addWidget(showExcluded);
    showExcluded->setToolTip("Показывать внизу списка приложения, исключённые правилом, и TeamSpeak.");
    form->addWidget(label("Тема оформления", "muted")); auto theme = new QComboBox;
    theme->addItem("Как в системе", "system"); theme->addItem("Светлая", "light"); theme->addItem("Тёмная", "dark");
    theme->setCurrentIndex(theme->findData(settings.theme)); form->addWidget(theme); form->addStretch(); tabs->addTab(general, "Настройки");
    auto help = new QTextBrowser; help->setPlainText("1. Запустите TeamSpeak и подключите панель.\n2. Начните трансляцию со звуком. Уже идущую перезапустите.\n3. Выберите приложения и громкость.\n\nИсключения важнее ручного выбора. Громкость меняется только в трансляции. Двойной клик по ползунку возвращает 100%.\n\nПосле отключения панели управляемый поток станет тихим. Для обычного звука перезапустите трансляцию.\n\nВ режиме трея окно можно вернуть значком; выход — через меню трея.");
    tabs->addTab(help, "Как пользоваться");
    auto updates = new UpdatePanel; tabs->addTab(updates, "Обновления");
    connect(updates, &UpdatePanel::installReady, &dialog, [this, &dialog](const QString& package, const QString& digest) {
        if(!commit(settings.json)) { QMessageBox::critical(&dialog, "Обновление", "Не удалось сохранить настройки. Обновление отменено."); return; }
        updatePackage = package; updateDigest = digest; exitRequested = true;
        dialog.accept(); QTimer::singleShot(0, this, &QWidget::close);
    });
    auto license = new QTextBrowser; QFile text(":/native/vendor/minhook/LICENSE.txt"); const bool licenseAvailable = text.open(QIODevice::ReadOnly);
    license->setPlainText("Qt Widgets: динамическая линковка. Условия Qt и сторонних компонентов находятся в каталоге licenses поставки.\nhttps://www.qt.io/licensing/\n\n" + (licenseAvailable ? QString::fromUtf8(text.readAll()) : QString{})); tabs->addTab(license, "Компоненты");
    auto close = new QPushButton("Закрыть"); layout->addWidget(close,0,Qt::AlignRight); connect(close,&QPushButton::clicked,&dialog,&QDialog::accept);
    connect(automatic,&QCheckBox::toggled,&dialog,[this,automatic](bool value) {
        auto next=settings.json; next["auto_connect"]=value;
        if(!commit(next)){QSignalBlocker guard(automatic);automatic->setChecked(settings.automatic);return;}
        autoTimer.stop();updateConnection();scheduleAutomatic();
    });
    connect(hide,&QCheckBox::toggled,&dialog,[this,hide](bool value) {auto next=settings.json;next["minimize_to_tray"]=value;if(!commit(next)){QSignalBlocker guard(hide);hide->setChecked(settings.tray);}});
    connect(showExcluded,&QCheckBox::toggled,&dialog,[this,showExcluded](bool value) {
        auto next=settings.json; next["show_excluded"]=value;
        if(!commit(next)) {QSignalBlocker guard(showExcluded);showExcluded->setChecked(settings.showExcluded);}
    });
    connect(startup,&QCheckBox::toggled,&dialog,[this,startup](bool value) {try{setStartup(value);}catch(const std::exception&e){notice->setText(QString::fromUtf8(e.what()));notice->show();QSignalBlocker guard(startup);startup->setChecked(startupEnabled());}});
    connect(theme,&QComboBox::currentIndexChanged,&dialog,[this,theme](int index) {auto next=settings.json;next["theme"]=theme->itemData(index).toString();if(commit(next))applyTheme();else{QSignalBlocker guard(theme);theme->setCurrentIndex(theme->findData(settings.theme));}});
    dialog.exec();
}
void Window::updateConnection() {
    connectButton->setText(connected ? "Отключить" : "Подключить"); connectButton->setEnabled(!settings.automatic && !busy && !closing);
    status->setProperty("connected", connected); status->style()->unpolish(status); status->style()->polish(status);
}
void Window::scheduleAutomatic() { if(settings.automatic && !connected && !busy && !closing) autoTimer.start(3000); }
void Window::toggleConnection() { if(busy || closing) return; busy = true; updateConnection(); if(connected)backend->disconnectTarget();else backend->connectTarget(); }
void Window::restoreWindow() { showNormal(); raise(); activateWindow(); }
void Window::closeEvent(QCloseEvent* event) {
    if(stopped) { event->accept(); return; }
    event->ignore(); if(closing) return;
    if(!exitRequested && settings.tray && tray->isVisible()) { hide(); return; }
    if(auto spin = qobject_cast<QSpinBox*>(QApplication::focusWidget())) spin->interpretText();
    if(rulesTimer.isActive()) applyRules();
    if(!pendingVolumes.isEmpty() && !commit(settings.json)) return;
    closing = true; autoTimer.stop(); rulesTimer.stop(); volumeTimer.stop(); centralWidget()->setEnabled(false);
    status->setText("Выключаю захват…"); backend->stop();
}
void Window::changeEvent(QEvent* event) { QMainWindow::changeEvent(event); if(event->type() == QEvent::WindowStateChange && isMinimized() && settings.tray && tray && tray->isVisible()) QTimer::singleShot(0,this,&QWidget::hide); }
void Window::resizeEvent(QResizeEvent* event) { QMainWindow::resizeEvent(event); if(table) table->setColumnWidth(2, table->viewport()->width()/2); }
