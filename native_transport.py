"""Bounded local IPC to the native agent; no Frida or Python in the target."""
import ctypes
import math
from app_runtime import ROOT


class Request(ctypes.Structure):
    _fields_ = [('magic', ctypes.c_uint32), ('version', ctypes.c_uint32),
                ('command', ctypes.c_uint32), ('count', ctypes.c_uint32),
                ('silent_pid', ctypes.c_uint32), ('diagnostic', ctypes.c_uint32),
                ('pids', ctypes.c_uint32 * 64), ('gains', ctypes.c_float * 64)]


class Response(ctypes.Structure):
    _fields_ = [('magic', ctypes.c_uint32), ('version', ctypes.c_uint32),
                ('error', ctypes.c_uint32), ('reserved', ctypes.c_uint32),
                ('stats', ctypes.c_uint64 * 16)]


def check(error):
    if not error:
        return
    hints = {
        5: 'Нет доступа к TeamSpeak. Запустите панель и TeamSpeak с одинаковыми правами.',
        126: 'Аудиомодуль TeamSpeak ещё не загружен либо отсутствует нативная DLL.',
        193: 'Нужны 64-битные Windows, TeamSpeak и панель.',
        231: 'К TeamSpeak уже подключена другая панель.',
        1306: 'В TeamSpeak загружена другая версия микшера. Перезапустите TeamSpeak.',
        170: 'В TeamSpeak обнаружена Frida. Завершите эксперимент и перезапустите TeamSpeak.',
    }
    raise OSError(error, hints.get(error, ctypes.FormatError(error).strip()))


class Transport:
    def __init__(self):
        self.dll = ctypes.CDLL(str(ROOT / 'build' / 'audio_transport.dll'))
        signatures = {
            'TransportFindTarget': (ctypes.c_ulong, [ctypes.POINTER(ctypes.c_ulong)]),
            'TransportOpen': (ctypes.c_ulong, [ctypes.c_ulong, ctypes.c_wchar_p, ctypes.c_ulong,
                                             ctypes.c_int, ctypes.POINTER(ctypes.c_void_p)]),
            'TransportRequest': (ctypes.c_ulong, [ctypes.c_void_p, ctypes.POINTER(Request), ctypes.POINTER(Response)]),
            'TransportAlive': (ctypes.c_int, [ctypes.c_void_p]),
            'TransportClose': (None, [ctypes.c_void_p]),
        }
        for name, (result, args) in signatures.items():
            method = getattr(self.dll, name)
            method.restype, method.argtypes = result, args

    def find_target(self):
        pid = ctypes.c_ulong()
        error = self.dll.TransportFindTarget(ctypes.byref(pid))
        if error == 1168:
            return None
        if error == 234:
            raise RuntimeError('Запусти один экземпляр TeamSpeak и повтори подключение.')
        check(error)
        return pid.value

    def connect(self, pid, library, silent_pid, *, diagnostic=False):
        handle = ctypes.c_void_p()
        check(self.dll.TransportOpen(pid, str(library.resolve()), silent_pid, diagnostic, ctypes.byref(handle)))
        return Session(self.dll, handle)


class Session:
    def __init__(self, dll, handle):
        self.dll, self.handle = dll, handle
        # Shared command interface with MixerControl; the transport is native.
        self.exports_sync = self

    @property
    def is_detached(self):
        return not self.handle or not self.dll.TransportAlive(self.handle)

    def _request(self, command, pids=(), gains=()):
        if not self.handle:
            raise RuntimeError('Соединение с TeamSpeak закрыто.')
        data = Request(magic=0x36534154, version=1, command=command, count=len(pids))
        for i, (pid, gain) in enumerate(zip(pids, gains)):
            data.pids[i], data.gains[i] = pid, gain
        reply = Response()
        check(self.dll.TransportRequest(self.handle, ctypes.byref(data), ctypes.byref(reply)))
        return reply

    def configure(self, pids, gains):
        if len(pids) > 64 or len(pids) != len(gains):
            raise ValueError('Invalid routing set')
        if any(type(pid) is not int or not 0 < pid <= 0xFFFFFFFF for pid in pids):
            raise ValueError('Invalid PID')
        if any(not math.isfinite(gain) or not 0 <= gain <= 2 for gain in gains):
            raise ValueError('Invalid volume')
        self._request(2, pids, gains)

    def heartbeat(self):
        self._request(3)

    def stats(self):
        return list(self._request(4).stats)

    def shutdown(self):
        self._request(5)

    def selftest(self):
        self._request(6)
        return 0

    def detach(self):
        if self.handle:
            self.dll.TransportClose(self.handle)
            self.handle = None
