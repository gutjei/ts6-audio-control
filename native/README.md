# Native integration

The C++/Qt Widgets controller links `transport.cpp` and `audio_inventory.cpp`
directly into its executable using `controller_api.h`. `audio_mixer.dll` contains the
agent, COM proxies, MinHook and existing mixer. `audio_clock.exe` is a silent process
without Python or Qt; it exits when its controlling process exits, including crashes.
Local CMake / `build-qt.cmd` builds compile the application, mixer and clock.
Application CI uses a released mixer DLL instead (see below). Production does not
depend on separate controller DLLs or any file in `_local/`.

## Independent mixer releases

`native/mixer` is a standalone CMake project requiring Windows x64 and MSVC,
but no Qt. Build it with:

```powershell
cmake -S native/mixer -B build/mixer -G "Visual Studio 17 2022" -A x64
cmake --build build/mixer --config Release
```

Three versions have separate purposes:

- `VERSION`: application release (`vMAJOR.MINOR.PATCH`).
- `native/mixer/VERSION`: mixer implementation release (`mixer-vMAJOR.MINOR.PATCH`).
- `transport_protocol.h`: wire protocol version; change it for incompatible IPC changes.

To release a changed mixer, update `native/mixer/VERSION` and its
`RELEASE_NOTES.md`, commit the sources, and push the matching `mixer-v...` tag.
The mixer workflow builds and publishes `audio_mixer.dll`, its SHA-256 checksum
and the MinHook license. Mixer releases are never marked Latest. Published mixer
binaries must not be replaced; the workflow rejects attempts to overwrite them.

After publication, set the application's `mixer-version.txt` to that version.
Application CI downloads that exact release and verifies the DLL against the
SHA-256 digest returned by GitHub's release asset API. The digest is obtained
from GitHub at build time, not pinned separately in source. A missing release,
missing digest or hash mismatch fails the build; there is no source-build fallback.
MSI and both portable packages include the same downloaded DLL. GUI-only releases
leave `mixer-version.txt` unchanged and reuse those bytes.

For the initial rollout, publish `mixer-v1.0.0` from a commit containing the mixer
workflow before building the next application release. Application CI cannot
download this dependency until it exists. The 14-day Actions artifact is only
used to transfer files between CI jobs; the persistent dependency is the Release asset.

To reproduce the application CI build locally (GitHub CLI authentication required):

```powershell
./download-mixer.ps1
cmd /c 'build-qt.cmd ".deps\qt" ".deps\mixer\audio_mixer.dll"'
```

Omit the second argument for local development using mixer sources. Passing it
sets CMake's `MIXER_DLL` and excludes the mixer and MinHook compilation targets.
Application releases explicitly become Latest; the updater also accepts only
stable `vMAJOR.MINOR.PATCH` tags, so mixer tags cannot be installed as GUI updates.
The existing loaded-DLL path check still applies: replacing a loaded mixer or
changing the cache directory requires restarting TeamSpeak.

## Attach and capture

The controller selects the root TeamSpeak.exe process using Toolhelp, checks x64
and the presence of TeamSpeak.dll, and loads the agent using LoadLibraryW in a
remote thread. Forwarded exports are resolved against their owning module in the
target. It then invokes the agent export outside the loader lock. Code does not
request debug privileges or automatically elevate.

The agent reserves one local named pipe per target, restricted to the current
user. The client checks the server PID. Requests and replies have fixed lengths,
a magic number, protocol version, deadlines and bounded PID/gain arrays. Protocol
errors close the connection and clear the routing policy. The wire ABI is in
`transport_protocol.h`; audio buffers never enter this channel.

MinHook replaces only ActivateAudioInterfaceAsync. Calls originating outside the
selected module and calls for other audio devices pass through unchanged. The
replacement redirects the underlying process-loopback to the silent process.
Completion and operation wrappers return an IAudioClient proxy; its capture
service returns an IAudioCaptureClient proxy backed by the existing mixer. COM
references own proxy lifetime, and capture destruction removes its mixer context.
The completion handler aggregates a free-threaded marshaler. No undocumented COM
vtable slots or TeamSpeak structure offsets are patched.

## Disconnect and limits

Disconnect, a broken pipe, an invalid command or an expired heartbeat clears the
allow-list. Existing proxies keep returning silence and release source captures.
The agent and pass-through activation hook remain loaded until target exit, because
TeamSpeak may still hold COM interfaces or an outstanding mixed PCM buffer. Idle
context managers sleep on events; reconnect does not create a second agent.

Late discovery of pre-existing streams is deliberately unavailable. Restart the
stream after connecting/reconnecting, especially if its old silent process exited.
When no controller is connected, newly created streams use normal TeamSpeak audio.
Updating an already loaded agent requires restarting TeamSpeak. Frida and native
control must not be mixed in the same target; there is no automatic Frida fallback.

The diagnostic mode scopes the hook to a private child executable instead of
TeamSpeak.dll. `--diagnose` validates loading and IPC only. Real WASAPI, target
updates, and receiver-side audio need integration checks before release. All local
fixtures and experimental Frida scripts are excluded from Git.

## Upstream references

- [MinHook 1.3.4](https://github.com/TsudaKageyu/minhook/tree/v1.3.4)
- [ActivateAudioInterfaceAsync and COM callback lifetime](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-activateaudiointerfaceasync)
- [Named pipe access control](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights)
- [CreateRemoteThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createremotethread)
