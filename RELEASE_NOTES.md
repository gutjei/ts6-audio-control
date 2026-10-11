# TeamSpeak Audio Control v0.2.0

A redesigned Qt interface with automatic connection and background operation.

## What's new

- A single-page interface with light, dark, and system themes.
- Only running applications with audio sessions appear in the list; saved preferences are restored when they return.
- Volume controls occupy half the table. Double-click a slider to reset it to 100%.
- Automatic connection to TeamSpeak, including reconnection after it restarts.
- Manual connection controls are disabled while automatic connection is enabled. The connected status shows the TeamSpeak PID.
- Optional launch at Windows sign-in and minimize/close to the system tray.
- Improved connection cleanup, standalone Qt packaging, and installation diagnostics.
- Concise English and Russian documentation.

## Getting started

1. Download **TeamSpeakAudioControl.exe** from the assets below.
2. Start TeamSpeak 6 and connect from the panel, or enable automatic connection in settings.
3. Start a new screen share with audio enabled, then select applications and adjust their volume.

Requires Windows 11 x64 and TeamSpeak 6 x64. Run only one panel at a time.
Settings are stored in `%LOCALAPPDATA%\TS6AudioControl` and retained across updates.
Automatic connection does not start TeamSpeak or a screen share. After reconnecting, start the screen share again.

The executable is unsigned. Verify its SHA-256 checksum using `SHA256SUMS.txt`.
This unofficial tool hooks audio capture in the TeamSpeak process; compatibility may change with TeamSpeak updates.
