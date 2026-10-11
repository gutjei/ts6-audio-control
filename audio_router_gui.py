"""Single-page Qt control panel for TeamSpeak screen-share audio."""
import math
import queue
import re
import sys
import threading

from PySide6.QtCore import Qt, QTimer, QSignalBlocker, QEvent, QSize
from PySide6.QtGui import QColor, QPainter, QPen, QIcon, QPixmap, QPainterPath, QPalette
from PySide6.QtWidgets import (
    QApplication, QAbstractItemView, QAbstractSpinBox, QCheckBox, QFrame, QGridLayout,
    QHBoxLayout, QHeaderView, QLabel, QLineEdit, QMainWindow, QMessageBox,
    QPushButton, QSlider, QSpinBox, QTableWidget, QTableWidgetItem,
    QVBoxLayout, QWidget, QSizePolicy, QToolButton, QDialog, QTabWidget, QComboBox, QListView, QTextBrowser,
    QSystemTrayIcon, QMenu,
)
from live_policy import compile_rules, decide
import windows_startup


STYLE = """
QWidget { color: #242930; font-family: 'Segoe UI'; font-size: 13px; }
QMainWindow, QWidget#page { background: #f6f7f8; }
QLabel { background: transparent; }
QLabel#title { font-size: 23px; font-weight: 600; }
QLabel#muted { color: #626b76; }
QLabel#section { font-size: 14px; font-weight: 600; }
QFrame#rules { background: #ffffff; border: 1px solid #d9dde3; border-radius: 4px; }
QFrame#connection { background: transparent; border: none; }
QToolButton { border: none; background: transparent; padding: 6px; color: #526171; }
QToolButton:hover { background: #e5e8ec; }
QToolButton[keyboardFocus="true"]:focus { border: 1px solid #2864a6; }
QToolButton::menu-indicator { image: none; }
QMenu { background: #ffffff; border: 1px solid #c7cdd5; padding: 4px; }
QMenu::item { padding: 7px 18px; }
QMenu::item:selected { background: #edf3fa; }
QLabel#status[connected="true"] { color: #26704d; }
QLabel#status { font-size: 14px; font-weight: 600; }
QLabel#notice { color: #626b76; }
QLabel#error { color: #9a4c13; }
QPushButton { background: #ffffff; border: 1px solid #c7cdd5; border-radius: 3px; padding: 6px 12px; }
QPushButton:hover { background: #edf1f5; border-color: #9aa5b3; }
QPushButton:pressed { background: #e0e6ed; }
QPushButton[keyboardFocus="true"]:focus, QLineEdit:focus, QSpinBox:focus { border: 1px solid #2864a6; }
QPushButton:disabled { color: #8a929c; background: #f0f2f4; border-color: #d9dde3; }
QPushButton#primary { background: #2864a6; color: #ffffff; border: 1px solid #2864a6; font-weight: 600; padding: 8px 20px; }
QPushButton#primary:hover { background: #21558e; }
QPushButton#primary:pressed { background: #194675; }
QPushButton#primary[keyboardFocus="true"]:focus { border: 1px solid #142e4c; }
QPushButton#primary:disabled { background: #e2e8ef; color: #737f8e; border-color: #c7cdd5; }
QLineEdit, QSpinBox { background: #ffffff; border: 1px solid #c7cdd5; border-radius: 3px; padding: 6px; selection-background-color: #2864a6; selection-color: #ffffff; }
QSpinBox { padding: 4px; }
QTableWidget { background: #ffffff; border: 1px solid #d9dde3; border-radius: 0; gridline-color: #e5e8ec; outline: none; }
QTableWidget::item { padding: 0; border-bottom: 1px solid #e5e8ec; }
QHeaderView::section { background: #f0f2f4; color: #526171; border: none; border-bottom: 1px solid #d9dde3; padding: 10px; font-weight: 600; }
QCheckBox { spacing: 7px; background: transparent; }
QSlider { background: transparent; }
QSlider::groove:horizontal { height: 4px; background: #d5dbe3; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #557ca7; border-radius: 2px; }
QSlider::handle:horizontal { background: #ffffff; border: 1px solid #557ca7; width: 14px; margin: -6px 0; border-radius: 3px; }
QSlider::handle:horizontal:hover { background: #e5edf7; }
QSlider::handle:horizontal:focus { border: 2px solid #2864a6; }
QToolTip { background: #ffffff; color: #242930; border: 1px solid #c7cdd5; padding: 6px; }
QScrollBar:vertical { background: #f0f2f4; width: 12px; }
QScrollBar::handle:vertical { background: #bcc4ce; min-height: 30px; border-radius: 3px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
"""

DARK_COLORS = {
    '#242930': '#e4e7eb', '#f6f7f8': '#202328', '#626b76': '#aab2bd',
    '#ffffff': '#292d33', '#d9dde3': '#454b55', '#526171': '#bac3cf',
    '#e5e8ec': '#3b414a', '#2864a6': '#6b9ed8', '#c7cdd5': '#555e6b',
    '#edf3fa': '#303f52', '#26704d': '#79c5a0', '#9a4c13': '#efb77b',
    '#edf1f5': '#363d46', '#9aa5b3': '#8391a2', '#e0e6ed': '#424b57',
    '#8a929c': '#9099a5', '#f0f2f4': '#25292f', '#21558e': '#487ab5',
    '#194675': '#365e8d', '#142e4c': '#a4c7ef', '#e2e8ef': '#3a424d',
    '#737f8e': '#a8b1bd', '#d5dbe3': '#505966', '#557ca7': '#82a9d5',
    '#e5edf7': '#40546c', '#bcc4ce': '#626d7b', '#fff4e5': '#493b2d',
    '#77818e': '#a5afbc', '#8c96a3': '#9ca8b7',
}
STYLE += """
QDialog { background: #f6f7f8; }
QTabWidget::pane { border: 1px solid #d9dde3; background: #ffffff; }
QTabBar::tab { background: #f0f2f4; padding: 9px 16px; }
QTabBar::tab:selected { background: #ffffff; }
QComboBox { combobox-popup: 0; background: #ffffff; border: 1px solid #c7cdd5; padding: 7px; }
QTextBrowser { background: transparent; border: none; }
QComboBox QAbstractItemView::item { padding: 7px 10px; }
QComboBox QAbstractItemView { border: 1px solid #c7cdd5; outline: none; padding: 0; background: #ffffff; color: #242930; selection-background-color: #edf3fa; }
"""


def theme_color(widget, color):
    return DARK_COLORS.get(color, color) if widget.window().property('darkTheme') else color



def label(text='', name=None):
    widget = QLabel(text)
    widget.setTextFormat(Qt.TextFormat.PlainText)
    widget.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Fixed)
    if name:
        widget.setObjectName(name)
    return widget


class SessionIcon(QWidget):
    """Shape + color, with a tooltip and an accessible description; no fake meter."""
    def __init__(self, name):
        super().__init__()
        self.name = name
        self.state = None
        self.setFixedSize(24, 24)
        self.setFocusPolicy(Qt.FocusPolicy.TabFocus)
        self.set_state(None)

    def set_state(self, state):
        self.state = state
        text = 'Аудиосессия активна' if state == 1 else 'Аудиосессия неактивна' if state is not None else 'Нет аудиосессии'
        self.setToolTip(text + '. Это состояние сессии Windows, а не индикатор громкости.')
        self.setAccessibleName(f'{self.name}: {text}')
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(QPen(QColor(theme_color(self, '#26704d' if self.state == 1 else '#77818e')), 2))
        if self.state == 1:
            for x, height in ((5, 6), (11, 16), (17, 10)):
                painter.drawLine(x, 12 - height // 2, x, 12 + height // 2)
        elif self.state is not None:
            painter.drawLine(9, 6, 9, 18)
            painter.drawLine(15, 6, 15, 18)
        else:
            painter.drawEllipse(5, 5, 14, 14)
            painter.drawLine(7, 17, 17, 7)
        if self.hasFocus():
            painter.setPen(QColor(theme_color(self, '#2864a6')))
            painter.drawRoundedRect(1, 1, 22, 22, 3, 3)


class CaptureCheck(QCheckBox):
    """Keep native checkbox keyboard/accessibility semantics with a legible mark."""
    def __init__(self, text=''):
        super().__init__(text)
        if text:
            self.setFixedHeight(28)
        else:
            self.setFixedSize(28, 28)

    def sizeHint(self):
        return QSize(40 + self.fontMetrics().horizontalAdvance(self.text()), 28) if self.text() else QSize(28, 28)

    def hitButton(self, point):
        return self.rect().contains(point)

    def paintEvent(self, event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        color = '#2864a6' if self.isChecked() else '#8c96a3'
        painter.setPen(QPen(QColor(theme_color(self, color if self.isEnabled() else '#c7cdd5')), 1.5))
        painter.setBrush(QColor(theme_color(self, '#2864a6' if self.isChecked() else '#ffffff')))
        painter.drawRoundedRect(4, 4, 20, 20, 2, 2)
        if self.isChecked():
            painter.setPen(QPen(QColor('#ffffff'), 2.5))
            painter.drawLine(8, 14, 12, 18)
            painter.drawLine(12, 18, 20, 10)
        if self.hasFocus():
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.setPen(QPen(QColor(theme_color(self, '#2864a6')), 1))
            painter.drawRoundedRect(1, 1, 26, 26, 5, 5)
        if self.text():
            painter.setPen(QColor(theme_color(self, '#242930' if self.isEnabled() else '#8a929c')))
            painter.drawText(self.rect().adjusted(36, 0, 0, 0),
                             Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, self.text())


class VolumeSlider(QSlider):
    def mouseDoubleClickEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self.setRepeatAction(QSlider.SliderAction.SliderNoAction)
            self.setSliderDown(False)
            self.setValue(100)
            event.accept()
        else:
            super().mouseDoubleClickEvent(event)


class VolumeControl(QWidget):
    def __init__(self, name, changed):
        super().__init__()
        layout = QHBoxLayout(self)
        layout.setContentsMargins(10, 8, 12, 8)
        layout.setSpacing(10)
        self.slider = VolumeSlider(Qt.Orientation.Horizontal)
        self.slider.setRange(0, 200)
        self.slider.setMinimumWidth(95)
        self.slider.setMinimumHeight(34)
        self.slider.setAccessibleName(f'Громкость в трансляции: {name}')
        self.slider.setToolTip('0–200%. Двойной клик по шкале — сброс до 100%. Выше 100% — усиление; пики могут ограничиваться.')
        self.spin = QSpinBox()
        self.spin.setRange(0, 200)
        self.spin.setSuffix('%')
        self.spin.setButtonSymbols(QAbstractSpinBox.ButtonSymbols.NoButtons)
        self.spin.setFixedWidth(78)
        self.spin.setFixedHeight(32)
        self.spin.setKeyboardTracking(False)
        self.spin.setAccessibleName(f'Точная громкость: {name}')
        layout.addWidget(self.slider, 1)
        layout.addWidget(self.spin)
        self.slider.valueChanged.connect(lambda value: self.edit(value, changed))
        self.spin.valueChanged.connect(lambda value: self.edit(value, changed))

    def set_value(self, value):
        with QSignalBlocker(self.slider), QSignalBlocker(self.spin):
            self.slider.setValue(round(value))
            self.spin.setValue(round(value))

    def edit(self, value, changed):
        self.set_value(value)
        changed(value)


class App(QMainWindow):
    def __init__(self, backend_factory=None):
        super().__init__()
        self.setWindowTitle('TeamSpeak · Звук трансляции')
        self.resize(940, 620)
        self.setMinimumSize(780, 520)
        self.setStyleSheet(STYLE)
        self.queue = queue.Queue()
        self.snapshot = None
        self.rows = {}
        self.connected = self.closing = self.closed = False
        self.exit_requested = False
        self.connection_busy = False
        self.disconnect_failed = False
        self.auto_failures = 0
        self.auto_attempt = False
        self.cancel_auto_attempt = False
        self.pending_volumes = {}
        if backend_factory is None:
            from router_backend import Backend
            backend_factory = Backend
        self.backend = backend_factory(lambda kind, value: self.queue.put((kind, value)))
        self.settings = self.backend.get_settings()
        self.rule_timer = QTimer(self)
        self.rule_timer.setSingleShot(True)
        self.rule_timer.setInterval(350)
        self.rule_timer.timeout.connect(self.apply_rules)
        self.volume_timer = QTimer(self)
        self.volume_timer.setSingleShot(True)
        self.volume_timer.setInterval(120)
        self.volume_timer.timeout.connect(self.save_volume)
        self.auto_timer = QTimer(self)
        self.auto_timer.setSingleShot(True)
        self.auto_timer.timeout.connect(self.try_auto_connect)
        QApplication.instance().installEventFilter(self)
        self.build_ui()
        self.build_tray()
        self.apply_theme()
        QApplication.instance().styleHints().colorSchemeChanged.connect(self.apply_theme)
        self.event_timer = QTimer(self)
        self.event_timer.setInterval(100)
        self.event_timer.timeout.connect(self.events)
        self.event_timer.start()
        self.update_auto_ui()
        self.schedule_auto_connect(0)

    def eventFilter(self, watched, event):
        if (event.type() == QEvent.Type.Resize and hasattr(self, 'table')
                and watched is self.table.viewport()):
            self.table.setColumnWidth(2, event.size().width() // 2)
        if isinstance(watched, (QPushButton, QToolButton)) and self.isAncestorOf(watched):
            if event.type() in (QEvent.Type.FocusIn, QEvent.Type.MouseButtonPress):
                keyboard = (event.type() == QEvent.Type.FocusIn and
                            event.reason() in (Qt.FocusReason.TabFocusReason, Qt.FocusReason.BacktabFocusReason))
                watched.setProperty('keyboardFocus', keyboard)
                watched.style().unpolish(watched)
                watched.style().polish(watched)
                watched.update()
        return super().eventFilter(watched, event)

    def build_ui(self):
        page = QWidget()
        page.setObjectName('page')
        self.setCentralWidget(page)
        layout = QVBoxLayout(page)
        layout.setContentsMargins(20, 16, 20, 16)
        layout.setSpacing(8)
        connection = QFrame()
        self.connection_card = connection
        connection.setObjectName('connection')
        connection_layout = QHBoxLayout(connection)
        connection_layout.setContentsMargins(0, 0, 0, 0)
        connection_layout.addWidget(label('Звук трансляции', 'title'))
        connection_layout.addStretch()
        self.status = label('Не подключено', 'status')
        connection_layout.addWidget(self.status)
        self.connect_button = QPushButton('Подключить')
        self.connect_button.setObjectName('primary')
        self.connect_button.setFocusPolicy(Qt.FocusPolicy.TabFocus)
        self.connect_button.clicked.connect(self.toggle_connection)
        self.connect_button.setToolTip('Подключиться к TeamSpeak. При отключении звук управляемой трансляции остановится.')
        connection_layout.addWidget(self.connect_button)
        self.more_button = QToolButton()
        self.more_button.setFixedSize(40, 40)
        # Mouse clicks should not leave a keyboard-focus frame behind.
        self.more_button.setFocusPolicy(Qt.FocusPolicy.TabFocus)
        self.more_button.setAccessibleName('Настройки')
        self.more_button.setToolTip('Настройки')
        self.more_button.clicked.connect(self.open_settings)
        connection_layout.addWidget(self.more_button)
        layout.addWidget(connection)
        self.stream_status = label('', 'muted')
        self.stream_status.setWordWrap(True)
        self.stream_status.hide()
        layout.addWidget(self.stream_status)
        toolbar = QHBoxLayout()
        toolbar.addWidget(label('Приложения', 'section'))
        toolbar.addStretch()
        self.summary = label('Загрузка…', 'muted')
        toolbar.addWidget(self.summary)
        layout.addLayout(toolbar)
        self.table = QTableWidget(0, 4)
        self.table.setHorizontalHeaderLabels(['Захват', 'Приложение', 'Громкость', 'Выбор / ограничение'])
        self.table.setColumnHidden(3, True)
        self.table.verticalHeader().hide()
        self.table.verticalHeader().setDefaultSectionSize(52)
        self.table.setShowGrid(False)
        self.table.setSelectionMode(QAbstractItemView.SelectionMode.NoSelection)
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.table.setMinimumHeight(175)
        header = self.table.horizontalHeader()
        header.setDefaultAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Fixed)
        self.table.setColumnWidth(0, 76)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        header.setSectionResizeMode(2, QHeaderView.ResizeMode.Fixed)
        self.table.setColumnWidth(2, self.table.viewport().width() // 2)
        header.setSectionResizeMode(3, QHeaderView.ResizeMode.Stretch)
        header.setMinimumSectionSize(50)
        self.table.setColumnWidth(0, 76)
        layout.addWidget(self.table, 1)
        self.empty = label('Загрузка аудиосессий…', 'muted')
        self.empty.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(self.empty)
        self.rules_toggle = QToolButton()
        self.rules_toggle.setText('Автоматические правила')
        self.rules_toggle.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextBesideIcon)
        self.rules_toggle.setArrowType(Qt.ArrowType.RightArrow)
        self.rules_toggle.setCheckable(True)
        self.rules_toggle.toggled.connect(self.show_rules)
        layout.addWidget(self.rules_toggle, alignment=Qt.AlignmentFlag.AlignLeft)
        rules = QFrame()
        self.rules_panel = rules
        rules.hide()
        rules.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Fixed)
        rules.setObjectName('rules')
        grid = QGridLayout(rules)
        grid.setContentsMargins(16, 12, 16, 12)
        grid.setHorizontalSpacing(16)

        reset = QPushButton('Вернуть выбор к правилам')
        reset.setToolTip('Сбросить ручной выбор всех приложений. Громкость сохранится.')
        reset.clicked.connect(self.reset_manual)
        grid.addWidget(reset, 3, 1, alignment=Qt.AlignmentFlag.AlignRight)
        self.include = QLineEdit(self.settings['include'])
        self.exclude = QLineEdit(self.settings['exclude'])
        for col, title, field in ((0, 'Автоматически включать', self.include), (1, 'Всегда исключать', self.exclude)):
            caption = label(title, 'muted')
            caption.setBuddy(field)
            field.setAccessibleName(title)
            field.setPlaceholderText('Регулярное выражение по имени exe')
            field.setToolTip(r'Например: ^firefox\.exe$ · без учёта регистра. Пустое поле не совпадает ни с одним приложением.')
            field.textChanged.connect(lambda _text: self.rule_timer.start())
            grid.addWidget(caption, 1, col)
            grid.addWidget(field, 2, col)
            grid.setColumnStretch(col, 1)
        hint = label('Исключения важнее ручного выбора. TeamSpeak исключён всегда.', 'muted')
        hint.setWordWrap(True)
        grid.addWidget(hint, 3, 0)
        self.rule_error = label('', 'error')
        self.rule_error.setWordWrap(True)
        self.rule_error.hide()
        grid.addWidget(self.rule_error, 4, 0, 1, 2)
        layout.addWidget(rules)
        self.notice = label('', 'error')
        self.notice.hide()
        self.notice.setWordWrap(True)
        layout.addWidget(self.notice)
        self.stats = label('Нет подключённого потока', 'muted')
        self.stats.setWordWrap(True)
        self.stats.hide()
        self.rules_toggle.setChecked(True)

    def build_tray(self):
        pixmap = QPixmap(32, 32)
        pixmap.fill(Qt.GlobalColor.transparent)
        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor('#2864a6'))
        painter.drawRoundedRect(0, 0, 32, 32, 7, 7)
        painter.setPen(QPen(QColor('#ffffff'), 3, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        for x, height in ((9, 8), (16, 18), (23, 12)):
            painter.drawLine(x, 16 - height // 2, x, 16 + height // 2)
        painter.end()
        self.setWindowIcon(QIcon(pixmap))
        self.tray = QSystemTrayIcon(self.windowIcon(), self)
        self.tray.setToolTip('TeamSpeak · Звук трансляции')
        self.tray_menu = QMenu(self)
        self.tray_menu.addAction('Открыть', self.restore_from_tray)
        self.tray_menu.addSeparator()
        self.tray_menu.addAction('Выйти', self.quit_app)
        self.tray.setContextMenu(self.tray_menu)
        self.tray.activated.connect(self.tray_activated)
        self.update_tray()

    def update_tray(self):
        self.tray.setVisible(bool(self.settings.get('minimize_to_tray', False))
                             and QSystemTrayIcon.isSystemTrayAvailable())

    def hide_to_tray(self):
        if (self.closing or not self.settings.get('minimize_to_tray', False)
                or not QSystemTrayIcon.isSystemTrayAvailable()):
            return False
        self.tray.show()
        self.hide()
        return True

    def restore_from_tray(self):
        if not self.closing:
            self.showNormal()
            self.raise_()
            self.activateWindow()

    def tray_activated(self, reason):
        if reason in (QSystemTrayIcon.ActivationReason.Trigger, QSystemTrayIcon.ActivationReason.DoubleClick):
            self.restore_from_tray()

    def quit_app(self):
        self.exit_requested = True
        self.close()

    def changeEvent(self, event):
        super().changeEvent(event)
        if event.type() == QEvent.Type.WindowStateChange and self.isMinimized():
            # Let Qt finish showMinimized() before hiding the window.
            QTimer.singleShot(0, self.minimize_to_tray)

    def minimize_to_tray(self):
        if self.isMinimized():
            self.hide_to_tray()

    def change_startup(self, enabled):
        try:
            windows_startup.set_enabled(enabled)
        except (OSError, ValueError) as exc:
            with QSignalBlocker(self.startup_check):
                self.startup_check.setChecked(not enabled)
            QMessageBox.warning(self.settings_dialog, 'Автозапуск', f'Не удалось изменить автозапуск: {exc}')

    def change_tray(self, enabled):
        if self.commit({**self.settings, 'minimize_to_tray': enabled}, ''):
            self.update_tray()
        else:
            with QSignalBlocker(self.tray_check):
                self.tray_check.setChecked(bool(self.settings.get('minimize_to_tray', False)))
            QMessageBox.warning(self.settings_dialog, 'Настройки', self.notice.text())

    def apply_theme(self, *_):
        mode = self.settings.get('theme', 'system')
        dark = mode == 'dark' or (mode == 'system' and
            QApplication.instance().styleHints().colorScheme() == Qt.ColorScheme.Dark)
        self.setProperty('darkTheme', dark)
        style = re.sub(r'#[0-9a-f]{6}', lambda m: DARK_COLORS.get(m[0], m[0]), STYLE) if dark else STYLE
        # White text must remain legible on the accent button in both themes.
        style += 'QPushButton#primary { color: #ffffff; background: #2864a6; }'
        self.setStyleSheet(style)
        if getattr(self, 'settings_dialog', None):
            self.settings_dialog.setProperty('darkTheme', dark)
            self.settings_dialog.setStyleSheet(style)
        if getattr(self, 'theme_choice', None):
            view = self.theme_choice.view()
            palette = view.palette()
            for role, color in ((QPalette.ColorRole.Window, '#ffffff'),
                                (QPalette.ColorRole.Base, '#ffffff'),
                                (QPalette.ColorRole.Text, '#242930'),
                                (QPalette.ColorRole.Highlight, '#edf3fa'),
                                (QPalette.ColorRole.HighlightedText, '#242930')):
                palette.setColor(role, QColor(theme_color(self, color)))
            view.setPalette(palette)
            view.window().setPalette(palette)
            view.window().setAutoFillBackground(True)
        pixmap = QPixmap(56, 56)
        pixmap.setDevicePixelRatio(2)
        pixmap.fill(Qt.GlobalColor.transparent)
        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(QPen(QColor(theme_color(self, '#526171')), 1.8,
                            Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap,
                            Qt.PenJoinStyle.RoundJoin))
        painter.translate(14, 14)
        outline = QPainterPath()
        # One continuous six-tooth contour, without overlapping rings and boxes.
        for tooth in range(6):
            for offset, radius in ((-30, 8), (-16, 8), (-12, 11),
                                   (12, 11), (16, 8)):
                angle = math.radians(tooth * 60 + offset)
                x, y = radius * math.cos(angle), radius * math.sin(angle)
                if tooth == 0 and offset == -30:
                    outline.moveTo(x, y)
                else:
                    outline.lineTo(x, y)
        outline.closeSubpath()
        painter.drawPath(outline)
        painter.drawEllipse(-3.5, -3.5, 7, 7)
        painter.end()
        self.more_button.setIcon(QIcon(pixmap))
        self.more_button.setIconSize(pixmap.deviceIndependentSize().toSize())
        self.refresh()
        self.update()

    def change_theme(self, index):
        mode = self.theme_choice.itemData(index)
        if self.commit({**self.settings, 'theme': mode}, ''):
            self.apply_theme()
        else:
            with QSignalBlocker(self.theme_choice):
                self.theme_choice.setCurrentIndex(self.theme_choice.findData(self.settings.get('theme', 'system')))
            QMessageBox.warning(self.settings_dialog, 'Настройки', self.notice.text())

    def open_settings(self):
        if getattr(self, 'settings_dialog', None):
            self.settings_dialog.deleteLater()
        dialog = QDialog(self)
        self.settings_dialog = dialog
        dialog.setWindowTitle('Настройки')
        dialog.resize(520, 390)
        dialog.setMinimumSize(420, 300)
        dialog.setStyleSheet(self.styleSheet())
        layout = QVBoxLayout(dialog)
        tabs = QTabWidget()
        layout.addWidget(tabs)
        general = QWidget()
        options = QVBoxLayout(general)
        options.setContentsMargins(16, 16, 16, 16)
        options.addWidget(label('Тема интерфейса', 'section'))
        self.theme_choice = QComboBox()
        self.theme_choice.setView(QListView())
        self.theme_choice.setAccessibleName('Тема интерфейса')
        for title, value in (('Как в системе', 'system'), ('Классическая', 'classic'), ('Тёмная', 'dark')):
            self.theme_choice.addItem(title, value)
        self.theme_choice.setCurrentIndex(max(0, self.theme_choice.findData(self.settings.get('theme', 'system'))))
        self.theme_choice.currentIndexChanged.connect(self.change_theme)
        options.addWidget(self.theme_choice)
        self.startup_check = CaptureCheck('Запускать при старте системы')
        self.startup_check.setToolTip('Запускать панель при входе текущего пользователя в Windows.')
        try:
            self.startup_check.setChecked(windows_startup.is_enabled())
        except OSError as exc:
            self.startup_check.setEnabled(False)
            self.startup_check.setToolTip(f'Не удалось прочитать автозапуск: {exc}')
        self.startup_check.toggled.connect(self.change_startup)
        options.addWidget(self.startup_check)
        self.tray_check = CaptureCheck('Сворачивать в трей')
        self.tray_check.setChecked(bool(self.settings.get('minimize_to_tray', False)))
        self.tray_check.setToolTip('Сворачивание и крестик скрывают окно. Захват продолжается. Для завершения выберите «Выйти» в меню значка.')
        self.tray_check.toggled.connect(self.change_tray)
        options.addWidget(self.tray_check)
        self.auto_check = CaptureCheck('Автоматически подключаться к TeamSpeak')
        self.auto_check.setChecked(bool(self.settings.get('auto_connect', False)))
        self.auto_check.setToolTip('Ждать запуска TeamSpeak и подключаться после его перезапуска. Для ручного управления выключите автоподключение.')
        self.auto_check.toggled.connect(self.change_auto_connect)
        options.addWidget(self.auto_check)
        refresh = QPushButton('Обновить список приложений')
        refresh.clicked.connect(self.backend.refresh_sources)
        options.addWidget(refresh)
        state = QPushButton('Состояние потока')
        state.clicked.connect(lambda: QMessageBox.information(dialog, 'Состояние потока', self.stats.text()))
        options.addWidget(state)
        options.addStretch()
        tabs.addTab(general, 'Настройки')
        help_page = QWidget()
        help_layout = QVBoxLayout(help_page)
        help_layout.setContentsMargins(16, 16, 16, 16)
        help_text = QTextBrowser()
        self.help_text = help_text
        help_text.setPlainText('1. Запустите TeamSpeak и нажмите «Подключить» или включите автоподключение в настройках.\n'
            '2. Начните новую трансляцию с захватом звука.\n'
            '3. Выберите приложения и настройте громкость.\n\n'
            'Список обновляется автоматически. Громкость меняется только в трансляции. '
            '100% — исходный уровень.\n\n'
            'При отключении или выходе из панели звук управляемой трансляции остановится. '
            'Если включено «Сворачивать в трей», сворачивание и крестик только скрывают окно; '
            'захват продолжается. Нажмите значок в трее, чтобы открыть окно, или выберите «Выйти» в его меню. '
            'Чтобы вернуть обычный захват, перезапустите трансляцию.')
        help_layout.addWidget(help_text, 1)
        tabs.addTab(help_page, 'Как пользоваться')
        close = QPushButton('Закрыть')
        close.clicked.connect(dialog.accept)
        layout.addWidget(close, alignment=Qt.AlignmentFlag.AlignRight)
        dialog.setModal(True)
        self.apply_theme()
        dialog.show()

    def show_rules(self, expanded):
        self.rules_panel.setVisible(expanded)
        self.rules_toggle.setArrowType(Qt.ArrowType.DownArrow if expanded else Qt.ArrowType.RightArrow)


    def commit(self, candidate, message):
        candidate = {**candidate, 'volumes': {**candidate['volumes'], **self.pending_volumes}}
        try:
            self.backend.set_settings(candidate)
        except (ValueError, OSError) as exc:
            self.notice.setText(f'Настройки не сохранены: {exc}')
            self.notice.show()
            return False
        self.settings = candidate
        self.pending_volumes.clear()
        self.volume_timer.stop()
        self.notice.clear()
        self.notice.hide()
        self.refresh()
        return True

    def apply_rules(self):
        self.rule_timer.stop()
        candidate = {**self.settings, 'include': self.include.text(), 'exclude': self.exclude.text()}
        try:
            compile_rules(candidate['include'], candidate['exclude'])
        except re.error as exc:
            self.rule_error.setText(f'Ошибка выражения: {exc}. Действуют последние сохранённые правила.')
            self.rules_toggle.setChecked(True)
            self.rule_error.show()
            return
        self.rule_error.hide()
        self.commit(candidate, 'Правила сохранены')

    def toggle(self, name):
        active, reason = decide(name, self.settings['overrides'], compile_rules(self.settings['include'], self.settings['exclude']))
        if reason in ('exclude', 'TeamSpeak исключён'):
            return
        self.commit({**self.settings, 'overrides': {**self.settings['overrides'], name: not active}}, 'Выбор сохранён')
        self.refresh()

    def reset_manual(self):
        self.commit({**self.settings, 'overrides': {}}, 'Ручной выбор сброшен. Громкость сохранена.')

    def change_volume(self, name, value):
        self.pending_volumes[name] = value
        self.volume_timer.start()

    def save_volume(self):
        self.volume_timer.stop()
        self.commit(self.settings, 'Громкость сохранена')

    def add_row(self, name):
        row = self.table.rowCount()
        self.table.insertRow(row)
        # Cell widgets are transparent; paint the selection across every column.
        for column in range(3):
            self.table.setItem(row, column, QTableWidgetItem())
        check = CaptureCheck()
        check.setAccessibleName(f'Захватывать {name}')
        check.clicked.connect(lambda: self.toggle(name))
        box = QWidget()
        box_layout = QHBoxLayout(box)
        box_layout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        box_layout.addWidget(check)
        self.table.setCellWidget(row, 0, box)
        app = QWidget()
        app_layout = QHBoxLayout(app)
        app_layout.setContentsMargins(8, 0, 8, 0)
        icon = SessionIcon(name)
        app_layout.addWidget(icon)
        title = label(name)
        title.setToolTip(name)
        title.setMinimumWidth(0)
        app_layout.addWidget(title, 1)
        self.table.setCellWidget(row, 1, app)
        volume = VolumeControl(name, lambda value: self.change_volume(name, value))
        self.table.setCellWidget(row, 2, volume)
        reason = QTableWidgetItem()
        self.table.setItem(row, 3, reason)
        reason_label = label('', 'muted')
        reason_label.setContentsMargins(12, 0, 8, 0)
        self.table.setCellWidget(row, 3, reason_label)
        cells = (box, app, volume, reason_label)
        for cell in cells:
            cell.setObjectName('captureCell')
            cell.setAttribute(Qt.WidgetAttribute.WA_StyledBackground)
        self.rows[name] = {'check': check, 'icon': icon, 'volume': volume,
                           'reason': reason, 'reason_label': reason_label, 'cells': cells}

    def refresh(self):
        if self.snapshot is None:
            return
        data = self.snapshot
        rules = compile_rules(self.settings['include'], self.settings['exclude'])
        # The inventory includes sessions belonging to existing processes only.
        # Saved preferences must not keep closed applications in the table.
        names = set(data['audio'])
        for name in set(self.rows) - names:
            self.table.removeRow(self.rows.pop(name)['reason'].row())
        for name in sorted(names - set(self.rows)):
            self.add_row(name)
        chosen = blocked_count = 0
        for name, widgets in self.rows.items():
            active, reason = decide(name, self.settings['overrides'], rules)
            chosen += active
            denied = reason in ('exclude', 'TeamSpeak исключён')
            conflicts = data['blocked'].get(name) or data['blocked'].get('*')
            blocked = active and bool(conflicts)
            blocked_count += blocked
            background = QColor(theme_color(self, '#fff4e5' if blocked else '#edf3fa' if active else '#ffffff'))
            row = widgets['reason'].row()
            for column in range(self.table.columnCount()):
                self.table.item(row, column).setBackground(background)
            for cell in widgets['cells']:
                if cell.property('rowColor') != background.name():
                    cell.setProperty('rowColor', background.name())
                    foreground = theme_color(self, '#9a4c13' if blocked else '#626b76' if cell is widgets['reason_label'] else '#242930')
                    border = theme_color(self, '#e5e8ec')
                    cell.setStyleSheet(f'QWidget#captureCell {{ background: {background.name()}; border-bottom: 1px solid {border}; color: {foreground}; }}')
            with QSignalBlocker(widgets['check']):
                widgets['check'].setChecked(active)
            widgets['check'].setEnabled(not denied)
            reason = {'include': 'По правилу', 'exclude': 'Исключено правилом', 'вручную': 'Ручной выбор', 'не выбран': 'Не выбрано'}.get(reason, reason)
            if blocked:
                reason = 'Захват заблокирован'
            detail = ('Ограничение: ' + ', '.join(conflicts)) if blocked else reason
            widgets['check'].setToolTip(detail + '. Пробел переключает выбор.' if not denied else detail)
            widgets['reason'].setText(reason)
            widgets['reason'].setToolTip(detail)
            widgets['reason'].setForeground(QColor(theme_color(self, '#9a4c13' if blocked else '#626b76')))
            widgets['reason'].setData(Qt.ItemDataRole.AccessibleTextRole, detail)
            widgets['reason_label'].setText(reason)
            widgets['reason_label'].setToolTip(detail)
            widgets['reason_label'].setAccessibleName(detail)
            widgets['icon'].set_state(data['audio'].get(name))
            widgets['volume'].set_value(self.pending_volumes.get(name, self.settings['volumes'].get(name, 100)))
        self.table.setColumnHidden(3, not blocked_count)
        self.summary.setText(f'{len(names)} всего · {chosen} выбрано' + (f' · {blocked_count} заблокировано' if blocked_count else ''))
        self.empty.setText('Нет приложений со звуком. Запустите воспроизведение, чтобы они появились здесь.')
        self.empty.setVisible(not names)

    def set_connection_style(self, connected):
        for widget in (self.connection_card, self.status):
            widget.setProperty('connected', connected)
            widget.style().unpolish(widget)
            widget.style().polish(widget)
            widget.update()

    def auto_enabled(self):
        return bool(self.settings.get('auto_connect', False))

    def update_auto_ui(self):
        self.connect_button.setEnabled(not self.auto_enabled() and not self.connection_busy and not self.closing)
        self.connect_button.setToolTip('Для ручного управления выключите автоподключение в настройках.'
                                       if self.auto_enabled() else 'Подключиться к TeamSpeak. При отключении звук управляемой трансляции остановится.')

    def schedule_auto_connect(self, delay=3000):
        if (self.auto_enabled() and not self.connected and not self.connection_busy
                and not self.disconnect_failed and not self.closing):
            self.auto_timer.start(delay)

    def try_auto_connect(self):
        if (self.auto_enabled() and not self.connected and not self.connection_busy
                and not self.disconnect_failed and not self.closing):
            self.connect(automatic=True)

    def change_auto_connect(self, enabled):
        if not self.commit({**self.settings, 'auto_connect': enabled}, ''):
            with QSignalBlocker(self.auto_check):
                self.auto_check.setChecked(bool(self.settings.get('auto_connect', False)))
            QMessageBox.warning(self.settings_dialog, 'Настройки', self.notice.text())
            return
        self.auto_timer.stop()
        self.auto_failures = 0
        if self.auto_attempt and self.connection_busy:
            self.cancel_auto_attempt = not enabled
        if not enabled and not self.connected and not self.connection_busy and not self.disconnect_failed:
            self.status.setText('Не подключено')
            self.stream_status.setText('Нажмите «Подключить», когда TeamSpeak запущен.')
        self.update_auto_ui()
        self.schedule_auto_connect(0)

    def toggle_connection(self):
        if self.connection_busy or self.closing or self.auto_enabled():
            return
        if self.connected or self.disconnect_failed:
            self.disconnect()
        else:
            self.connect()

    def connect(self, automatic=False):
        if self.connection_busy or self.closing or self.connected:
            return
        self.auto_timer.stop()
        self.auto_attempt = automatic
        self.cancel_auto_attempt = False
        self.connection_busy = True
        self.set_connection_style(False)
        self.connect_button.setEnabled(False)
        self.update_auto_ui()
        if not automatic:
            self.stream_status.hide()
            self.status.setText('Подключение…')
            self.stream_status.setText('Подключаем управление звуком к TeamSpeak.')
        def work():
            try:
                if automatic:
                    self.backend.connect(wait_for_process=True)
                else:
                    self.backend.connect()
            except Exception as exc:
                self.queue.put(('connect-error', str(exc)))
        threading.Thread(target=work, daemon=True).start()

    def disconnect(self):
        if self.connection_busy or self.closing:
            return
        self.connection_busy = True
        self.auto_timer.stop()
        self.update_auto_ui()
        self.set_connection_style(False)
        self.connect_button.setEnabled(False)
        self.stream_status.hide()
        self.status.setText('Отключение…')
        self.stream_status.setText('Останавливаем захват и освобождаем подключение.')
        def work():
            try:
                self.backend.disconnect()
            except Exception as exc:
                self.queue.put(('disconnect-error', str(exc)))
        threading.Thread(target=work, daemon=True).start()

    def events(self):
        while True:
            try:
                kind, value = self.queue.get_nowait()
            except queue.Empty:
                return
            if kind == 'closed':
                self.closed = True
                self.event_timer.stop()
                if value:
                    QMessageBox.warning(self, 'Завершение работы', value)
                self.close()
                QApplication.instance().quit()
                return
            if self.closing:
                continue
            if kind == 'snapshot':
                self.snapshot = value
                self.refresh()
            elif kind == 'connecting':
                self.status.setText('Подключение…')
                self.stream_status.setText('Подключаем управление звуком к TeamSpeak.')
                self.stream_status.hide()
            elif kind == 'connect-waiting':
                self.connection_busy = False
                self.auto_attempt = False
                self.auto_failures = 0
                self.status.setText('Ожидание TeamSpeak' if self.auto_enabled() else 'Не подключено')
                self.stream_status.setText('Запустите TeamSpeak — панель подключится автоматически.' if self.auto_enabled()
                                           else 'Нажмите «Подключить», когда TeamSpeak запущен.')
                self.stream_status.hide()
                self.connect_button.setText('Подключить')
                self.connect_button.setEnabled(True)
                self.update_auto_ui()
                self.schedule_auto_connect()
            elif kind == 'stats' and self.connected and not self.connection_busy:
                contexts, sources, packets, audible, errors, code = value[:6]
                self.stream_status.setText('Начните заново трансляцию с включённым звуком.' if not contexts else 'Поток подключён')
                self.stream_status.setVisible(bool(errors))
                self.stats.setText(f'Потоков: {contexts} · Источников: {sources} · Пакетов со звуком: {audible}' + (f' · Ошибка 0x{code:08x} ({errors})' if errors else ''))
                if errors:
                    self.stream_status.setText('Ошибка аудиопотока. Подробности указаны внизу окна.')
            elif kind == 'connected':
                self.stream_status.hide()
                self.connected = True
                self.disconnect_failed = False
                self.connection_busy = False
                self.set_connection_style(True)
                self.status.setText(f'TeamSpeak подключён ({value})')
                self.stream_status.setText('Начните заново трансляцию с включённым звуком.')
                self.connect_button.setText('Отключить')
                self.connect_button.setEnabled(True)
                self.auto_timer.stop()
                self.auto_failures = 0
                self.auto_attempt = False
                self.update_auto_ui()
                if self.cancel_auto_attempt:
                    self.cancel_auto_attempt = False
                    self.disconnect()
            elif kind == 'connect-error':
                self.stream_status.show()
                self.connected = False
                self.disconnect_failed = False
                self.connection_busy = False
                self.set_connection_style(False)
                self.status.setText('Не удалось подключиться')
                self.stream_status.setText(value)
                self.connect_button.setText('Повторить')
                self.connect_button.setEnabled(True)
                self.auto_attempt = False
                self.update_auto_ui()
                if self.auto_enabled():
                    self.auto_failures += 1
                    delay = min(30, 5 * 2 ** min(self.auto_failures - 1, 3))
                    self.stream_status.setText(f'{value}\nПовторная попытка через {delay} с.')
                    self.schedule_auto_connect(delay * 1000)
            elif kind in ('detached', 'disconnected'):
                self.stream_status.hide()
                self.connected = False
                self.disconnect_failed = False
                self.connection_busy = False
                self.set_connection_style(False)
                if kind == 'disconnected':
                    self.status.setText('Отключено от TeamSpeak')
                    self.stream_status.setText('Для возврата обычного звука перезапустите трансляцию в TeamSpeak.')
                else:
                    self.status.setText('TeamSpeak закрыт' if value == 'process-terminated' else 'Соединение потеряно')
                    self.stream_status.setText('Запустите TeamSpeak и нажмите «Подключить». Перезапуск панели не нужен.')
                self.stats.setText('Нет подключённого потока')
                self.connect_button.setText('Подключить')
                self.connect_button.setEnabled(True)
                self.update_auto_ui()
                if self.auto_enabled():
                    self.status.setText('Ожидание TeamSpeak')
                    self.stream_status.setText('Подключимся автоматически, когда TeamSpeak будет доступен. После подключения начните трансляцию заново.')
                    self.schedule_auto_connect()
            elif kind == 'disconnect-error':
                self.stream_status.show()
                self.connected = False
                self.disconnect_failed = True
                self.connection_busy = False
                self.set_connection_style(False)
                self.status.setText('Не удалось завершить отключение')
                self.stream_status.setText(value)
                self.connect_button.setText('Повторить отключение')
                self.connect_button.setEnabled(True)
                self.auto_timer.stop()
                self.update_auto_ui()
            elif kind == 'error':
                self.notice.setText(value)
                self.notice.show()
            elif kind == 'native' and value.get('event') == 'error':
                self.notice.setText(value.get('message', str(value)))
                self.notice.show()

    def closeEvent(self, event):
        if self.closed:
            self.tray.hide()
            event.accept()
            return
        event.ignore()
        if self.closing:
            return
        if not self.exit_requested and self.hide_to_tray():
            return
        if self.rule_timer.isActive():
            self.apply_rules()
        if self.pending_volumes:
            self.save_volume()
        self.closing = True
        self.auto_timer.stop()
        self.centralWidget().setEnabled(False)
        self.status.setText('Выключаю захват…')
        def finish():
            error = None
            try:
                self.backend.close()
            except Exception as exc:
                error = f'Не удалось полностью завершить захват: {exc}'
            finally:
                self.queue.put(('closed', error))
        threading.Thread(target=finish, daemon=True).start()


def main():
    application = QApplication(sys.argv)
    application.setStyle('Fusion')
    try:
        window = App()
    except Exception as exc:
        QMessageBox.critical(None, 'TeamSpeak Audio Control', str(exc))
        return 1
    window.show()
    return application.exec()


if __name__ == '__main__':
    raise SystemExit(main())
