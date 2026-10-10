"""Small local control panel for live TeamSpeak screen-share audio selection."""
import queue
import re
import threading
import tkinter as tk
from tkinter import ttk, messagebox
from live_policy import compile_rules, decide
from router_backend import Backend


class App:
    def __init__(self, root):
        self.root = root
        self.root.title('TeamSpeak · Звук трансляции')
        self.root.geometry('850x740')
        self.root.minsize(730, 640)
        self.root.configure(bg='#111827')
        self.queue = queue.Queue()
        self.backend = Backend(lambda kind, value: self.queue.put((kind, value)))
        self.settings = self.backend.get_settings()
        self.snapshot = None
        self.connected = False
        self.closing = False
        self.volume_pending = None
        self.selected_app = None
        self.syncing_volume = False
        style = ttk.Style()
        style.theme_use('clam')
        style.configure('.', font=('Segoe UI', 10), background='#111827', foreground='#e5e7eb')
        style.configure('TButton', background='#243449', padding=(12, 7))
        style.map('TButton', background=[('active', '#365571')])
        style.configure('TEntry', fieldbackground='#1f2937', foreground='#f9fafb', insertcolor='white', padding=7)
        style.configure('Treeview', background='#182231', fieldbackground='#182231', foreground='#e5e7eb', rowheight=32)
        style.configure('Treeview.Heading', background='#243449', padding=7)
        style.map('Treeview', background=[('selected', '#274969')])
        outer = ttk.Frame(root, padding=22); outer.pack(fill='both', expand=True)
        ttk.Label(outer, text='Звук трансляции', font=('Segoe UI', 21, 'bold')).pack(anchor='w')
        ttk.Label(outer, text='Выбирай приложения. Изменения действуют во время шаринга.', foreground='#94a3b8').pack(anchor='w', pady=(4, 15))
        line = ttk.Frame(outer); line.pack(fill='x')
        self.status = tk.StringVar(value='Не подключено к TeamSpeak')
        ttk.Label(line, textvariable=self.status).pack(side='left')
        self.connect_button = ttk.Button(line, text='Подключить', command=self.connect)
        self.connect_button.pack(side='right')
        rules = ttk.Frame(outer); rules.pack(fill='x', pady=(18, 5)); rules.columnconfigure(1, weight=1)
        self.include = tk.StringVar(value=self.settings['include'])
        self.exclude = tk.StringVar(value=self.settings['exclude'])
        for row, (label, variable) in enumerate([('Include regexp', self.include), ('Exclude regexp', self.exclude)]):
            ttk.Label(rules, text=label).grid(row=row, column=0, sticky='w', padx=(0, 12), pady=4)
            ttk.Entry(rules, textvariable=variable).grid(row=row, column=1, sticky='ew', pady=4)
        self.rule_status = tk.StringVar(value='Include выбирает автоматически; галочка задаёт выбор вручную. Exclude всегда главнее.')
        ttk.Label(outer, textvariable=self.rule_status, wraplength=720, foreground='#94a3b8').pack(anchor='w', pady=(3, 12))
        toolbar = ttk.Frame(outer); toolbar.pack(fill='x', pady=(0, 8))
        ttk.Button(toolbar, text='Обновить аудиосессии', command=self.backend.refresh_sources).pack(side='left')
        ttk.Button(toolbar, text='Сбросить ручной выбор', command=self.reset_manual).pack(side='right')
        footer = ttk.Frame(outer); footer.pack(side='bottom', fill='x')
        body = ttk.Frame(outer); body.pack(fill='both', expand=True)
        self.tree = ttk.Treeview(body, columns=('check', 'name', 'volume', 'state', 'rule'), show='headings', selectmode='browse', height=6)
        for key, title, width in [('check', 'Захват', 62), ('name', 'Приложение', 220), ('volume', 'Уровень', 80), ('state', 'Аудиосессия', 125), ('rule', 'Правило', 175)]:
            self.tree.heading(key, text=title); self.tree.column(key, width=width, minwidth=45, stretch=key!='check')
        self.tree.tag_configure('blocked', foreground='#fbbf24')
        self.tree.tag_configure('on', foreground='#86efac')
        self.tree.pack(side='left', fill='both', expand=True)
        scroll = ttk.Scrollbar(body, orient='vertical', command=self.tree.yview); scroll.pack(side='right', fill='y')
        self.tree.configure(yscrollcommand=scroll.set)
        self.tree.bind('<Button-1>', self.click)
        self.tree.bind('<space>', lambda e: self.toggle(self.tree.focus()))
        self.tree.bind('<<TreeviewSelect>>', self.select_volume)
        volume_head = ttk.Frame(footer); volume_head.pack(fill='x', pady=(12, 0))
        self.volume_title = tk.StringVar(value='Выбери приложение в списке, чтобы настроить громкость')
        ttk.Label(volume_head, textvariable=self.volume_title).pack(side='left')
        self.volume_label = tk.StringVar(value='100%')
        ttk.Label(volume_head, textvariable=self.volume_label, foreground='#93c5fd').pack(side='right')
        volume_line = ttk.Frame(footer); volume_line.pack(fill='x', pady=(5, 2))
        ttk.Label(volume_line, text='0%').pack(side='left', padx=(0, 8))
        self.volume_value = tk.DoubleVar(value=100)
        self.volume_scale = ttk.Scale(volume_line, from_=0, to=200, variable=self.volume_value, command=self.change_volume, state='disabled')
        self.volume_scale.pack(side='left', fill='x', expand=True)
        ttk.Label(volume_line, text='200%').pack(side='left', padx=8)
        self.volume_reset = ttk.Button(volume_line, text='100%', command=self.reset_volume, state='disabled')
        self.volume_reset.pack(side='right')
        ttk.Label(footer, text='Уровень трансляции сохраняется по имени exe. 100% — исходная громкость.', foreground='#94a3b8').pack(anchor='w', pady=(1, 2))
        self.stats = tk.StringVar(value='Для первого подключения один раз перезапусти шаринг со звуком.')
        ttk.Label(footer, textvariable=self.stats, foreground='#93c5fd', wraplength=720).pack(anchor='w', pady=(14, 5))
        ttk.Label(footer, text='Захватываются деревья процессов. При закрытии панели управляемый поток замолкает.\nПосле подключения можно менять галочки и regexp без перезапуска трансляции.', foreground='#94a3b8', wraplength=720).pack(anchor='w')
        self.pending = None
        self.include.trace_add('write', self.schedule_rules)
        self.exclude.trace_add('write', self.schedule_rules)
        root.protocol('WM_DELETE_WINDOW', self.close)
        root.after(100, self.events)

    def schedule_rules(self, *_):
        if self.pending:
            self.root.after_cancel(self.pending)
        self.pending = self.root.after(350, self.apply_rules)

    def apply_rules(self):
        self.pending = None
        candidate = {**self.settings, 'include': self.include.get(), 'exclude': self.exclude.get()}
        try:
            compile_rules(candidate['include'], candidate['exclude'])
            self.backend.set_settings(candidate)
        except (re.error, OSError) as e:
            self.rule_status.set(f'Ошибка: {e}. Продолжают действовать последние корректные правила.')
            return
        self.settings = candidate
        self.rule_status.set('Правила сохранены · Include + ручной выбор · Exclude всегда главнее')
        self.refresh()

    def toggle(self, name):
        if not name:
            return
        rules = compile_rules(self.settings['include'], self.settings['exclude'])
        active, reason = decide(name, self.settings['overrides'], rules)
        if reason in ('exclude', 'TeamSpeak исключён'):
            self.rule_status.set('Это приложение запрещено exclude. Сначала измени regexp исключения.')
            return
        self.settings['overrides'][name] = not active
        self.backend.set_settings({**self.settings, 'overrides': dict(self.settings['overrides'])})
        self.refresh()

    def click(self, event):
        if self.tree.identify_column(event.x) == '#1':
            self.toggle(self.tree.identify_row(event.y))

    def reset_manual(self):
        self.settings['overrides'] = {}
        self.backend.set_settings(dict(self.settings)); self.refresh()

    def select_volume(self, _event=None):
        selected = self.tree.selection()
        self.selected_app = selected[0] if selected else None
        if not self.selected_app:
            self.volume_scale.configure(state='disabled'); self.volume_reset.configure(state='disabled')
            return
        self.syncing_volume = True
        level = self.settings['volumes'].get(self.selected_app, 100)
        self.volume_value.set(level)
        self.volume_title.set(f'Громкость · {self.selected_app}')
        self.volume_label.set(f'{level:g}%')
        self.volume_scale.configure(state='normal'); self.volume_reset.configure(state='normal')
        self.syncing_volume = False

    def change_volume(self, value):
        if self.syncing_volume or not self.selected_app:
            return
        level = max(0, min(200, round(float(value))))
        self.settings['volumes'][self.selected_app] = level
        self.volume_label.set(f'{level}%')
        if self.tree.exists(self.selected_app):
            self.tree.set(self.selected_app, 'volume', f'{level}%')
        if self.volume_pending:
            self.root.after_cancel(self.volume_pending)
        self.volume_pending = self.root.after(120, self.save_volume)

    def save_volume(self):
        self.volume_pending = None
        try:
            self.backend.set_settings(self.settings)
            self.rule_status.set('Настройки сохранены · Громкость привязана к имени exe, а не PID')
        except (ValueError, OSError) as e:
            self.rule_status.set(f'Не удалось сохранить громкость: {e}')

    def reset_volume(self):
        self.volume_value.set(100); self.change_volume(100)

    def refresh(self):
        if not self.snapshot:
            return
        data = self.snapshot
        rules = compile_rules(self.settings['include'], self.settings['exclude'])
        names = set(data['audio']) | set(self.settings['overrides']) | set(self.settings['volumes'])
        old = set(self.tree.get_children())
        for name in sorted(names):
            active, reason = decide(name, self.settings['overrides'], rules)
            blocked = data['blocked'].get(name)
            if blocked:
                reason = 'Блок: ' + ', '.join(blocked)
            state = 'активна' if data['audio'].get(name) == 1 else 'есть, молчит' if name in data['audio'] else 'нет аудиосессии'
            values = ('☑' if active else '☐', name, f"{self.settings['volumes'].get(name, 100):g}%", state, reason)
            tags = ('blocked',) if blocked else ('on',) if active else ()
            if name in old:
                current = self.tree.item(name)
                if tuple(map(str, current['values'])) != tuple(map(str, values)) or tuple(current['tags']) != tags:
                    self.tree.item(name, values=values, tags=tags)
                old.remove(name)
            else:
                self.tree.insert('', 'end', iid=name, values=values, tags=tags)
        for name in old:
            self.tree.delete(name)

    def connect(self):
        self.connect_button.configure(state='disabled')
        self.status.set('Подключение…')
        def work():
            try:
                self.backend.connect()
            except Exception as e:
                self.queue.put(('connect-error', str(e)))
        threading.Thread(target=work, daemon=True).start()

    def events(self):
        if self.closing:
            return
        while True:
            try:
                kind, value = self.queue.get_nowait()
            except queue.Empty:
                break
            if kind == 'snapshot':
                self.snapshot = value; self.refresh()
            elif kind == 'stats':
                contexts, sources, packets, audible, errors, code = value[:6]
                self.stats.set(('Ожидаю новый захват: перезапусти шаринг с включённым звуком.' if not contexts else f'Потоков: {contexts}  ·  Источников: {sources}  ·  Пакетов со звуком: {audible}  ·  Буфер: 100 мс') + (f'  ·  Ошибка 0x{code:08x} ({errors})' if errors else ''))
            elif kind == 'connected':
                self.connected = True; self.status.set(f'Подключено · TeamSpeak PID {value}')
                self.connect_button.configure(text='Подключено')
            elif kind == 'connect-error':
                self.status.set(value); self.connect_button.configure(state='normal', text='Повторить')
            elif kind == 'detached':
                self.connected = False; self.status.set('TeamSpeak отключён. Закрой и открой панель для нового подключения.')
            elif kind == 'error':
                self.rule_status.set(value)
            elif kind == 'native' and value.get('event') == 'error':
                self.rule_status.set(value.get('message', str(value)))
        self.root.after(100, self.events)

    def close(self):
        if self.pending:
            self.root.after_cancel(self.pending); self.apply_rules()
        if self.volume_pending:
            self.root.after_cancel(self.volume_pending); self.save_volume()
        self.closing = True; self.status.set('Выключаю захват…'); self.root.update_idletasks()
        def finish():
            self.backend.close(); self.queue.put(('closed', None))
        threading.Thread(target=finish, daemon=True).start()
        def wait():
            try:
                while self.queue.get_nowait()[0] != 'closed':
                    pass
            except queue.Empty:
                self.root.after(100, wait); return
            self.root.destroy()
        self.root.after(100, wait)


def main():
    root = tk.Tk()
    try:
        app = App(root)
    except Exception as exc:
        messagebox.showerror('TeamSpeak Audio Router', str(exc)); root.destroy()
        return 1
    else:
        root.mainloop()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
