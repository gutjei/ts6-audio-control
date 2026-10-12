# v0.4.2 — Connection status and independent mixer releases

- Fixed a stale mixer warning remaining visible after reconnecting successfully
  to TeamSpeak. Settings warnings are preserved separately.
- The application now bundles the independently released mixer v1.0.0.
  Future GUI-only releases reuse the same DLL instead of rebuilding it.
- Mixer releases use separate `mixer-v...` tags and do not replace the latest
  application release used by the in-app updater.

Settings are preserved during upgrades and removal. MSI and both portable
packages include the mixer; no separate DLL installation is needed.

Requires Windows 11 x64 and TeamSpeak 6 x64. Restart TeamSpeak after this update
to load the newly released mixer. Restart an existing stream after reconnecting.
