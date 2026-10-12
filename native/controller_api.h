#pragma once
#include "transport_protocol.h"
struct ProcessRow { DWORD pid, parent; wchar_t name[MAX_PATH]; };
struct AudioRow { DWORD pid, state; };
struct Inventory;
extern "C" {
void* InventoryCreate();
void InventoryDestroy(Inventory*);
DWORD InventoryWait(Inventory*, DWORD);
void InventoryWake(Inventory*);
void InventoryRefresh(Inventory*);
HRESULT InventoryRead(Inventory*, ProcessRow*, unsigned, AudioRow*, unsigned, unsigned*, unsigned*, uint64_t*);
DWORD TransportFindTarget(DWORD*);
DWORD TransportOpen(DWORD, const wchar_t*, DWORD, BOOL, void**);
DWORD TransportRequest(void*, transport::Request*, transport::Response*);
BOOL TransportAlive(void*);
void TransportClose(void*);
}
