# v0.4.3 — Audio source filtering and excluded applications

- Fixed `0x80070005` when broad inclusion rules such as `.*` selected hidden
  system ancestor processes. Rules now select only eligible audio sources.
- Hide system-sound sessions, including those attributed to Explorer, and
  audio processes that cannot be opened with the required capture permissions.
- Move excluded applications, including TeamSpeak, to the bottom of the list.
- Add “Показывать исключённые процессы” in Settings. Uncheck it to hide excluded
  applications immediately; the preference is preserved between launches.
- Continue using the independently released mixer v1.0.0 without changes.

Settings are preserved during upgrades and removal. MSI and both portable
packages include the mixer; no separate DLL installation is needed.

Requires Windows 11 x64 and TeamSpeak 6 x64. Close the previous control panel
before launching the updated version. Restart an existing stream after reconnecting.
