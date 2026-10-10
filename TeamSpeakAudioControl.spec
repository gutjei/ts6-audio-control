# Build with: python -m PyInstaller --noconfirm TeamSpeakAudioControl.spec
from PyInstaller.utils.hooks import collect_all

frida_data, frida_binaries, frida_imports = collect_all('frida')
a = Analysis(
    ['main.py'],
    pathex=[],
    binaries=frida_binaries + [('build/audio_router_volume.dll', 'build')],
    datas=frida_data + [('live_router.js', '.'), ('README.md', '.')],
    hiddenimports=frida_imports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
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
