// One native interception point; COM proxies own all subsequent audio lifetimes.
// No work runs from DllMain, no executable buffers are handwritten, and PCM never
// crosses IPC. The module stays resident while TeamSpeak can hold proxy objects.
#include "transport_protocol.h"
#include "mixer_api.h"
#include "vendor/minhook/include/MinHook.h"
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl.h>
#include <sddl.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>
using namespace Microsoft::WRL;
using namespace transport;
namespace agent {
static std::atomic<bool> enabled{false};
static std::atomic<DWORD> clockPid{0};
static HMODULE targetModule = nullptr;
using ActivateFn = decltype(&ActivateAudioInterfaceAsync);
static ActivateFn originalActivate = nullptr;
static std::mutex startMutex;
static HANDLE serverPipe = INVALID_HANDLE_VALUE;

class Capture final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IAudioCaptureClient> {
public:
    ComPtr<IAudioCaptureClient> inner;
    explicit Capture(IAudioCaptureClient* capture) : inner(capture) {}
    ~Capture() { RouterRemove(this); }
    STDMETHODIMP GetBuffer(BYTE** data, UINT32* frames, DWORD* flags, UINT64* position, UINT64* qpc) override {
        return RouterGetBuffer(this, data, frames, flags, position, qpc);
    }
    STDMETHODIMP ReleaseBuffer(UINT32 frames) override { return inner->ReleaseBuffer(frames); }
    STDMETHODIMP GetNextPacketSize(UINT32* frames) override { return inner->GetNextPacketSize(frames); }
    static HRESULT STDMETHODCALLTYPE original(IAudioCaptureClient* self, BYTE** data, UINT32* frames,
                                               DWORD* flags, UINT64* position, UINT64* qpc) {
        return static_cast<Capture*>(self)->inner->GetBuffer(data, frames, flags, position, qpc);
    }
};

class Client final : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
    ChainInterfaces<IAudioClient3, IAudioClient2, IAudioClient>> {
    ComPtr<IAudioClient> inner;
    ComPtr<IAudioClient2> second;
    ComPtr<IAudioClient3> third;
    ComPtr<Capture> capture;
    std::mutex mutex;
    std::vector<BYTE> format;
public:
    explicit Client(IAudioClient* audio) : inner(audio) { inner.As(&second); inner.As(&third); }
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if(!out) return E_POINTER;
        *out = nullptr;
        if((iid == __uuidof(IAudioClient2) && !second) || (iid == __uuidof(IAudioClient3) && !third)) return E_NOINTERFACE;
        return RuntimeClass::QueryInterface(iid, out);
    }
    STDMETHODIMP Initialize(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME duration,
                            REFERENCE_TIME periodicity, const WAVEFORMATEX* f, LPCGUID session) override {
        if(!f || f->cbSize > 256) return E_INVALIDARG;
        try {
            std::lock_guard<std::mutex> lock(mutex);
            std::vector<BYTE> copy(reinterpret_cast<const BYTE*>(f), reinterpret_cast<const BYTE*>(f) + sizeof(*f) + f->cbSize);
            HRESULT hr = inner->Initialize(mode, flags, duration, periodicity, f, session);
            if(SUCCEEDED(hr)) format = std::move(copy);
            return hr;
        } catch(...) { return E_OUTOFMEMORY; }
    }
    STDMETHODIMP GetBufferSize(UINT32* n) override { return inner->GetBufferSize(n); }
    STDMETHODIMP GetStreamLatency(REFERENCE_TIME* t) override { return inner->GetStreamLatency(t); }
    STDMETHODIMP GetCurrentPadding(UINT32* n) override { return inner->GetCurrentPadding(n); }
    STDMETHODIMP IsFormatSupported(AUDCLNT_SHAREMODE m, const WAVEFORMATEX* f, WAVEFORMATEX** closest) override { return inner->IsFormatSupported(m, f, closest); }
    STDMETHODIMP GetMixFormat(WAVEFORMATEX** f) override { return inner->GetMixFormat(f); }
    STDMETHODIMP GetDevicePeriod(REFERENCE_TIME* a, REFERENCE_TIME* b) override { return inner->GetDevicePeriod(a, b); }
    STDMETHODIMP Start() override { return inner->Start(); }
    STDMETHODIMP Stop() override { return inner->Stop(); }
    STDMETHODIMP Reset() override { return inner->Reset(); }
    STDMETHODIMP SetEventHandle(HANDLE e) override { return inner->SetEventHandle(e); }
    STDMETHODIMP GetService(REFIID iid, void** out) override {
        if(!out) return E_POINTER;
        *out = nullptr;
        if(iid != __uuidof(IAudioCaptureClient)) return inner->GetService(iid, out);
        try {
            std::lock_guard<std::mutex> lock(mutex);
            if(!capture) {
                if(format.empty()) return AUDCLNT_E_NOT_INITIALIZED;
                ComPtr<IAudioCaptureClient> real;
                HRESULT hr = inner->GetService(IID_PPV_ARGS(&real));
                if(FAILED(hr)) return hr;
                auto proxy = Make<Capture>(real.Get());
                if(!proxy) return E_OUTOFMEMORY;
                hr = static_cast<HRESULT>(RouterRegister(proxy.Get(), reinterpret_cast<WAVEFORMATEX*>(format.data())));
                if(FAILED(hr)) return hr; // Never expose an unfiltered capture on failure.
                capture = proxy;
            }
            return capture.CopyTo(iid, out);
        } catch(...) { return E_OUTOFMEMORY; }
    }
    STDMETHODIMP IsOffloadCapable(AUDIO_STREAM_CATEGORY c, BOOL* b) override { return second ? second->IsOffloadCapable(c, b) : E_NOINTERFACE; }
    STDMETHODIMP SetClientProperties(const AudioClientProperties* p) override { return second ? second->SetClientProperties(p) : E_NOINTERFACE; }
    STDMETHODIMP GetBufferSizeLimits(const WAVEFORMATEX* f, BOOL e, REFERENCE_TIME* a, REFERENCE_TIME* b) override { return second ? second->GetBufferSizeLimits(f, e, a, b) : E_NOINTERFACE; }
    STDMETHODIMP GetSharedModeEnginePeriod(const WAVEFORMATEX* f, UINT32* a, UINT32* b, UINT32* c, UINT32* d) override { return third ? third->GetSharedModeEnginePeriod(f, a, b, c, d) : E_NOINTERFACE; }
    STDMETHODIMP GetCurrentSharedModeEnginePeriod(WAVEFORMATEX** f, UINT32* n) override { return third ? third->GetCurrentSharedModeEnginePeriod(f, n) : E_NOINTERFACE; }
    STDMETHODIMP InitializeSharedAudioStream(DWORD flags, UINT32 period, const WAVEFORMATEX* f, LPCGUID session) override {
        if(!third) return E_NOINTERFACE;
        if(!f || f->cbSize > 256) return E_INVALIDARG;
        try {
            std::lock_guard<std::mutex> lock(mutex);
            std::vector<BYTE> copy(reinterpret_cast<const BYTE*>(f), reinterpret_cast<const BYTE*>(f) + sizeof(*f) + f->cbSize);
            HRESULT hr = third->InitializeSharedAudioStream(flags, period, f, session);
            if(SUCCEEDED(hr)) format = std::move(copy);
            return hr;
        } catch(...) { return E_OUTOFMEMORY; }
    }
};

struct Activation {
    std::mutex mutex;
    ComPtr<Client> client;
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    PROPVARIANT variant{};
    explicit Activation(DWORD pid) {
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = pid;
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        variant.vt = VT_BLOB; variant.blob.cbSize = sizeof(params);
        variant.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
    }
};
class Operation final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IActivateAudioInterfaceAsyncOperation> {
    ComPtr<IActivateAudioInterfaceAsyncOperation> inner;
    std::mutex mutex;
    std::shared_ptr<Activation> state;
public:
    explicit Operation(std::shared_ptr<Activation> s) : state(std::move(s)) {}
    void bind(IActivateAudioInterfaceAsyncOperation* op) {
        std::lock_guard<std::mutex> lock(mutex);
        if(!inner) inner = op;
    }
    STDMETHODIMP GetActivateResult(HRESULT* result, IUnknown** out) override {
        if(!result || !out) return E_POINTER;
        *out = nullptr;
        try {
            ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
            { std::lock_guard<std::mutex> lock(mutex); operation = inner; }
            if(!operation) { *result = E_PENDING; return E_PENDING; }
            ComPtr<IUnknown> real;
            HRESULT hr = operation->GetActivateResult(result, &real);
            if(FAILED(hr) || FAILED(*result)) return hr;
            std::lock_guard<std::mutex> lock(state->mutex);
            if(!state->client) {
                ComPtr<IAudioClient> audio;
                hr = real.As(&audio);
                if(FAILED(hr)) { *result = hr; return S_OK; }
                state->client = Make<Client>(audio.Get());
                if(!state->client) { *result = E_OUTOFMEMORY; return S_OK; }
            }
            return state->client.CopyTo(out);
        } catch(...) { *result = E_OUTOFMEMORY; return S_OK; }
    }
};
class Completion final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase, IActivateAudioInterfaceCompletionHandler> {
    ComPtr<IActivateAudioInterfaceCompletionHandler> inner;
    ComPtr<Operation> operation;
public:
    Completion(IActivateAudioInterfaceCompletionHandler* callback, Operation* op) : inner(callback), operation(op) {}
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        // Return the same COM identity through both the callback and out-param.
        // Drop this reference after the one-shot callback to break the real
        // operation -> completion -> proxy -> real operation reference cycle.
        auto proxy = std::move(operation);
        if(!proxy) return E_UNEXPECTED;
        try {
            proxy->bind(op);
            return inner->ActivateCompleted(proxy.Get());
        } catch(...) { return E_OUTOFMEMORY; }
    }
};
static HRESULT WINAPI activate(LPCWSTR device, REFIID iid, PROPVARIANT* params,
                              IActivateAudioInterfaceCompletionHandler* callback, IActivateAudioInterfaceAsyncOperation** out) {
    HMODULE caller = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(_ReturnAddress()), &caller);
    if(!enabled.load() || caller != targetModule || !device || wcscmp(device, VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK) != 0)
        return originalActivate(device, iid, params, callback, out);
    if(!callback || !out || iid != __uuidof(IAudioClient)) return E_INVALIDARG;
    *out = nullptr;
    try {
        auto state = std::make_shared<Activation>(clockPid.load());
        auto proxy = Make<Operation>(state);
        if(!proxy) return E_OUTOFMEMORY;
        auto completion = Make<Completion>(callback, proxy.Get());
        if(!completion) return E_OUTOFMEMORY;
        ComPtr<IActivateAudioInterfaceAsyncOperation> real;
        HRESULT hr = originalActivate(device, iid, &state->variant, completion.Get(), &real);
        if(FAILED(hr)) return hr;
        proxy->bind(real.Get());
        return proxy.CopyTo(out);
    } catch(...) { return E_OUTOFMEMORY; }
}

static DWORD install(bool diagnostic) {
    if(originalActivate) return ERROR_SUCCESS;
    targetModule = GetModuleHandleW(diagnostic ? nullptr : L"TeamSpeak.dll");
    if(!targetModule) return ERROR_MOD_NOT_FOUND;
    HMODULE audio = LoadLibraryExW(L"mmdevapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!audio) return GetLastError();
    auto address = GetProcAddress(audio, "ActivateAudioInterfaceAsync");
    if(!address) return ERROR_PROC_NOT_FOUND;
    auto initialized = MH_Initialize();
    if(initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return ERROR_INVALID_FUNCTION;
    RouterSetOriginal(reinterpret_cast<void*>(&Capture::original));
    if(MH_CreateHook(reinterpret_cast<void*>(address), reinterpret_cast<void*>(&activate), reinterpret_cast<void**>(&originalActivate)) != MH_OK)
        return ERROR_INVALID_FUNCTION;
    if(MH_EnableHook(reinterpret_cast<void*>(address)) != MH_OK) {
        MH_RemoveHook(reinterpret_cast<void*>(address)); originalActivate = nullptr;
        return ERROR_INVALID_FUNCTION;
    }
    return ERROR_SUCCESS;
}
static DWORD WINAPI serve(void*) {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for(;;) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        OVERLAPPED ov{}; ov.hEvent = event;
        BOOL connected = ConnectNamedPipe(serverPipe, &ov);
        DWORD error = connected ? ERROR_SUCCESS : GetLastError();
        if(error == ERROR_IO_PENDING) {
            DWORD ignored = 0;
            connected = GetOverlappedResult(serverPipe, &ov, &ignored, TRUE);
        } else connected = connected || error == ERROR_PIPE_CONNECTED;
        bool greeted = false;
        Handle clock;
        if(connected) for(;;) {
            Request request;
            if(transfer(serverPipe, &request, sizeof(request), false, 3500)) break;
            Response response;
            bool closing = false;
            if(request.magic != magic || request.version != version || request.count > 64)
                response.error = ERROR_INVALID_DATA;
            else if(request.command == Hello && !greeted) {
                clock.value = OpenProcess(SYNCHRONIZE, FALSE, request.silentPid);
                if(!clock.value || WaitForSingleObject(clock, 0) != WAIT_TIMEOUT || request.silentPid == GetCurrentProcessId())
                    response.error = ERROR_INVALID_PARAMETER;
                else {
                    response.error = FAILED(com) ? ERROR_INVALID_FUNCTION : install(request.diagnostic != 0);
                    if(!response.error) {
                        RouterSilence(); clockPid = request.silentPid;
                        greeted = true; enabled = true;
                    }
                }
            } else if(!greeted) response.error = ERROR_INVALID_STATE;
            else if(WaitForSingleObject(clock, 0) != WAIT_TIMEOUT) response.error = ERROR_PROCESS_ABORTED;
            else switch(request.command) {
                case Configure:
                    for(unsigned i = 0; i < request.count; ++i)
                        if(!request.pids[i] || !std::isfinite(request.gains[i]) || request.gains[i] < 0 || request.gains[i] > 2)
                            response.error = ERROR_INVALID_PARAMETER;
                    if(!response.error) RouterConfigureVolumes(request.pids, request.gains, request.count);
                    break;
                case Heartbeat: RouterHeartbeat(); break;
                case Stats: RouterStats(response.stats); break;
                case SelfTest: response.error = RouterSelfTest() ? ERROR_INVALID_DATA : ERROR_SUCCESS; break;
                case Shutdown: enabled = false; RouterSilence(); closing = true; break;
                default: response.error = ERROR_INVALID_FUNCTION;
            }
            if(response.error) { enabled = false; RouterSilence(); }
            if(transfer(serverPipe, &response, sizeof(response), true) || closing || response.error) break;
        }
        enabled = false; RouterSilence();
        DisconnectNamedPipe(serverPipe);
    }
    if(SUCCEEDED(com)) CoUninitialize();
    return 0;
}
extern "C" __declspec(dllexport) DWORD WINAPI RouterAgentStart(void*) {
    try {
        std::lock_guard<std::mutex> lock(startMutex);
        if(serverPipe != INVALID_HANDLE_VALUE) return ERROR_SUCCESS;
        // Same user only, local clients only, and one agent/controller per target.
        Handle token;
        if(!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return GetLastError();
        DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<BYTE> bytes(size);
        if(!GetTokenInformation(token, TokenUser, bytes.data(), size, &size)) return GetLastError();
        LPWSTR sid = nullptr;
        if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid)) return GetLastError();
        std::wstring acl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")"; LocalFree(sid);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return GetLastError();
        SECURITY_ATTRIBUTES sa{sizeof(sa), descriptor, FALSE};
        auto name = pipeName(GetCurrentProcessId());
        serverPipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, &sa);
        DWORD error = serverPipe == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        LocalFree(descriptor);
        if(error) return error;
        HMODULE pinned = nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&RouterAgentStart), &pinned)) error = GetLastError();
        Handle thread(error ? nullptr : CreateThread(nullptr, 0, serve, nullptr, 0, nullptr));
        if(!thread.value && !error) error = GetLastError();
        if(error) { CloseHandle(serverPipe); serverPipe = INVALID_HANDLE_VALUE; }
        return error;
    } catch(...) { return ERROR_OUTOFMEMORY; }
}
} // namespace agent
