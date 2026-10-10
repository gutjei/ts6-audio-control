#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <ksmedia.h>
#include <wrl.h>
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
static std::mutex policyMutex;
static std::map<DWORD,float> permitted;
static std::atomic<ULONGLONG> heartbeat{0};
static std::atomic<unsigned long long> packets{0}, audiblePackets{0}, failures{0};
static std::atomic<unsigned long long> sourcePackets{0}, sourceLoud{0}, sourceQpc{0}, sinkQpc{0}, sourceFlags{0}, sinkFlags{0}, writtenFrame{0}, readFrame{0};
static std::atomic<unsigned long long> outputEnergy{0}, outputSamples{0};
static std::atomic<unsigned long> lastError{0}, sourceCount{0};
static std::atomic<bool> terminating{false};

static std::map<DWORD,float> policy() {
    if (terminating || GetTickCount64() - heartbeat.load() > 3000) return {};
    std::lock_guard<std::mutex> lock(policyMutex);
    return permitted;
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

// Timestamped circular buffer: readers never consume stale frames after wraparound.
struct Ring {
    unsigned channels, capacity;
    std::vector<float> samples;
    std::vector<int64_t> tags;
    Ring(unsigned rate, unsigned ch) : channels(ch), capacity(rate * 2), samples(size_t(capacity)*ch), tags(capacity, -1) {}
    void put(int64_t frame, const float* values) {
        if (frame < 0) return;
        auto slot = size_t(frame % capacity);
        std::copy(values, values+channels, samples.data()+slot*channels); tags[slot] = frame;
    }
    void add(int64_t first, unsigned frames, std::vector<float>& out, float gain=1.f) const {
        for (unsigned i=0; i<frames; ++i) {
            int64_t f = first+i; if (f<0) continue;
            auto slot=size_t(f%capacity); if (tags[slot]!=f) continue;
            for (unsigned c=0;c<channels;++c) out[size_t(i)*channels+c] += samples[slot*channels+c] * gain;
        }
    }
};

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
    Ring ring;
    int64_t anchor = 0;
    Source(DWORD id, const Format& f) : pid(id), ring(f.rate, f.channels) {}
    ~Source() { if(client) client->Stop(); if(process) CloseHandle(process); }
    HRESULT start(Format& fmt) {
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
        if(WaitForSingleObject(cb->event, 2000)!=WAIT_OBJECT_0) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        if(FAILED(cb->result)) return cb->result;
        client=cb->client;
        hr=client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            200000, 0, fmt.wave(), nullptr);
        if(FAILED(hr)) return hr;
        hr=client->GetService(IID_PPV_ARGS(&capture)); if(FAILED(hr)) return hr;
        return client->Start();
    }
};

struct Context {
    Format fmt;
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::map<DWORD,std::shared_ptr<Source>> sources;
    std::vector<BYTE> output;
    std::vector<float> mix;
    std::thread worker;
    explicit Context(const WAVEFORMATEX* f) : fmt(f) {}
    ~Context() { stop=true; if(worker.joinable()) worker.join(); }
    void run() {
        if(!fmt.supported) { lastError=static_cast<unsigned long>(AUDCLNT_E_UNSUPPORTED_FORMAT); ++failures; return; }
        worker=std::thread([this] {
            HRESULT com=CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if(FAILED(com)) { lastError=com; ++failures; return; }
            std::map<DWORD, ULONGLONG> retry;
            while(!stop && !terminating) {
                auto wanted=policy();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    for(auto i=sources.begin();i!=sources.end();) {
                        if(!wanted.count(i->first) || WaitForSingleObject(i->second->process,0)==WAIT_OBJECT_0) {
                            --sourceCount; i=sources.erase(i);
                        } else ++i;
                    }
                }
                for(auto [id,gain]:wanted) {
                    if(stop || terminating) break;
                    { std::lock_guard<std::mutex> lock(mutex); if(sources.count(id)) continue; }
                    auto now=GetTickCount64(); if(retry[id]>now) continue; retry[id]=now+3000;
                    auto s=std::make_shared<Source>(id,fmt);
                    HRESULT hr=s->start(fmt);
                    if(FAILED(hr)) { lastError=hr; ++failures; continue; }
                    std::lock_guard<std::mutex> lock(mutex); sources[id]=s; ++sourceCount;
                }
                std::vector<std::shared_ptr<Source>> snapshot;
                { std::lock_guard<std::mutex> lock(mutex); for(auto& [id,s]:sources) snapshot.push_back(s); }
                for(auto& s:snapshot) {
                    for(unsigned packet=0;packet<64 && !stop;++packet) {
                        UINT32 n=0; DWORD flags=0; UINT64 pos=0,qpc=0; BYTE* data=nullptr;
                        HRESULT hr=s->capture->GetBuffer(&data,&n,&flags,&pos,&qpc);
                        if(hr==AUDCLNT_S_BUFFER_EMPTY || (SUCCEEDED(hr)&&!n)) break;
                        if(FAILED(hr)) { lastError=hr; ++failures; break; }
                        ++sourcePackets; sourceQpc=qpc; sourceFlags=flags;
                        if(data && !(flags&AUDCLNT_BUFFERFLAGS_SILENT)) {
                            for(unsigned k=0;k<n*fmt.channels;++k) {
                                float v=fmt.floating?reinterpret_cast<float*>(data)[k]:reinterpret_cast<int16_t*>(data)[k]/32768.f;
                                if(std::abs(v)>.00003f) {++sourceLoud;break;}
                            }
                        }
                        if(!(flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) {
                            // Process loopback can return a constant device position.
                            // Its QPC timestamp is the usable common timeline.
                            s->anchor=frameAt(qpc,fmt.rate);
                            std::lock_guard<std::mutex> lock(mutex);
                            writtenFrame=s->anchor;
                            for(unsigned i=0;i<n;++i) {
                                float values[8]{};
                                if(!(flags&AUDCLNT_BUFFERFLAGS_SILENT) && data)
                                    for(unsigned c=0;c<fmt.channels;++c) {
                                        size_t k=size_t(i)*fmt.channels+c;
                                        values[c]=fmt.floating ? reinterpret_cast<float*>(data)[k] : reinterpret_cast<int16_t*>(data)[k]/32768.f;
                                        if(!std::isfinite(values[c])) values[c]=0;
                                    }
                                s->ring.put(s->anchor+i, values);
                            }
                        }
                        s->capture->ReleaseBuffer(n);
                    }
                }
                Sleep(4);
            }
            { std::lock_guard<std::mutex> lock(mutex); sourceCount-=unsigned(sources.size()); sources.clear(); }
            CoUninitialize();
        });
    }
    bool render(UINT32 frames, UINT64 /*position*/, UINT64 qpc, DWORD flags) {
        sinkQpc=qpc;sinkFlags=flags;
        output.assign(size_t(frames)*fmt.align,0);
        if(!fmt.supported || flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) return false;
        // 100 ms of alignment headroom for independent loopback capture streams.
        int64_t first=frameAt(qpc,fmt.rate)-fmt.rate/10;
        readFrame=first;
        mix.assign(size_t(frames)*fmt.channels,0);
        auto allowed=policy();
        {
            std::lock_guard<std::mutex> lock(mutex);
            for(auto& [id,s]:sources) {
                auto gain=allowed.find(id);
                if(gain!=allowed.end()) s->ring.add(first,frames,mix,gain->second);
            }
        }
        bool audible=false;
        double energy=0;
        for(size_t i=0;i<mix.size();++i) {
            float sample=std::clamp(mix[i],-1.f,1.f);
            energy+=double(sample)*sample;
            audible|=std::abs(sample)>0.00003f;
            if(fmt.floating) reinterpret_cast<float*>(output.data())[i]=sample;
            else reinterpret_cast<int16_t*>(output.data())[i]=int16_t(std::clamp(std::lround(sample*32768),-32768L,32767L));
        }
        outputEnergy+=static_cast<unsigned long long>(energy*1000000000.0);
        outputSamples+=mix.size();
        return audible;
    }
};
static std::mutex contextsMutex;
static std::map<void*,std::shared_ptr<Context>> contexts;

API void RouterConfigure(const DWORD* ids, unsigned count) {
    std::lock_guard<std::mutex> lock(policyMutex);
    permitted.clear();
    for(unsigned i=0;i<std::min(count,64u);++i) if(ids[i] && ids[i]!=GetCurrentProcessId()) permitted[ids[i]]=1.f;
    heartbeat=GetTickCount64();
}
API void RouterConfigureVolumes(const DWORD* ids, const float* gains, unsigned count) {
    std::lock_guard<std::mutex> lock(policyMutex);
    permitted.clear();
    for(unsigned i=0;i<std::min(count,64u);++i) {
        if(ids[i] && ids[i]!=GetCurrentProcessId())
            permitted[ids[i]]=std::isfinite(gains[i])?std::clamp(gains[i],0.f,2.f):0.f;
    }
    heartbeat=GetTickCount64();
}
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
API void RouterSilence() { std::lock_guard<std::mutex> lock(policyMutex); permitted.clear(); heartbeat=0; }
API void RouterShutdown() {
    terminating=true; RouterSilence();
    // Stop sources while preserving buffers that the consumer may still hold.
    std::vector<std::shared_ptr<Context>> all;
    { std::lock_guard<std::mutex> lock(contextsMutex); for(auto& [k,v]:contexts) { v->stop=true; all.push_back(v); } }
    for(auto& v:all) if(v->worker.joinable()) v->worker.join();
}
API void RouterReset() { terminating=false; }

// Enumerate sessions from every active playback device, not just the default one.
API unsigned RouterSessions(DWORD* pids, DWORD* states, unsigned capacity) {
    HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    unsigned count=0;
    {
        ComPtr<IMMDeviceEnumerator> e;
        if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&e)))) {
            ComPtr<IMMDeviceCollection> ds;
            if(SUCCEEDED(e->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&ds))) {
                UINT devices=0; ds->GetCount(&devices);
                for(UINT d=0;d<devices;++d) {
                    ComPtr<IMMDevice> device; if(FAILED(ds->Item(d,&device))) continue;
                    ComPtr<IAudioSessionManager2> manager;
                    if(FAILED(device->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,&manager))) continue;
                    ComPtr<IAudioSessionEnumerator> sessions; if(FAILED(manager->GetSessionEnumerator(&sessions))) continue;
                    int n=0;sessions->GetCount(&n);
                    for(int i=0;i<n && count<capacity;++i) {
                        ComPtr<IAudioSessionControl> c; ComPtr<IAudioSessionControl2> c2;
                        if(FAILED(sessions->GetSession(i,&c)) || FAILED(c.As(&c2))) continue;
                        DWORD pid=0; AudioSessionState state=AudioSessionStateExpired;
                        c2->GetProcessId(&pid); c->GetState(&state);
                        if(pid && state!=AudioSessionStateExpired) { pids[count]=pid; states[count]=DWORD(state); ++count; }
                    }
                }
            }
        }
    }
    if(SUCCEEDED(init)) CoUninitialize(); return count;
}
API int RouterSelfTest() {
    Ring r(8,2); float a[]={0.25f,-0.5f}; r.put(100,a);
    std::vector<float> out(4); r.add(100,2,out);
    if(out[0]!=0.25f || out[1]!=-0.5f || out[2]!=0) return 1;
    r.put(116,a); out.assign(4,0); r.add(100,2,out); if(out[0]!=0) return 2;
    if(frameAt(123456789,48000)!=592592) return 3;
    return 0;
}
