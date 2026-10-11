# Build with: python -m PyInstaller --noconfirm TeamSpeakAudioControl.spec
import os
import sys

# Qt uses Windows ICU. Unrelated tools on PATH (e.g. Poppler) may ship an
# incompatible icuuc.dll with version-suffixed exports. Do not bundle those
# DLLs or third-party API-set shims; package hooks supply their own DLL paths.
windows = os.environ['SystemRoot']
os.environ['PATH'] = os.pathsep.join([
    sys.prefix, sys.base_prefix, os.path.join(windows, 'System32'), windows,
])

a = Analysis(
    ['main.py'],
    pathex=[],
    binaries=[('build/audio_mixer.dll', 'build'), ('build/audio_inventory.dll', 'build'),
              ('build/audio_transport.dll', 'build'), ('build/audio_clock.exe', 'build')],
    datas=[('README.md', '.'), ('native/vendor/minhook/LICENSE.txt', 'licenses/minhook')],
    hiddenimports=[],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=['tkinter', 'PyQt5', 'PyQt6', 'PySide2', 'frida'],
    noarchive=False,
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz, a.scripts, a.binaries, a.datas, [],
    name='TeamSpeakAudioControl',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,
    disable_windowed_traceback=False,
)
