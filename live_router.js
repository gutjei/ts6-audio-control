'use strict';
// Only control calls cross JS; PCM processing stays in native/router.cpp.
const native = Module.load(ROUTER_DLL);
const api = (name, result, args) => new NativeFunction(native.getExportByName(name), result, args);
const configure = api('RouterConfigureVolumes','void',['pointer','pointer','uint']);
const heartbeat = api('RouterHeartbeat','void',[]);
const stats = api('RouterStats','void',['pointer']);
const registerCapture = api('RouterRegister','int',['pointer','pointer']);
const removeCapture = api('RouterRemove','void',['pointer']);
const setOriginal = api('RouterSetOriginal','void',['pointer']);
const silence = api('RouterSilence','void',[]);
const shutdown = api('RouterShutdown','void',[]);
api('RouterReset','void',[])();
const clients = new Map(), captures = new Set(), handlers = new Set(), operations = new Set();
const attached = new Set(), allocations = [];
const targetModule = typeof TEST_TARGET_MODULE === 'string' ? TEST_TARGET_MODULE : 'teamspeak.dll';
let getBufferAddress = null;
let enabled = true;
function method(object, slot) { return object.readPointer().add(slot * Process.pointerSize).readPointer(); }
function once(address, label, callbacks) {
    const key = address.toString() + ':' + label;
    if (!attached.has(key)) { Interceptor.attach(address, callbacks); attached.add(key); }
}
function guid(address) { return Array.from(new Uint8Array(address.readByteArray(16)), b=>b.toString(16).padStart(2,'0')).join(''); }
function trackRelease(object) {
    once(method(object,2),'release',{
        onEnter(args) { this.key=args[0].toString(); },
        onLeave(ret) {
            if(ret.toUInt32()!==0) return;
            clients.delete(this.key); operations.delete(this.key); handlers.delete(this.key);
            if(captures.delete(this.key)) removeCapture(ptr(this.key));
        }
    });
}
function trackClient(object) {
    const key=object.toString(); if(clients.has(key)) return;
    clients.set(key,{format:null}); trackRelease(object);
    once(method(object,3),'initialize',{
        onEnter(args) {
            this.state=clients.get(args[0].toString());
            if(!this.state) return;
            const f=args[5], size=18+f.add(16).readU16();
            if(size>274) throw new Error('Unexpected WAVEFORMAT size');
            this.format=Memory.alloc(size); this.format.writeByteArray(f.readByteArray(size));
            this.description={rate:f.add(4).readU32(),channels:f.add(2).readU16(),bits:f.add(14).readU16()};
        },
        onLeave(ret) {
            if(this.state && ret.toInt32()>=0) {
                this.state.format=this.format;
                send({event:'format',...this.description});
            }
        }
    });
    once(method(object,14),'get-service',{
        onEnter(args) {
            this.state=clients.get(args[0].toString());
            this.capture=this.state && guid(args[1])==='64bdadc81ee7a048a4de185c395cd317';
            this.output=args[2];
        },
        onLeave(ret) {
            if(!this.capture || ret.toInt32()<0 || !this.state.format) return;
            const capture=this.output.readPointer();
            const address=method(capture,3);
            if(getBufferAddress===null) {
                const original=Interceptor.replaceFast(address,native.getExportByName('RouterGetBuffer'));
                setOriginal(original); getBufferAddress=address;
            } else if(!getBufferAddress.equals(address)) {
                send({event:'error',message:'Unexpected second capture implementation; silent fallback'}); return;
            }
            const result=registerCapture(capture,this.state.format);
            captures.add(capture.toString()); trackRelease(capture);
            send({event:'capture-ready',capture:capture.toString(),result});
        }
    });
}
function trackOperation(op) {
    operations.add(op.toString()); trackRelease(op);
    once(method(op,3),'activation-result',{
        onEnter(args) {
            this.watched=operations.has(args[0].toString()); this.hr=args[1];this.output=args[2];
        },
        onLeave(ret) {
            if(this.watched && ret.toInt32()>=0 && this.hr.readS32()>=0) {
                const object=this.output.readPointer(); if(!object.isNull()) trackClient(object);
            }
        }
    });
}
const observer=Process.attachModuleObserver({onAdded(module) {
    if(module.name.toLowerCase()!=='mmdevapi.dll') return;
    once(module.getExportByName('ActivateAudioInterfaceAsync'),'activation',{
        onEnter(args) {
            if(!enabled) return;
            const caller=Process.findModuleByAddress(this.returnAddress);
            if(!caller || caller.name.toLowerCase()!==targetModule || args[0].readUtf16String()!=='VAD\\Process_Loopback') return;
            // Underlying clock captures a dedicated silent process. Detach cannot leak desktop audio.
            const blob=Memory.alloc(12); blob.writeU32(1);blob.add(4).writeU32(SILENT_PID);blob.add(8).writeU32(0);
            const variant=Memory.alloc(24); variant.writeU16(65);variant.add(8).writeU32(12);variant.add(16).writePointer(blob);
            allocations.push({blob,variant}); args[2]=variant;
            const handler=args[3]; handlers.add(handler.toString());
            once(method(handler,3),'completed',{
                onEnter(a) { if(handlers.has(a[0].toString())) trackOperation(a[1]); }
            });
            this.output=args[4]; this.watched=true;
            send({event:'activation',silentPid:SILENT_PID});
        },
        onLeave(ret) {
            if(this.watched && ret.toInt32()>=0 && !this.output.isNull()) {
                const op=this.output.readPointer(); if(!op.isNull()) trackOperation(op);
            }
        }
    });
    send({event:'hook-ready',hostPid:Process.id});
}});
rpc.exports={
    heartbeat() { if(enabled) heartbeat(); },
    configure(ids, gains) {
        if(ids.length>64 || ids.some(x=>!Number.isInteger(x)||x<=0)) throw new Error('Invalid PID set');
        if(gains==null) gains=ids.map(()=>1);
        if(gains.length!==ids.length || gains.some(x=>!Number.isFinite(x)||x<0||x>2)) throw new Error('Invalid volume set');
        const data=Memory.alloc(Math.max(4,ids.length*4));
        const levels=Memory.alloc(Math.max(4,ids.length*4));
        ids.forEach((id,i)=>{data.add(i*4).writeU32(id);levels.add(i*4).writeFloat(gains[i]);});
        configure(data,levels,ids.length);
    },
    stats() {
        const out=Memory.alloc(128); stats(out);
        return Array.from({length:16},(_,i)=>Number(out.add(i*8).readU64().toString()));
    },
    shutdown() { enabled=false; silence(); shutdown(); },
    selftest() { return api('RouterSelfTest','int',[])(); }
};
