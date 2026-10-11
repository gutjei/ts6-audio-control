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
- Attach to an already running screen share on the supported TeamSpeak build.
- Light and dark themes, with saved settings.

## How it works

The app attaches to the TeamSpeak process using Frida, loads a custom DLL into it,
and hooks audio capture calls. The DLL mixes audio from the selected applications
at their configured volumes and feeds it into the stream. The hooks operate in
process memory without modifying TeamSpeak files on disk.

An unofficial project, not affiliated with the TeamSpeak developers.
