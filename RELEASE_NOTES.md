# C++ / Qt Widgets preview (unreleased)

This branch replaces the production Frida integration with a native Windows
controller and agent, and ports the interface to C++17 / Qt Widgets. Python and
PySide are removed from the production application and build pipeline.

## Changes

- Optional single-file portable EXE automatically extracts and verifies a
  versioned per-user cache; no installer, Python or manual unpacking is required.
- Portable ZIP with Qt Core, Gui, Widgets and Windows plugins; extract the whole
  archive before starting the application. Python is not required.
- Existing JSON settings, volume controls, rules, themes, tray and Windows startup
  are supported by C++ code. Rules use PCRE2 instead of Python's regex engine.
- Native DLL loading, a MinHook activation hook, COM audio wrappers and bounded
  local IPC replace Frida and its JavaScript runtime.
- The silent clock is a small C++ executable without Python or Qt.
- A single controller owns each connection; malformed commands and expired
  heartbeats mute managed captures.
- Offline diagnostics exercise the native loading and control path in private
  child processes. Experimental Frida files remain local and are not packaged.

## Usage and compatibility

Requires Windows 11 x64 and TeamSpeak 6 x64. Connect the panel before starting
screen sharing with audio. **Restart an existing stream after connecting.**
Late attachment from v0.3.0 is not implemented by this native backend.

Disconnecting mutes a managed stream. Restart that stream to restore normal
TeamSpeak capture. Newly created streams while disconnected use normal capture.
After updating the injected DLL or using Frida experiments, restart TeamSpeak.
Settings remain in `%LOCALAPPDATA%\TS6AudioControl` for packaged applications.

This is a development preview pending validation in a real TeamSpeak screen
share, including listening at the receiving peer. Local WASAPI and offline
diagnostics do not establish compatibility with every TeamSpeak release.
