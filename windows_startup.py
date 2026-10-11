"""Current-user Windows logon registration; no machine-wide settings."""
from pathlib import Path
import subprocess
import sys
import winreg

from app_runtime import FROZEN, ROOT

RUN_KEY = r'Software\Microsoft\Windows\CurrentVersion\Run'
VALUE_NAME = 'TeamSpeakAudioControl'


def startup_command():
    if FROZEN:
        args = [sys.executable]
    else:
        pythonw = Path(sys.executable).with_name('pythonw.exe')
        if not pythonw.is_file():
            raise OSError('Не найден pythonw.exe для запуска без консольного окна')
        args = [str(pythonw), str(ROOT / 'audio_router_gui.py')]
    command = subprocess.list2cmdline(args)
    if len(command) > 260:
        raise ValueError('Путь слишком длинный для автозапуска Windows. Переместите приложение ближе к корню диска.')
    return command


def is_enabled():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as key:
            value, kind = winreg.QueryValueEx(key, VALUE_NAME)
            return kind == winreg.REG_SZ and bool(value)
    except FileNotFoundError:
        return False


def set_enabled(enabled):
    if enabled:
        command = startup_command()
        with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as key:
            winreg.SetValueEx(key, VALUE_NAME, 0, winreg.REG_SZ, command)
    else:
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as key:
                winreg.DeleteValue(key, VALUE_NAME)
        except FileNotFoundError:
            pass
