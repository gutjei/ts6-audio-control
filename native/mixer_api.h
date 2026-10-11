#pragma once
#include <windows.h>
#include <audioclient.h>
extern "C" {
void RouterConfigureVolumes(const DWORD*, const float*, unsigned);
void RouterHeartbeat();
void RouterSetOriginal(void*);
int RouterRegister(void*, const WAVEFORMATEX*);
void RouterRemove(void*);
HRESULT STDMETHODCALLTYPE RouterGetBuffer(IAudioCaptureClient*, BYTE**, UINT32*, DWORD*, UINT64*, UINT64*);
void RouterStats(unsigned long long*);
void RouterSilence();
int RouterSelfTest();
}
