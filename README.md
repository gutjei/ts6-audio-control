# TeamSpeak Audio Control

**English** | [Русский](README.ru.md)

An app for controlling screen-sharing audio in **TeamSpeak 6** on Windows.
Choose which applications are heard in your stream and adjust their volume individually.
Your applications’ system volume stays unchanged.

[Download for Windows](https://github.com/gutjei/ts6-audio-control/releases/latest)

## Features

- Choose which applications to capture audio from.
- Adjust each application’s volume from 0 to 200%. Double-click the slider to reset it to 100%.
- Automatically include or exclude applications using rules.
- Automatic connection to TeamSpeak, launch at Windows startup, and system tray support.
- Connect before starting screen sharing; restart an existing stream after connecting.
- Light and dark themes, with saved settings.

## How it works

The app loads a small native DLL into TeamSpeak. MinHook intercepts process-loopback
activation from TeamSpeak.dll; COM wrappers feed the existing C++ mixer into that
capture. Control commands use a local named pipe, and PCM stays in native code.
A separate native silent process supplies the underlying capture clock. Frida and
its JavaScript runtime are not included in the production build. The UI still uses
Python and Qt Widgets. TeamSpeak files on disk are not modified.

Connect the panel **before** starting the stream. This native backend does not
discover a capture created before connection. Disconnecting mutes a managed stream;
restart it to restore normal TeamSpeak capture. Loss of controller heartbeat also
mutes managed captures. New streams created while disconnected use normal capture.
Only one controller may connect to a target. Restart TeamSpeak after changing the
agent DLL version or after using Frida experiments.

## Building

Use Windows x64, Python x64 and Visual Studio C++ Build Tools with the Windows SDK:

```powershell
python -m pip install -r requirements-build.txt
.\build-native.cmd
python -m PyInstaller --noconfirm --clean TeamSpeakAudioControl.spec
.\dist\TeamSpeakAudioControl.exe --diagnose diagnostics.json
```

The offline diagnostic uses its own child processes, not TeamSpeak. It checks Qt,
audio inventory, the native clock, DLL loading, hook installation and control IPC.
It does not verify a real screen share or audio at the receiving peer. Local tests
and Frida experiments belong in ignored `_local/` and are not release dependencies.

MinHook 1.3.4 is vendored under its BSD license; the executable includes its license
in `licenses/minhook`. See [native integration](native/README.md) for lifecycle details.

An unofficial project, not affiliated with the TeamSpeak developers.
