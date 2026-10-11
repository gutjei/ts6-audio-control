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
    source = ROOT / 'build' / 'audio_mixer.dll'
    # TeamSpeak can keep the injected DLL loaded after our UI exits. A content
    # address also keeps rebuilds from overwriting a module still used by a target.
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    cache = DATA_DIR / 'native' if FROZEN else ROOT / 'build' / 'loaded'
    destination = cache / digest / source.name
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
    return [str(ROOT / 'build' / 'audio_clock.exe'), '--parent', str(os.getpid())]


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
