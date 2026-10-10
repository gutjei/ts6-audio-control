#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <wrl.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
using namespace Microsoft::WRL;
#define API extern "C" __declspec(dllexport)

struct ProcessRow { DWORD pid, parent; wchar_t name[MAX_PATH]; };
struct AudioRow { DWORD pid, state; };
struct alignas(MEMORY_ALLOCATION_ALIGNMENT) Pending {
    SLIST_ENTRY link{};
    unsigned epoch;
    ComPtr<IAudioSessionControl> control;
    Pending(unsigned e, IAudioSessionControl* c) : epoch(e), control(c) {}
};
struct Signals {
    HANDLE wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    HANDLE changed=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    HANDLE stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::atomic<bool> closing{false}, devices{true};
    std::atomic<HRESULT> error{S_OK};
    alignas(MEMORY_ALLOCATION_ALIGNMENT) SLIST_HEADER pending;
    Signals() { InitializeSListHead(&pending); }
    void notify() { if(!closing) SetEvent(wake); }
    void fail(HRESULT hr) { error=hr; SetEvent(changed); }
    void drain() {
        auto entry=InterlockedFlushSList(&pending);
        while(entry) { auto next=entry->Next; delete reinterpret_cast<Pending*>(entry); entry=next; }
    }
    ~Signals() { drain(); CloseHandle(wake); CloseHandle(changed); CloseHandle(stop); }
};

class SessionEvents final : public RuntimeClass<RuntimeClassFlags<ClassicCom>,IAudioSessionEvents> {
    std::shared_ptr<Signals> signals;
public:
    explicit SessionEvents(std::shared_ptr<Signals> s):signals(std::move(s)) {}
    STDMETHODIMP OnDisplayNameChanged(LPCWSTR,LPCGUID) override { return S_OK; }
    STDMETHODIMP OnIconPathChanged(LPCWSTR,LPCGUID) override { return S_OK; }
    STDMETHODIMP OnSimpleVolumeChanged(float,BOOL,LPCGUID) override { return S_OK; }
    STDMETHODIMP OnChannelVolumeChanged(DWORD,float[],DWORD,LPCGUID) override { return S_OK; }
    STDMETHODIMP OnGroupingParamChanged(LPCGUID,LPCGUID) override { return S_OK; }
    STDMETHODIMP OnStateChanged(AudioSessionState) override { signals->notify(); return S_OK; }
    STDMETHODIMP OnSessionDisconnected(AudioSessionDisconnectReason) override {
        signals->devices=true; signals->notify(); return S_OK;
    }
};
class SessionCreated final : public RuntimeClass<RuntimeClassFlags<ClassicCom>,IAudioSessionNotification> {
    std::shared_ptr<Signals> signals;
    unsigned epoch;
public:
    SessionCreated(std::shared_ptr<Signals> s,unsigned e):signals(std::move(s)),epoch(e) {}
    STDMETHODIMP OnSessionCreated(IAudioSessionControl* control) override {
        if(signals->closing) return S_OK;
        try {
            // AddRef before returning; the session enumerator may never see it.
            auto node=new Pending(epoch,control);
            InterlockedPushEntrySList(&signals->pending,&node->link);
            signals->notify();
        } catch(...) { signals->fail(E_OUTOFMEMORY); }
        return S_OK;
    }
};
class Devices final : public RuntimeClass<RuntimeClassFlags<ClassicCom>,IMMNotificationClient> {
    std::shared_ptr<Signals> signals;
    HRESULT changed() { signals->devices=true; signals->notify(); return S_OK; }
public:
    explicit Devices(std::shared_ptr<Signals> s):signals(std::move(s)) {}
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR,DWORD) override { return changed(); }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return changed(); }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return changed(); }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow,ERole,LPCWSTR) override { return changed(); }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR,const PROPERTYKEY) override { return S_OK; }
};
struct Session {
    ComPtr<IAudioSessionControl2> control;
    ComPtr<SessionEvents> sink;
    HANDLE process=nullptr;
    ~Session() {
        if(control && sink) control->UnregisterAudioSessionNotification(sink.Get());
        if(process) CloseHandle(process);
    }
};
struct Endpoint {
    ComPtr<IAudioSessionManager2> manager;
    ComPtr<SessionCreated> sink;
    ~Endpoint() { if(manager && sink) manager->UnregisterSessionNotification(sink.Get()); }
};
struct ProcessExit {
    HANDLE process=nullptr, wait=nullptr;
    std::shared_ptr<Signals> signals;
    explicit ProcessExit(std::shared_ptr<Signals> s):signals(std::move(s)) {}
    static VOID CALLBACK callback(PVOID self,BOOLEAN) { static_cast<ProcessExit*>(self)->signals->notify(); }
    ~ProcessExit() {
        if(wait) UnregisterWaitEx(wait,INVALID_HANDLE_VALUE);
        if(process) CloseHandle(process);
    }
};
struct Inventory {
    std::shared_ptr<Signals> signals=std::make_shared<Signals>();
    std::thread thread;
    std::mutex mutex;
    std::vector<ProcessRow> processes;
    std::vector<AudioRow> audio;
    uint64_t revision=0;
    // Only the COM worker owns and releases these objects.
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<Devices> deviceSink;
    std::vector<std::unique_ptr<Endpoint>> endpoints;
    std::map<std::wstring,std::unique_ptr<Session>> sessions;
    std::map<DWORD,std::unique_ptr<ProcessExit>> exits;
    unsigned epoch=0;

    void addSession(IAudioSessionControl* control) {
        ComPtr<IAudioSessionControl2> c;
        if(FAILED(control->QueryInterface(IID_PPV_ARGS(&c)))) return;
        DWORD pid=0;
        if(FAILED(c->GetProcessId(&pid)) || !pid) return;
        LPWSTR id=nullptr;
        if(FAILED(c->GetSessionInstanceIdentifier(&id))) return;
        std::wstring key(id); CoTaskMemFree(id);
        if(sessions.count(key)) return;
        auto item=std::make_unique<Session>(); item->control=c;
        item->process=OpenProcess(SYNCHRONIZE,FALSE,pid);
        item->sink=Make<SessionEvents>(signals);
        HRESULT hr=c->RegisterAudioSessionNotification(item->sink.Get());
        if(FAILED(hr)) { signals->fail(hr); return; }
        sessions.emplace(key,std::move(item));
    }
    void rebuildDevices() {
        signals->error=S_OK;
        ++epoch;
        endpoints.clear(); sessions.clear();
        ComPtr<IMMDeviceCollection> collection;
        HRESULT hr=enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&collection);
        if(FAILED(hr)) { signals->fail(hr); return; }
        UINT count=0; collection->GetCount(&count);
        for(UINT i=0;i<count;++i) {
            ComPtr<IMMDevice> device;
            if(FAILED(collection->Item(i,&device))) continue;
            auto endpoint=std::make_unique<Endpoint>();
            hr=device->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,&endpoint->manager);
            if(FAILED(hr)) { signals->fail(hr); continue; }
            endpoint->sink=Make<SessionCreated>(signals,epoch);
            hr=endpoint->manager->RegisterSessionNotification(endpoint->sink.Get());
            if(FAILED(hr)) { signals->fail(hr); continue; }
            ComPtr<IAudioSessionEnumerator> list;
            hr=endpoint->manager->GetSessionEnumerator(&list);
            if(FAILED(hr)) { signals->fail(hr); continue; }
            int n=0;
            hr=list->GetCount(&n); // Arms notifications; register before seeding.
            if(FAILED(hr)) { signals->fail(hr); continue; }
            for(int j=0;j<n;++j) {
                ComPtr<IAudioSessionControl> c;
                if(SUCCEEDED(list->GetSession(j,&c))) addSession(c.Get());
            }
            endpoints.push_back(std::move(endpoint));
        }
    }
    void publish() {
        std::map<DWORD,DWORD> active;
        for(auto i=sessions.begin();i!=sessions.end();) {
            AudioSessionState state=AudioSessionStateExpired;
            DWORD pid=0;
            auto& control=i->second->control;
            if((i->second->process && WaitForSingleObject(i->second->process,0)==WAIT_OBJECT_0) ||
                FAILED(control->GetState(&state)) || state==AudioSessionStateExpired || FAILED(control->GetProcessId(&pid))) {
                i=sessions.erase(i); continue;
            }
            active[pid]=std::max(active[pid],DWORD(state)); ++i;
        }
        // Cheap metadata snapshot only on session/device/lifetime events, never a timer.
        HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
        if(snapshot==INVALID_HANDLE_VALUE) { signals->fail(HRESULT_FROM_WIN32(GetLastError())); return; }
        std::map<DWORD,ProcessRow> all;
        PROCESSENTRY32W item{}; item.dwSize=sizeof(item);
        if(Process32FirstW(snapshot,&item)) do {
            ProcessRow row{}; row.pid=item.th32ProcessID; row.parent=item.th32ParentProcessID;
            wcsncpy_s(row.name,item.szExeFile,_TRUNCATE); all.emplace(row.pid,row);
        } while(Process32NextW(snapshot,&item));
        CloseHandle(snapshot);
        // Retain metadata only for audio processes and their ancestors. Register
        // one-shot exit notifications, including for a browser's silent parent.
        std::set<DWORD> relevant;
        for(auto [pid,state]:active) {
            std::set<DWORD> seen;
            DWORD ancestor=pid;
            while(ancestor && all.count(ancestor) && seen.insert(ancestor).second) {
                relevant.insert(ancestor); ancestor=all.at(ancestor).parent;
            }
        }
        for(auto i=exits.begin();i!=exits.end();) {
            if(!relevant.count(i->first) || WaitForSingleObject(i->second->process,0)==WAIT_OBJECT_0) i=exits.erase(i);
            else ++i;
        }
        std::vector<ProcessRow> rows;
        for(DWORD pid:relevant) {
            rows.push_back(all.at(pid));
            if(exits.count(pid)) continue;
            auto watch=std::make_unique<ProcessExit>(signals);
            watch->process=OpenProcess(SYNCHRONIZE,FALSE,pid);
            if(watch->process && WaitForSingleObject(watch->process,0)==WAIT_TIMEOUT &&
                RegisterWaitForSingleObject(&watch->wait,watch->process,ProcessExit::callback,watch.get(),INFINITE,WT_EXECUTEONLYONCE))
                exits.emplace(pid,std::move(watch));
        }
        std::vector<AudioRow> output;
        for(auto [pid,state]:active) if(all.count(pid)) output.push_back({pid,state});
        std::lock_guard<std::mutex> lock(mutex);
        bool same=rows.size()==processes.size() && output.size()==audio.size();
        if(same) {
            for(size_t i=0;i<rows.size();++i) if(rows[i].pid!=processes[i].pid || rows[i].parent!=processes[i].parent || wcscmp(rows[i].name,processes[i].name)) { same=false; break; }
            for(size_t i=0;i<output.size();++i) if(output[i].pid!=audio[i].pid || output[i].state!=audio[i].state) { same=false; break; }
        }
        if(!revision || !same) { processes=std::move(rows); audio=std::move(output); ++revision; SetEvent(signals->changed); }
    }
    void run() {
        HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        if(FAILED(hr)) { signals->fail(hr); return; }
        try {
            hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator));
            if(FAILED(hr)) { signals->fail(hr); }
            else {
                deviceSink=Make<Devices>(signals);
                hr=enumerator->RegisterEndpointNotificationCallback(deviceSink.Get());
                if(FAILED(hr)) signals->fail(hr);
                else {
                    HANDLE handles[]={signals->stop,signals->wake};
                    signals->notify();
                    while(WaitForMultipleObjects(2,handles,FALSE,INFINITE)==WAIT_OBJECT_0+1) {
                        if(signals->devices.exchange(false)) rebuildDevices();
                        auto entry=InterlockedFlushSList(&signals->pending);
                        while(entry) {
                            auto next=entry->Next;
                            std::unique_ptr<Pending> node(reinterpret_cast<Pending*>(entry));
                            if(node->epoch==epoch) addSession(node->control.Get());
                            entry=next;
                        }
                        publish();
                    }
                }
            }
        } catch(...) { signals->fail(E_FAIL); }
        signals->closing=true;
        if(enumerator && deviceSink) enumerator->UnregisterEndpointNotificationCallback(deviceSink.Get());
        exits.clear(); endpoints.clear(); sessions.clear();
        deviceSink.Reset(); enumerator.Reset(); signals->drain();
        CoUninitialize();
    }
    ~Inventory() { signals->closing=true; SetEvent(signals->stop); if(thread.joinable()) thread.join(); }
};

API void* InventoryCreate() {
    try {
        auto inventory=std::make_unique<Inventory>();
        if(!inventory->signals->wake || !inventory->signals->changed || !inventory->signals->stop) return nullptr;
        inventory->thread=std::thread([p=inventory.get()] { p->run(); });
        return inventory.release();
    } catch(...) { return nullptr; }
}
API void InventoryDestroy(Inventory* inventory) { delete inventory; }
API DWORD InventoryWait(Inventory* inventory,DWORD timeout) { return WaitForSingleObject(inventory->signals->changed,timeout); }
API void InventoryWake(Inventory* inventory) { SetEvent(inventory->signals->changed); }
API void InventoryRefresh(Inventory* inventory) { inventory->signals->devices=true; inventory->signals->notify(); }
API HRESULT InventoryRead(Inventory* inventory,ProcessRow* processes,unsigned processCapacity,AudioRow* audio,unsigned audioCapacity,unsigned* processCount,unsigned* audioCount,uint64_t* revision) {
    std::lock_guard<std::mutex> lock(inventory->mutex);
    *processCount=unsigned(inventory->processes.size()); *audioCount=unsigned(inventory->audio.size()); *revision=inventory->revision;
    HRESULT hr=inventory->signals->error;
    if(FAILED(hr)) return hr;
    if(processCapacity<*processCount || audioCapacity<*audioCount) return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
    std::copy(inventory->processes.begin(),inventory->processes.end(),processes);
    std::copy(inventory->audio.begin(),inventory->audio.end(),audio);
    return *revision ? S_OK : E_PENDING;
}
