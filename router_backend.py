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
from live_policy import compile_rules, resolve
import settings_store
from audio_inventory import AudioInventory
from mixer_control import MixerControl

ROUTER_DLL = native_library()


class Backend:
    def __init__(self, emit):
        self.emit = emit
        self.device = frida.get_local_device()
        self.session = self.script = self.clock = None
        self.ts_pid = None
        self.error = None
        self.stop = threading.Event()
        self.detach_state = None
        self.control = MixerControl()
        self.settings_revision = 0
        self.connection_revision = 0
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
        try:
            self.inventory = AudioInventory()
        except Exception:
            self.log.close()
            raise
        self.thread = threading.Thread(target=self.run, daemon=True)
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
            self.settings_revision += 1
        self.inventory.wake()

    def refresh_sources(self):
        self.inventory.refresh()

    def connect(self, wait_for_process=False):
        with self.rpc_lock:
            if self.stop.is_set():
                return
            self._handle_detachment_locked()
            if self.session:
                self.emit('connected', self.ts_pid)
                return
            procs = self.device.enumerate_processes(scope='metadata')
            mains = [p for p in procs if p.name.lower() == 'teamspeak.exe' and '--type=' not in ' '.join(p.parameters.get('argv', []))]
            if not mains and wait_for_process:
                self.emit('connect-waiting', None)
                return
            if len(mains) != 1:
                raise RuntimeError('Запусти один экземпляр TeamSpeak и повтори подключение.')
            for p in procs:
                argv = ' '.join(p.parameters.get('argv', []))
                if p.name.lower().startswith('python') and 'trace_audio.py' in argv:
                    raise RuntimeError('Сначала останови прежний trace_audio.py: одновременно два перехвата не поддерживаются.')
            # Retry cleanup before starting a new worker if a previous one failed.
            self._release_locked()
            self.emit('connecting', None)
            self.clock = subprocess.Popen(clock_command(), creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                self.ts_pid = mains[0].pid
                state = {'event': threading.Event(), 'reason': None}
                self.detach_state = state
                session = self.device.attach(self.ts_pid)
                self.session = session
                prefix = f'const ROUTER_DLL={json.dumps(str(ROUTER_DLL))}; const SILENT_PID={self.clock.pid};\n'
                script = session.create_script(prefix + (ROOT / 'live_router.js').read_text(encoding='utf-8'))
                self.script = script
                script.on('message', self.on_message)
                session.on('detached', lambda reason, *args: self.on_detached(state, reason))
                script.load()
                if script.exports_sync.selftest() != 0:
                    raise RuntimeError('Самопроверка микшера не прошла')
                if state['event'].is_set() or session.is_detached:
                    raise RuntimeError('TeamSpeak закрылся во время подключения. Запустите его и повторите.')
                self.control.reset()
                self.connection_revision += 1
                self.error = None
            except Exception:
                self._release_locked()
                raise
            # Serialize status publication with cleanup and the health worker.
            self.emit('connected', self.ts_pid)
        self.inventory.wake()

    def on_detached(self, state, reason):
        # Frida may call this during detach() while rpc_lock is held. Only signal
        # here; each session owns its state so late callbacks cannot end a new one.
        state['reason'] = reason
        state['event'].set()
        if not self.stop.is_set():
            self.inventory.wake()

    def _release_locked(self):
        """Stop routing and release one connection, keeping inventory/settings alive."""
        if self.script:
            try:
                self.script.exports_sync.shutdown()
            except Exception:
                pass  # A terminated target no longer accepts RPCs.
        if self.session:
            try:
                self.session.detach()
            except Exception:
                if not self.session.is_detached:
                    raise  # Do not permit a second attachment over an active one.
        self.script = self.session = None
        self.ts_pid = None
        self.control.reset()
        if self.clock:
            if self.clock.poll() is None:
                self.clock.terminate()
            try:
                self.clock.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.clock.kill()
                self.clock.wait(timeout=5)
            self.clock = None
        self.detach_state = None
        self.connection_revision += 1

    def _handle_detachment_locked(self):
        if self.detach_state and self.detach_state['event'].is_set():
            reason = self.detach_state['reason']
            self._release_locked()
            self.emit('detached', reason)

    def disconnect(self):
        with self.rpc_lock:
            self._release_locked()
            self.emit('disconnected', None)
        self.inventory.wake()

    def on_message(self, message, data):
        event = message.get('payload') if message['type'] == 'send' else {'event': 'error', 'message': message.get('stack', str(message))}
        self.record(event)
        self.emit('native', event)

    def run(self):
        cached = None
        view_key = None
        next_health = time.monotonic()
        previous_error = None
        while not self.stop.is_set():
            try:
                notified = self.inventory.wait(max(0, next_health - time.monotonic()))
                if self.stop.is_set():
                    break
                # Handle target exit even if the audio inventory has no snapshot.
                with self.rpc_lock:
                    self._handle_detachment_locked()
                if notified or cached is None:
                    cached = self.inventory.snapshot()
                if cached is None:
                    next_health = time.monotonic() + 1
                    continue
                revision, rows, sessions = cached
                with self.lock:
                    settings = {**self.settings, 'overrides': dict(self.settings['overrides']), 'volumes': dict(self.settings['volumes'])}
                    settings_revision = self.settings_revision
                with self.rpc_lock:
                    if self.stop.is_set():
                        break
                    self._handle_detachment_locked()
                    key = revision, settings_revision, self.connection_revision
                    if key != view_key:
                        pid_names = {p['pid']: p['name'].lower() for p in rows}
                        audio = {}
                        for pid, state in sessions.items():
                            if pid in pid_names:
                                name = pid_names[pid]
                                audio[name] = max(audio.get(name, 0), state)
                        rules = compile_rules(settings['include'], settings['exclude'])
                        own = [os.getpid(), self.ts_pid or 0, self.clock.pid if self.clock else 0]
                        selected, blocked = resolve(rows, settings['overrides'], rules, own)
                        gains = settings_store.gains_for(rows, selected, settings)
                        if self.script:
                            self.control.apply(self.script, selected, gains)
                        self.emit('snapshot', {'processes': rows, 'audio': audio, 'selected': selected, 'blocked': blocked, 'stats': None})
                        if view_key is None or key[2] != view_key[2]:
                            next_health = 0
                        view_key = key
                    if time.monotonic() >= next_health:
                        if self.script:
                            self.emit('stats', self.control.health(self.script))
                        next_health = time.monotonic() + (1 if self.script else 60)
                previous_error = None
            except Exception as e:
                # An invalid inventory must not keep an old allow-list alive.
                with self.rpc_lock:
                    if self.script:
                        try:
                            self.control.apply(self.script, [], [])
                        except Exception:
                            self.control.reset()
                cached = view_key = None
                next_health = time.monotonic() + 1
                if str(e) != previous_error:
                    self.emit('error', str(e))
                    previous_error = str(e)

    def close(self):
        self.stop.set()
        self.inventory.wake()
        # Serialize against an in-flight configure RPC so it cannot unmute after shutdown.
        try:
            with self.rpc_lock:
                self._release_locked()
        finally:
            self.thread.join()
            self.inventory.close()
            with self.log_lock:
                self.log.close()
