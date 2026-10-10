"""Event-driven Core Audio inventory. No WMI, elevation or periodic scans."""
import ctypes
from app_runtime import ROOT


class ProcessRow(ctypes.Structure):
    _fields_ = [('pid', ctypes.c_ulong), ('parent', ctypes.c_ulong), ('name', ctypes.c_wchar * 260)]


class AudioRow(ctypes.Structure):
    _fields_ = [('pid', ctypes.c_ulong), ('state', ctypes.c_ulong)]


class AudioInventory:
    def __init__(self):
        self.dll = ctypes.CDLL(str(ROOT / 'build' / 'audio_inventory.dll'))
        signatures = {
            'InventoryCreate': (ctypes.c_void_p, []),
            'InventoryDestroy': (None, [ctypes.c_void_p]),
            'InventoryWait': (ctypes.c_ulong, [ctypes.c_void_p, ctypes.c_ulong]),
            'InventoryWake': (None, [ctypes.c_void_p]),
            'InventoryRefresh': (None, [ctypes.c_void_p]),
            'InventoryRead': (ctypes.c_long, [ctypes.c_void_p, ctypes.POINTER(ProcessRow), ctypes.c_uint,
                ctypes.POINTER(AudioRow), ctypes.c_uint, ctypes.POINTER(ctypes.c_uint),
                ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint64)]),
        }
        for name, (result, args) in signatures.items():
            method = getattr(self.dll, name)
            method.restype, method.argtypes = result, args
        self.handle = self.dll.InventoryCreate()
        if not self.handle:
            raise RuntimeError('Не удалось запустить уведомления Core Audio.')
        self.processes = (ProcessRow * 256)()
        self.audio = (AudioRow * 64)()

    def wait(self, seconds):
        result = self.dll.InventoryWait(self.handle, max(0, int(seconds * 1000)))
        if result == 0xFFFFFFFF:
            raise RuntimeError('Ошибка ожидания событий Core Audio.')
        return result == 0

    def wake(self):
        self.dll.InventoryWake(self.handle)

    def refresh(self):
        self.dll.InventoryRefresh(self.handle)

    def snapshot(self):
        while True:
            pc, ac, revision = ctypes.c_uint(), ctypes.c_uint(), ctypes.c_uint64()
            hr = self.dll.InventoryRead(self.handle, self.processes, len(self.processes), self.audio,
                len(self.audio), ctypes.byref(pc), ctypes.byref(ac), ctypes.byref(revision)) & 0xFFFFFFFF
            if hr == 0x800700EA:  # ERROR_MORE_DATA: retry with a coherent snapshot.
                self.processes = (ProcessRow * max(pc.value, len(self.processes)))()
                self.audio = (AudioRow * max(ac.value, len(self.audio)))()
                continue
            if hr == 0x8000000A:  # Initial enumeration is still pending.
                return None
            if hr:
                raise RuntimeError(f'Ошибка уведомлений Core Audio: 0x{hr:08X}. Перезапусти панель.')
            rows = [{'pid': p.pid, 'ppid': p.parent, 'name': p.name} for p in self.processes[:pc.value]]
            sessions = {s.pid: s.state for s in self.audio[:ac.value]}
            return revision.value, rows, sessions

    def close(self):
        if self.handle:
            self.dll.InventoryDestroy(self.handle)
            self.handle = None
