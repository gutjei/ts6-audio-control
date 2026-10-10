#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <ksmedia.h>
#include <wrl.h>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace Microsoft::WRL;
#define API extern "C" __declspec(dllexport)

using GetBufferFn = HRESULT (STDMETHODCALLTYPE *)(IAudioCaptureClient*, BYTE**, UINT32*, DWORD*, UINT64*, UINT64*);
static GetBufferFn originalGetBuffer = nullptr;
using Policy = std::map<DWORD,float>;
static std::shared_ptr<const Policy> permitted = std::make_shared<const Policy>();
static void wakeContexts();
static std::atomic<ULONGLONG> heartbeat{0};
static std::atomic<unsigned long long> packets{0}, audiblePackets{0}, failures{0};
static std::atomic<unsigned long long> sourcePackets{0}, sourceLoud{0}, sourceQpc{0}, sinkQpc{0}, sourceFlags{0}, sinkFlags{0}, writtenFrame{0}, readFrame{0};
static std::atomic<unsigned long long> outputEnergy{0}, outputSamples{0};
static std::atomic<unsigned long> lastError{0}, sourceCount{0};
static std::atomic<bool> terminating{false};
static std::atomic<unsigned long long> workerWakeups{0}, droppedFrames{0};
static std::atomic<unsigned long long> captureWakeups{0}, renderedSourceFrames{0}, missingSourceFrames{0};
static constexpr unsigned MIX_DELAY_MS=30;

static std::shared_ptr<const Policy> policy() {
    if (terminating || GetTickCount64() - heartbeat.load() > 3000) return {};
    return std::atomic_load(&permitted);
}
static int64_t frameAt(UINT64 qpc, unsigned rate) {
    // QPC from WASAPI is already expressed in 100 ns units.
    return int64_t(qpc / 10000000) * rate + int64_t((qpc % 10000000) * rate / 10000000);
}
struct Format {
    unsigned rate = 0, channels = 0, align = 0;
    bool floating = false, supported = false;
    std::vector<BYTE> bytes;
    explicit Format(const WAVEFORMATEX* f) {
        if (!f || f->cbSize > 256) return;
        bytes.assign(reinterpret_cast<const BYTE*>(f), reinterpret_cast<const BYTE*>(f) + sizeof(*f) + f->cbSize);
        rate = f->nSamplesPerSec; channels = f->nChannels; align = f->nBlockAlign;
        WORD tag = f->wFormatTag;
        if (tag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
            auto x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
            if (x->SubFormat == KSDATAFORMAT_SUBTYPE_PCM && x->Samples.wValidBitsPerSample == 16) tag = WAVE_FORMAT_PCM;
            if (x->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
        }
        floating = tag == WAVE_FORMAT_IEEE_FLOAT && f->wBitsPerSample == 32;
        supported = rate >= 8000 && rate <= 192000 && channels >= 1 && channels <= 8 &&
            (floating || (tag == WAVE_FORMAT_PCM && f->wBitsPerSample == 16)) &&
            align == channels * (floating ? 4u : 2u);
    }
    WAVEFORMATEX* wave() { return reinterpret_cast<WAVEFORMATEX*>(bytes.data()); }
};

// SPSC queue: the capture worker owns writeIndex; GetBuffer owns readIndex.
// Release/acquire publication prevents overwriting or reading unfinished frames.
// On overflow discard incoming audio rather than blocking the render thread.
#pragma warning(push)
#pragma warning(disable:4324) // Intentional cache-line separation of SPSC indices.
struct Ring {
    unsigned channels, capacity;
    std::vector<float> samples;
    std::vector<int64_t> tags;
    alignas(64) std::atomic<uint64_t> readIndex{0};
    alignas(64) std::atomic<uint64_t> writeIndex{0};
    Ring(unsigned rate, unsigned ch) : channels(ch), capacity(rate * 2), samples(size_t(capacity)*ch), tags(capacity, -1) {}
    bool putBlock(int64_t frame, unsigned count, const float* values) {
        if(frame<0 || !count) return false;
        const auto write=writeIndex.load(std::memory_order_relaxed);
        if(count>capacity || write-readIndex.load(std::memory_order_acquire)>capacity-count) {
            droppedFrames+=count; return false;
        }
        // One publication per WASAPI packet, not one atomic operation per frame.
        const auto slot=size_t(write%capacity);
        const auto head=std::min<size_t>(count,capacity-slot);
        std::copy_n(values,head*channels,samples.data()+slot*channels);
        std::copy_n(values+head*channels,(count-head)*channels,samples.data());
        for(unsigned i=0;i<count;++i) tags[(slot+i)%capacity]=frame+i;
        writeIndex.store(write+count,std::memory_order_release);
        return true;
    }
    bool put(int64_t frame,const float* values) { return putBlock(frame,1,values); }
    unsigned add(int64_t first, unsigned frames, std::vector<float>& out, float gain=1.f) {
        auto read=readIndex.load(std::memory_order_relaxed);
        const auto end=writeIndex.load(std::memory_order_acquire);
        const auto last=first+frames;
        unsigned covered=0;
        while(read<end) {
            const auto slot=size_t(read%capacity);
            const auto frame=tags[slot];
            if(frame>=last) break;
            if(frame>=first && frame>=0) {
                ++covered;
                const auto offset=size_t(frame-first)*channels;
                for(unsigned c=0;c<channels;++c) out[offset+c]+=samples[slot*channels+c]*gain;
            }
            ++read;
        }
        readIndex.store(read,std::memory_order_release);
        return covered;
    }
};

#pragma warning(pop)

class Completion final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_PENDING;
    ComPtr<IAudioClient> client;
    ~Completion() { if(event) CloseHandle(event); }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        ComPtr<IUnknown> unk; HRESULT activation = E_FAIL;
        result = op->GetActivateResult(&activation, &unk);
        if (SUCCEEDED(result)) result=activation;
        if (SUCCEEDED(result)) result=unk.As(&client);
        SetEvent(event); return S_OK;
    }
};

struct Source {
    DWORD pid;
    HANDLE process = nullptr;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    std::shared_ptr<Ring> ring;
    HANDLE ready=nullptr, processWait=nullptr;
    bool ownsReady=false;
    Format format;
    std::vector<float> scratch;
    HANDLE cancel=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::thread worker;
    std::atomic<bool> done{false};
    HRESULT result=S_OK;
    void requestStop() { if(cancel) SetEvent(cancel); }
    void closeClient() {
        if(client) client->Stop();
        capture.Reset(); client.Reset();
    }
    Source(DWORD id, const Format& f) : pid(id), ring(std::make_shared<Ring>(f.rate, f.channels)), format(f), scratch(size_t(f.rate)*f.channels) {
        if(!cancel) throw std::bad_alloc();
    }
    ~Source() {
        requestStop(); if(worker.joinable()) worker.join();
        closeClient();
        if(cancel) CloseHandle(cancel);
        if(processWait) UnregisterWaitEx(processWait,INVALID_HANDLE_VALUE);
        if(process) CloseHandle(process);
        if(ownsReady && ready) CloseHandle(ready);
    }
    static VOID CALLBACK exited(PVOID event,BOOLEAN) { SetEvent(static_cast<HANDLE>(event)); }
    HRESULT start(Format& fmt,HANDLE sharedReady=nullptr,HANDLE stopEvent=nullptr) {
        ready=sharedReady;
        if(!ready) { ready=CreateEventW(nullptr,FALSE,FALSE,nullptr); ownsReady=true; }
        if(!ready) return HRESULT_FROM_WIN32(GetLastError());
        process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if(!process) return HRESULT_FROM_WIN32(GetLastError());
        AUDIOCLIENT_ACTIVATION_PARAMS p{};
        p.ActivationType=AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        p.ProcessLoopbackParams.TargetProcessId=pid;
        p.ProcessLoopbackParams.ProcessLoopbackMode=PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT v{}; v.vt=VT_BLOB; v.blob.cbSize=sizeof(p); v.blob.pBlobData=reinterpret_cast<BYTE*>(&p);
        auto cb=Make<Completion>(); if (!cb || !cb->event) return E_OUTOFMEMORY;
        ComPtr<IActivateAudioInterfaceAsyncOperation> op;
        HRESULT hr=ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &v, cb.Get(), &op);
        if(FAILED(hr)) return hr;
        HANDLE waits[]={cb->event,stopEvent};
        DWORD wait=WaitForMultipleObjects(stopEvent?2:1,waits,FALSE,2000);
        if(wait!=WAIT_OBJECT_0) return HRESULT_FROM_WIN32(wait==WAIT_OBJECT_0+1?ERROR_CANCELLED:(wait==WAIT_FAILED?GetLastError():WAIT_TIMEOUT));
        if(FAILED(cb->result)) return cb->result;
        client=cb->client;
        hr=client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK|AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            0, 0, fmt.wave(), nullptr);
        if(FAILED(hr)) return hr;
        hr=client->GetService(IID_PPV_ARGS(&capture)); if(FAILED(hr)) return hr;
        hr=client->SetEventHandle(ready); if(FAILED(hr)) return hr;
        if(!RegisterWaitForSingleObject(&processWait,process,exited,ready,INFINITE,WT_EXECUTEONLYONCE))
            return HRESULT_FROM_WIN32(GetLastError());
        return client->Start();
    }
    HRESULT drain() {
        for(unsigned packet=0;packet<64;++packet) {
            if(WaitForSingleObject(cancel,0)==WAIT_OBJECT_0) return S_OK;
            UINT32 n=0; DWORD flags=0; UINT64 pos=0,qpc=0; BYTE* data=nullptr;
            HRESULT hr=capture->GetBuffer(&data,&n,&flags,&pos,&qpc);
            if(hr==AUDCLNT_S_BUFFER_EMPTY || (SUCCEEDED(hr)&&!n)) return S_OK;
            if(FAILED(hr)) return hr;
            ++sourcePackets; sourceQpc=qpc; sourceFlags=flags;
            bool loud=false;
            if(!(flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && n<=format.rate) {
                const auto anchor=frameAt(qpc,format.rate); writtenFrame=anchor;
                const auto count=size_t(n)*format.channels;
                for(size_t k=0;k<count;++k) {
                    float value=0;
                    if(!(flags&AUDCLNT_BUFFERFLAGS_SILENT) && data)
                        value=format.floating?reinterpret_cast<float*>(data)[k]:reinterpret_cast<int16_t*>(data)[k]/32768.f;
                    if(!std::isfinite(value)) value=0;
                    scratch[k]=value; loud|=std::abs(value)>.00003f;
                }
                ring->putBlock(anchor,n,scratch.data());
            } else droppedFrames+=n;
            if(loud) ++sourceLoud;
            hr=capture->ReleaseBuffer(n); if(FAILED(hr)) return hr;
        }
        SetEvent(ready); // Continue a backlog without a polling timer.
        return S_OK;
    }
    void run(HANDLE managerWake) {
        worker=std::thread([this,managerWake] {
            const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            bool counted=false;
            DWORD task=0; HANDLE mmcss=nullptr;
            result=com;
            if(SUCCEEDED(com)) {
                try {
                    result=start(format,nullptr,cancel);
                    if(SUCCEEDED(result)) {
                        ++sourceCount; counted=true;
                        mmcss=AvSetMmThreadCharacteristicsW(L"Audio",&task);
                        HANDLE handles[]={cancel,ready};
                        while(true) {
                            DWORD wait=WaitForMultipleObjects(2,handles,FALSE,INFINITE);
                            if(wait==WAIT_OBJECT_0) break;
                            if(wait==WAIT_FAILED) { result=HRESULT_FROM_WIN32(GetLastError()); break; }
                            ++captureWakeups;
                            if(WaitForSingleObject(process,0)==WAIT_OBJECT_0) break;
                            result=drain(); if(FAILED(result)) break;
                        }
                    }
                } catch(...) { result=E_OUTOFMEMORY; }
                closeClient();
                if(mmcss) AvRevertMmThreadCharacteristics(mmcss);
                CoUninitialize();
            }
            if(counted) --sourceCount;
            if(FAILED(result) && result!=HRESULT_FROM_WIN32(ERROR_CANCELLED)) { lastError=result; ++failures; }
            done.store(true,std::memory_order_release);
            SetEvent(managerWake);
        });
    }

};

struct RenderSource { DWORD pid; std::shared_ptr<Ring> ring; };
using RenderSources = std::vector<RenderSource>;
struct Context {
    Format fmt;
    std::atomic<bool> stop{false};
    // Manager owns Source lifetimes; each Source owns an event-driven capture
    // thread. Render snapshots own queues only, never COM clients or threads.
    std::map<DWORD,std::shared_ptr<Source>> sources;
    std::shared_ptr<const RenderSources> renderSources=std::make_shared<const RenderSources>();
    HANDLE wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    HANDLE cancel=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::vector<BYTE> output;
    std::vector<float> mix;
    std::thread worker;
    explicit Context(const WAVEFORMATEX* f) : fmt(f) {
        try {
            if(!wake || !cancel) throw std::bad_alloc();
            if(fmt.supported) {
                output.reserve(size_t(fmt.rate)*fmt.align);
                mix.reserve(size_t(fmt.rate)*fmt.channels);
            }
        } catch(...) {
            if(wake) CloseHandle(wake);
            if(cancel) CloseHandle(cancel);
            throw;
        }
    }
    void requestStop() { stop=true; SetEvent(cancel); SetEvent(wake); }
    ~Context() {
        requestStop(); if(worker.joinable()) worker.join();
        sources.clear(); CloseHandle(wake); CloseHandle(cancel);
    }
    void publishSources() {
        auto list=std::make_shared<RenderSources>(); list->reserve(sources.size());
        for(auto& [id,s]:sources) list->push_back({id,s->ring});
        std::atomic_store(&renderSources,std::shared_ptr<const RenderSources>(list));
    }
    void run() {
        if(!fmt.supported) { lastError=static_cast<unsigned long>(AUDCLNT_E_UNSUPPORTED_FORMAT); ++failures; return; }
        worker=std::thread([this] {
            HRESULT com=CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if(FAILED(com)) { lastError=com; ++failures; return; }
            try {
                std::map<DWORD, ULONGLONG> retry;
                HANDLE handles[]={cancel,wake};
                while(!stop && !terminating) {
                    auto wanted=policy();
                    bool changed=false;
                    for(auto& [id,s]:sources)
                        if(!wanted || !wanted->count(id)) s->requestStop();
                    for(auto i=sources.begin();i!=sources.end();) {
                        if(!wanted || !wanted->count(i->first) || i->second->done.load(std::memory_order_acquire)) {
                            retry[i->first]=GetTickCount64()+3000;
                            i=sources.erase(i); changed=true;
                        } else ++i;
                    }
                    for(auto i=retry.begin();i!=retry.end();) {
                        if(!wanted || !wanted->count(i->first)) i=retry.erase(i); else ++i;
                    }
                    if(wanted) for(auto [id,gain]:*wanted) {
                        if(stop || terminating) break;
                        if(sources.count(id)) continue;
                        const auto now=GetTickCount64(); if(retry[id]>now) continue;
                        retry[id]=now+3000;
                        auto s=std::make_shared<Source>(id,fmt);
                        s->run(wake);
                        sources[id]=s; changed=true; retry.erase(id);
                    }
                    if(changed) publishSources();
                    // Deadlines only: watchdog expiry or retrying a failed activation.
                    // With no sources/policy the worker sleeps until explicitly woken.
                    DWORD timeout=INFINITE;
                    const auto now=GetTickCount64(), beat=heartbeat.load();
                    if(wanted && !wanted->empty() && now-beat<=3000) timeout=DWORD(3001-(now-beat));
                    for(auto [id,when]:retry) {
                        DWORD delay=when<=now?0:DWORD(std::min<ULONGLONG>(when-now,MAXDWORD-1));
                        timeout=std::min(timeout,delay);
                    }
                    DWORD result=WaitForMultipleObjects(2,handles,FALSE,timeout);
                    if(result==WAIT_OBJECT_0) break;
                    if(result==WAIT_FAILED) { lastError=HRESULT_FROM_WIN32(GetLastError()); ++failures; break; }
                    ++workerWakeups;
                }
            } catch(...) { lastError=static_cast<unsigned long>(E_OUTOFMEMORY); ++failures; }
            for(auto& [id,s]:sources) s->requestStop();
            sources.clear();
            std::atomic_store(&renderSources,std::shared_ptr<const RenderSources>{});
            CoUninitialize();
        });
    }
    bool render(UINT32 frames, UINT64 /*position*/, UINT64 qpc, DWORD flags) {
        // Reserve at construction; never grow the PCM buffers in GetBuffer.
        if(frames>fmt.rate) throw std::bad_alloc();
        sinkQpc=qpc;sinkFlags=flags;
        output.assign(size_t(frames)*fmt.align,0);
        if(!fmt.supported || flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) return false;
        const int64_t first=frameAt(qpc,fmt.rate)-int64_t(fmt.rate)*MIX_DELAY_MS/1000;
        readFrame=first;
        mix.assign(size_t(frames)*fmt.channels,0);
        auto allowed=policy();
        auto snapshot=std::atomic_load(&renderSources);
        if(snapshot) for(auto& s:*snapshot) {
            float gain=0;
            if(allowed) { auto i=allowed->find(s.pid); if(i!=allowed->end()) gain=i->second; }
            // Consume even muted queues so unmuting cannot replay old samples.
            const auto covered=s.ring->add(first,frames,mix,gain);
            if(gain!=0) { renderedSourceFrames+=frames; missingSourceFrames+=frames-std::min(frames,covered); }
        }
        bool audible=false; double energy=0;
        for(size_t i=0;i<mix.size();++i) {
            float sample=std::clamp(mix[i],-1.f,1.f);
            energy+=double(sample)*sample; audible|=std::abs(sample)>0.00003f;
            if(fmt.floating) reinterpret_cast<float*>(output.data())[i]=sample;
            else reinterpret_cast<int16_t*>(output.data())[i]=int16_t(std::clamp(std::lround(sample*32768),-32768L,32767L));
        }
        outputEnergy+=static_cast<unsigned long long>(energy*1000000000.0); outputSamples+=mix.size();
        return audible;
    }
};
static std::mutex contextsMutex;
static std::map<void*,std::shared_ptr<Context>> contexts;

static void wakeContexts() {
    std::lock_guard<std::mutex> lock(contextsMutex);
    for(auto& [key,ctx]:contexts) SetEvent(ctx->wake);
}
API void RouterConfigureVolumes(const DWORD* ids, const float* gains, unsigned count) {
    try {
        auto next=std::make_shared<Policy>();
        for(unsigned i=0;i<std::min(count,64u);++i) {
            if(ids[i] && ids[i]!=GetCurrentProcessId())
                (*next)[ids[i]]=gains?(std::isfinite(gains[i])?std::clamp(gains[i],0.f,2.f):0.f):1.f;
        }
        std::atomic_store(&permitted,std::shared_ptr<const Policy>(next));
        heartbeat=GetTickCount64(); wakeContexts();
    } catch(...) { heartbeat=0; lastError=static_cast<unsigned long>(E_OUTOFMEMORY); ++failures; wakeContexts(); }
}
API void RouterConfigure(const DWORD* ids, unsigned count) { RouterConfigureVolumes(ids,nullptr,count); }
API void RouterHeartbeat() { if(!terminating) { heartbeat=GetTickCount64(); wakeContexts(); } }
API void RouterSetOriginal(void* fn) { originalGetBuffer=reinterpret_cast<GetBufferFn>(fn); }
API int RouterRegister(void* capture, const WAVEFORMATEX* fmt) {
    try {
        auto ctx=std::make_shared<Context>(fmt);
        { std::lock_guard<std::mutex> lock(contextsMutex); if(contexts.count(capture)) return 0; contexts[capture]=ctx; }
        ctx->run(); return ctx->fmt.supported ? 0 : int(AUDCLNT_E_UNSUPPORTED_FORMAT);
    } catch(...) { lastError=static_cast<unsigned long>(E_OUTOFMEMORY); ++failures; return int(E_OUTOFMEMORY); }
}
API void RouterRemove(void* capture) {
    std::shared_ptr<Context> old;
    { std::lock_guard<std::mutex> lock(contextsMutex); auto i=contexts.find(capture); if(i!=contexts.end()) { old=i->second; contexts.erase(i); } }
}
API HRESULT STDMETHODCALLTYPE RouterGetBuffer(IAudioCaptureClient* self, BYTE** data, UINT32* frames, DWORD* flags, UINT64* posOut, UINT64* qpcOut) {
    if(!originalGetBuffer) return E_UNEXPECTED;
    std::shared_ptr<Context> ctx;
    { std::lock_guard<std::mutex> lock(contextsMutex); auto i=contexts.find(self); if(i!=contexts.end()) ctx=i->second; }
    UINT64 pos=0,qpc=0;
    HRESULT hr=originalGetBuffer(self,data,frames,flags,ctx?&pos:posOut,ctx?&qpc:qpcOut);
    if(!ctx || FAILED(hr) || !frames || !*frames) return hr;
    if(posOut) *posOut=pos; if(qpcOut) *qpcOut=qpc;
    try {
        bool audible=ctx->render(*frames,pos,qpc,*flags);
        *data=ctx->output.data(); *flags &= ~AUDCLNT_BUFFERFLAGS_SILENT;
        if(!audible) *flags |= AUDCLNT_BUFFERFLAGS_SILENT;
        ++packets; if(audible) ++audiblePackets;
    } catch(...) { *flags |= AUDCLNT_BUFFERFLAGS_SILENT; lastError=static_cast<unsigned long>(E_OUTOFMEMORY); ++failures; }
    return hr;
}
API void RouterStats(unsigned long long* out) {
    std::lock_guard<std::mutex> lock(contextsMutex);
    out[0]=contexts.size(); out[1]=sourceCount; out[2]=packets; out[3]=audiblePackets; out[4]=failures; out[5]=lastError;
    out[6]=sourcePackets;out[7]=sourceLoud;out[8]=sourceQpc;out[9]=sinkQpc;out[10]=sourceFlags;out[11]=sinkFlags;out[12]=writtenFrame;out[13]=readFrame;
    out[14]=outputEnergy;out[15]=outputSamples;
}
API void RouterSilence() { std::atomic_store(&permitted,std::shared_ptr<const Policy>{}); heartbeat=0; wakeContexts(); }
API void RouterShutdown() {
    terminating=true; RouterSilence();
    // Stop sources while preserving buffers that the consumer may still hold.
    std::vector<std::shared_ptr<Context>> all;
    { std::lock_guard<std::mutex> lock(contextsMutex); for(auto& [k,v]:contexts) { v->requestStop(); all.push_back(v); } }
    for(auto& v:all) if(v->worker.joinable()) v->worker.join();
}
API void RouterReset() { terminating=false; }

API int RouterSelfTest() {
    Ring r(8,2); float a[]={0.25f,-0.5f}; r.put(100,a);
    std::vector<float> out(4); r.add(100,2,out);
    if(out[0]!=0.25f || out[1]!=-0.5f || out[2]!=0) return 1;
    out.assign(4,0); r.add(100,2,out); if(out[0]!=0) return 2; // Consumed frames cannot replay.
    if(frameAt(123456789,48000)!=592592) return 3;
    return 0;
}

API void RouterPerformance(unsigned long long* out) { out[0]=workerWakeups; out[1]=droppedFrames; }

API void RouterDiagnostics(unsigned long long* out) { out[0]=captureWakeups; out[1]=renderedSourceFrames; out[2]=missingSourceFrames; out[3]=MIX_DELAY_MS; }
