"""Paths and child-process commands shared by source and packaged applications."""
import hashlib
import os
from pathlib import Path
import shutil
import sys
import tempfile

FROZEN = bool(getattr(sys, 'frozen', False))
ROOT = Path(__file__).resolve().parent
DATA_DIR = Path(os.environ.get('LOCALAPPDATA', Path.home() / 'AppData' / 'Local')) / 'TS6AudioControl' if FROZEN else ROOT
LOG_DIR = DATA_DIR / 'logs' if FROZEN else ROOT / '_local' / 'logs'


def native_library():
    source = ROOT / 'build' / 'audio_router_volume.dll'
    if not FROZEN:
        return source
    # TeamSpeak can keep the injected DLL loaded after our UI exits. Keep it
    # outside PyInstaller's temporary extraction directory, keyed by content.
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    destination = DATA_DIR / 'native' / digest / source.name
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if hashlib.sha256(destination.read_bytes()).hexdigest() != digest:
            raise RuntimeError('Повреждена DLL микшера в каталоге настроек.')
        return destination
    handle, temporary = tempfile.mkstemp(dir=destination.parent, suffix='.tmp')
    os.close(handle)
    try:
        shutil.copyfile(source, temporary)
        try:
            os.rename(temporary, destination)
        except FileExistsError:
            if hashlib.sha256(destination.read_bytes()).hexdigest() != digest:
                raise
    finally:
        Path(temporary).unlink(missing_ok=True)
    return destination


def clock_command():
    if FROZEN:
        return [sys.executable, '--silent-clock']
    return [sys.executable, str(ROOT / 'silent_clock.py')]


def settings_path():
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    destination = DATA_DIR / 'settings.json'
    # A user can carry settings.json alongside the downloaded exe on first run.
    # Existing per-user preferences always win; releases ship no personal settings.
    if FROZEN and not destination.exists():
        portable = Path(sys.executable).parent / 'settings.json'
        if portable.is_file():
            import settings_store
            settings_store.save(destination, settings_store.load(portable))
    return destination
