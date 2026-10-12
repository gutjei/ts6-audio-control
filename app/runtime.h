#pragma once
#include <QString>
#include "../native/controller_api.h"
QString applicationDirectory();
QString cachedMixer(const QString& dataDirectory);
QString windowsError(DWORD error);
void checked(DWORD error);
bool startupEnabled();
void setStartup(bool enabled);
class ClockProcess {
    HANDLE handle = nullptr;
public:
    DWORD pid = 0;
    ~ClockProcess() { stop(); }
    void start();
    void stop();
    bool alive() const { return handle && WaitForSingleObject(handle, 0) == WAIT_TIMEOUT; }
};
