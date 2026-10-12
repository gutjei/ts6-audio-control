# v0.4.1 — MSI wizard and application icons

- Added a Russian-language MSI installation and maintenance wizard, with
  installation confirmation, progress, cancellation and optional app launch
  after installation. Passive updates continue to use the native updater.
- Fixed missing application icons in the installed EXE, Start menu shortcut,
  Windows installed-apps list and single-file portable executable.
- Upgrades from v0.4.0 remain supported despite the installer language change.

Settings are preserved during upgrades and removal. The experimental option to
remove settings is not included in this release.

Requires Windows 11 x64 and TeamSpeak 6 x64. Restart TeamSpeak after updating.
