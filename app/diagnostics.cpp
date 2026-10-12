#include "runtime.h"
#include <QApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QSaveFile>
#include <QWidget>
#include <memory>
#include <stdexcept>
int diagnose(const QString& output, const QString& directory) {
    QJsonObject report{{"runtime", "C++ / Qt Widgets"}, {"qt", qVersion()}, {"ok", false}};
    QJsonArray checks;
    auto record = [&](const QString& name) { checks.append(name); };
    struct Session {
        void* handle = nullptr;
        ~Session() { if(handle) { transport::Request q; q.command = transport::Shutdown; transport::Response r; TransportRequest(handle,&q,&r); TransportClose(handle); } }
    };
    try {
        QWidget probe; probe.resize(160,80); probe.show(); qApp->processEvents();
        if(probe.grab().isNull()) throw std::runtime_error("Qt platform rendering failed");
        probe.hide(); record("Qt Widgets rendering");
        std::unique_ptr<Inventory, decltype(&InventoryDestroy)> inventory(static_cast<Inventory*>(InventoryCreate()), InventoryDestroy);
        if(!inventory) throw std::runtime_error("Core Audio inventory failed");
        unsigned pc=0, ac=0; uint64_t revision=0; HRESULT hr=E_PENDING;
        for(int i=0;i<20 && hr==E_PENDING;++i) {
            InventoryWait(inventory.get(),500);
            hr=InventoryRead(inventory.get(),nullptr,0,nullptr,0,&pc,&ac,&revision);
        }
        if(hr!=S_OK && hr!=HRESULT_FROM_WIN32(ERROR_MORE_DATA)) throw std::runtime_error("Core Audio snapshot failed");
        report["process_count"]=int(pc); report["audio_sessions"]=int(ac); record("Core Audio inventory");
        ClockProcess target, clock; target.start(); clock.start();
        const auto mixer=cachedMixer(directory).toStdWString();
        Session session; checked(TransportOpen(target.pid,mixer.c_str(),clock.pid,TRUE,&session.handle)); record("Native child attach / IPC");
        auto request=[&](transport::Command command) { transport::Request q; q.command=command; transport::Response r; checked(TransportRequest(session.handle,&q,&r)); return r; };
        request(transport::SelfTest); record("Native mixer self-test");
        request(transport::Configure); request(transport::Heartbeat);
        const auto stats=request(transport::Stats); QJsonArray counters;
        for(auto counter:stats.stats) counters.append(QString::number(counter)); report["stats"]=counters;
        record("Configure / heartbeat / stats");
        void* duplicate=nullptr; const auto result=TransportOpen(target.pid,mixer.c_str(),clock.pid,TRUE,&duplicate);
        if(duplicate) TransportClose(duplicate);
        if(result!=ERROR_PIPE_BUSY) throw std::runtime_error("Duplicate controller was not rejected");
        record("Exclusive controller ownership");
        request(transport::Shutdown); TransportClose(session.handle); session.handle=nullptr;
        checked(TransportOpen(target.pid,mixer.c_str(),clock.pid,TRUE,&session.handle)); request(transport::SelfTest);
        record("Disconnect / reconnect"); report["ok"]=true;
    } catch(const std::exception& e) { report["error"]=QString::fromUtf8(e.what()); }
    report["checks"]=checks;
    QSaveFile file(output); const auto data=QJsonDocument(report).toJson(QJsonDocument::Indented);
    if(!file.open(QIODevice::WriteOnly) || file.write(data)!=data.size() || !file.commit()) return 2;
    return report["ok"].toBool()?0:1;
}
