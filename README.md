# TeamSpeak Audio Control

**English** | [Русский](README.ru.md)

An app for controlling screen-sharing audio in **TeamSpeak 6** on Windows.
Choose which applications are heard in your stream and adjust their volume
without changing their system volume.

## Features

- Select applications for audio capture.
- Adjust each application's volume from 0 to 200%; double-click to reset to 100%.
- Automatic inclusion and exclusion rules.
- Automatic connection, Windows startup and system tray support.
- Light and dark themes, with saved settings.
- Single-file portable version without installation or Python.

## How it works

The app loads a small DLL into TeamSpeak to intercept audio capture and mix sound
from the selected applications. TeamSpeak files on disk remain unchanged.
