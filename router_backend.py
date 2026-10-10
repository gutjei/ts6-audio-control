import ctypes
import json
import os
import subprocess
import sys
import threading
import time

from app_runtime import ROOT, FROZEN, LOG_DIR, native_library, settings_path, clock_command
if not FROZEN:
    sys.path.insert(0, str(ROOT / '.deps'))
import frida
from live_policy import compile_rules, decide, resolve
import settings_store

ROUTER_DLL = native_library()


class Backend:
    def __init__(self, emit):
        self.emit = emit
        self.device = frida.get_local_device()
        self.dll = ctypes.CDLL(str(ROUTER_DLL))
        self.dll.RouterSessions.argtypes = [ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong), ctypes.c_uint]
        self.dll.RouterSessions.restype = ctypes.c_uint
        self.session = self.script = self.clock = None
        self.ts_pid = None
        self.error = None
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.rpc_lock = threading.Lock()
        self.log_lock = threading.Lock()
        self.settings = {'include': r'^firefox\.exe$', 'exclude': r'^teamspeak\.exe$', 'overrides': {}, 'volumes': {}}
        self.config_path = settings_path()
        if self.config_path.exists():
            try:
                self.settings = settings_store.load(self.config_path)
            except (ValueError, KeyError, TypeError, OSError) as e:
                self.emit('error', f'Настройки не прочитаны: {e}')
        log_dir = LOG_DIR
        log_dir.mkdir(parents=True, exist_ok=True)
        self.log = (log_dir / 'gui-router.jsonl').open('a', encoding='utf-8')
        self.thread = threading.Thread(target=self.poll, daemon=True)
        self.thread.start()

    def record(self, event):
        with self.log_lock:
            if not self.log.closed:
                self.log.write(json.dumps({'time': time.time(), **event}, ensure_ascii=False) + '\n')
                self.log.flush()

    def get_settings(self):
        with self.lock:
            return {**self.settings, 'overrides': dict(self.settings['overrides']), 'volumes': dict(self.settings['volumes'])}

    def set_settings(self, settings):
        with self.lock:
            self.settings = settings_store.save(self.config_path, settings)

    def connect(self):
        with self.rpc_lock:
            if self.session:
                return
            procs = self.device.enumerate_processes(scope='metadata')
            mains = [p for p in procs if p.name.lower() == 'teamspeak.exe' and '--type=' not in ' '.join(p.parameters.get('argv', []))]
            if len(mains) != 1:
                raise RuntimeError('Запусти один экземпляр TeamSpeak и повтори подключение.')
            for p in procs:
                argv = ' '.join(p.parameters.get('argv', []))
                if p.name.lower().startswith('python') and 'trace_audio.py' in argv:
                    raise RuntimeError('Сначала останови прежний trace_audio.py: одновременно два перехвата не поддерживаются.')
            self.clock = subprocess.Popen(clock_command(), creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                self.ts_pid = mains[0].pid
                session = self.device.attach(self.ts_pid)
                prefix = f'const ROUTER_DLL={json.dumps(str(ROUTER_DLL))}; const SILENT_PID={self.clock.pid};\n'
                script = session.create_script(prefix + (ROOT / 'live_router.js').read_text(encoding='utf-8'))
                script.on('message', self.on_message)
                session.on('detached', lambda reason, *args: self.emit('detached', reason))
                script.load()
                if script.exports_sync.selftest() != 0:
                    raise RuntimeError('Самопроверка микшера не прошла')
                self.session, self.script = session, script
                self.error = None
            except Exception:
                if 'session' in locals():
                    session.detach()
                self.clock.terminate(); self.clock.wait(timeout=5); self.clock = None
                raise
        self.emit('connected', self.ts_pid)

    def on_message(self, message, data):
        event = message.get('payload') if message['type'] == 'send' else {'event': 'error', 'message': message.get('stack', str(message))}
        self.record(event)
        self.emit('native', event)

    def poll(self):
        while not self.stop.is_set():
            try:
                procs = self.device.enumerate_processes(scope='metadata')
                rows = [{'pid': p.pid, 'ppid': p.parameters.get('ppid', 0), 'name': p.name} for p in procs]
                pid_names = {p['pid']: p['name'].lower() for p in rows}
                ids, states = (ctypes.c_ulong * 1024)(), (ctypes.c_ulong * 1024)()
                n = self.dll.RouterSessions(ids, states, 1024)
                audio = {}
                for i in range(n):
                    name = pid_names.get(ids[i])
                    if name:
                        audio[name] = max(audio.get(name, 0), states[i])
                settings = self.get_settings()
                rules = compile_rules(settings['include'], settings['exclude'])
                own = [os.getpid(), self.ts_pid or 0, self.clock.pid if self.clock else 0]
                selected, blocked = resolve(rows, settings['overrides'], rules, own)
                gains = settings_store.gains_for(rows, selected, settings)
                stats = None
                with self.rpc_lock:
                    if self.script:
                        self.script.exports_sync.configure(selected, gains)
                        stats = self.script.exports_sync.stats()
                self.emit('snapshot', {'processes': rows, 'audio': audio, 'selected': selected, 'blocked': blocked, 'stats': stats})
            except Exception as e:
                self.emit('error', str(e))
            self.stop.wait(0.5)

    def close(self):
        self.stop.set()
        # Serialize against an in-flight configure RPC so it cannot unmute after shutdown.
        with self.rpc_lock:
            if self.script:
                try:
                    self.script.exports_sync.shutdown()
                except Exception:
                    pass
            if self.session:
                try:
                    self.session.detach()
                except Exception:
                    pass
            self.script = self.session = None
        self.thread.join(timeout=5)
        if self.clock:
            self.clock.terminate(); self.clock.wait(timeout=5); self.clock = None
        with self.log_lock:
            self.log.close()
