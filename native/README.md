# Native integration

The C++/Qt Widgets controller links `transport.cpp` and `audio_inventory.cpp`
directly into its executable using `controller_api.h`. `audio_mixer.dll` contains the
agent, COM proxies, MinHook and existing mixer. `audio_clock.exe` is a silent process
without Python or Qt; it exits when its controlling process exits, including crashes.
The application, mixer and clock are built by CMake / `build-qt.cmd`. Production
does not depend on separate controller DLLs or any file in `_local/`.

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
