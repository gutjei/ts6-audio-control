"""Offline installation diagnostics; never attaches to TeamSpeak or plays audio."""
import json
from pathlib import Path
import subprocess
import tempfile
import time
import traceback


def run(output):
    result = {'ok': False, 'checks': []}
    child = session = script = root = None
    try:
        from PySide6.QtWidgets import QApplication, QWidget
        from router_backend import frida, ROOT, ROUTER_DLL
        from app_runtime import clock_command
        import ctypes
        application = QApplication.instance() or QApplication([])
        root = QWidget()
        root.resize(320, 200)
        application.processEvents()
        if root.grab().isNull():
            raise RuntimeError('Qt widget rendering failed')
        root.close()
        root = None
        result['checks'].append('Qt / PySide6')
        dll = ctypes.CDLL(str(ROUTER_DLL))
        dll.RouterSelfTest.restype = ctypes.c_int
        if dll.RouterSelfTest() != 0:
            raise RuntimeError('Native mixer self-check failed')
        result['checks'].append('native mixer')
        from audio_inventory import AudioInventory
        inventory = AudioInventory()
        try:
            deadline = time.monotonic() + 10
            while inventory.snapshot() is None:
                if time.monotonic() > deadline:
                    raise RuntimeError('Core Audio inventory did not initialize')
                inventory.wait(.25)
            result['checks'].append('Core Audio event subscriptions')
        finally:
            inventory.close()
        with tempfile.TemporaryDirectory(prefix='ts6-diagnostics-') as temporary:
            ready = Path(temporary) / 'ready'
            child = subprocess.Popen(clock_command() + ['--ready-file', str(ready)], creationflags=subprocess.CREATE_NO_WINDOW)
            deadline = time.monotonic() + 30
            while not ready.exists():
                if child.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Silent worker did not start')
                time.sleep(.05)
            worker_pid = int(ready.read_text(encoding='ascii'))
            result['checks'].append('silent worker')
            session = frida.get_local_device().attach(worker_pid)
            errors = []
            prefix = f'const ROUTER_DLL={json.dumps(str(ROUTER_DLL))};const SILENT_PID={worker_pid};const TEST_TARGET_MODULE="diagnostics-only.dll";\n'
            script = session.create_script(prefix + (ROOT / 'live_router.js').read_text(encoding='utf-8'))
            script.on('message', lambda message, data: errors.append(message) if message['type'] == 'error' else None)
            script.load()
            if script.exports_sync.selftest() != 0:
                raise RuntimeError('Injected mixer self-check failed')
            script.exports_sync.configure([], [])
            script.exports_sync.heartbeat()
            if len(script.exports_sync.stats()) != 16 or errors:
                raise RuntimeError(f'Frida diagnostic failed: {errors}')
            result['checks'].append('Frida + bundled JS + injected DLL')
            script.exports_sync.shutdown()
            script = None
            session.detach()
            session = None
            child.terminate()
            child.wait(timeout=10)
            child = None
        result['ok'] = True
    except Exception:
        result['error'] = traceback.format_exc()
    finally:
        if script:
            try:
                script.exports_sync.shutdown()
            except Exception:
                pass
        if session:
            try:
                session.detach()
            except Exception:
                pass
        if child:
            child.terminate()
            child.wait(timeout=10)
        if root:
            root.close()
    Path(output).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    return 0 if result['ok'] else 1
