# TeamSpeak Audio Control v0.3.0

Connect to an already running screen share without restarting it on the supported TeamSpeak build.

## What's new

- Discover an existing audio capture and route selected applications into it without restarting the stream.
- Keep per-packet audio processing in the native mixer after initial discovery.
- Reconnect to the same running stream with fresh capture workers and reset statistics.
- Check TeamSpeak compatibility before attaching to an existing capture. Other builds retain the usual workflow: connect the panel, then start the stream.
- Show connection guidance and explain the audio behavior when disconnecting.

## Getting started

1. Download **TeamSpeakAudioControl.exe** from the assets below.
2. Start TeamSpeak 6 and connect from the panel, or enable automatic connection in settings.
3. Enable audio in your screen share, then select applications and adjust their volume. An existing stream attaches automatically on the supported build.

Requires Windows 11 x64 and TeamSpeak 6 x64. Run only one panel at a time.
Settings are stored in `%LOCALAPPDATA%\TS6AudioControl` and retained across updates.

## Compatibility and disconnect behavior

- Existing-stream attachment is enabled only for the verified TeamSpeak.dll build (SHA-256 `a302bcc4c6ed341ecb922d0e3c29201f5bb385705edbd6f6d6e227eb7b505198`). Compatibility is checked automatically.
- Disconnecting from a previously running stream restores its original audio. A stream started after panel connection becomes silent on disconnect; restart that stream to restore normal capture.
- Capture cleanup when a stream stops still depends on TeamSpeak releasing its audio capture object. This release does not add separate stream-stop detection.

The executable is unsigned. Verify its SHA-256 checksum using **SHA256SUMS.txt**.
This unofficial tool hooks audio capture in the TeamSpeak process without modifying TeamSpeak files on disk.
