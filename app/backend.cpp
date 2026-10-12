#include "backend.h"
#include "runtime.h"
#include <chrono>
#include <optional>
#include <stdexcept>
#include <vector>
Backend::Backend(const Settings& value, QString path) : settings(value), directory(std::move(path)) {
    qRegisterMetaType<ViewState>();
    inventory = static_cast<Inventory*>(InventoryCreate());
    if(!inventory) throw std::runtime_error("Не удалось запустить Core Audio.");
}
void Backend::start() { worker = std::thread([this] { run(); }); }
Backend::~Backend() { stop(); if(worker.joinable()) worker.join(); InventoryDestroy(inventory); }
void Backend::stop() { stopping = true; InventoryWake(inventory); }
void Backend::refresh() { InventoryRefresh(inventory); }
void Backend::updateSettings(const Settings& value) {
    { std::lock_guard<std::mutex> lock(mutex); settings = value; ++revision; }
    InventoryWake(inventory);
}
void Backend::connectTarget(bool automatic) {
    { std::lock_guard<std::mutex> lock(mutex); command = automatic ? 1 : 2; } InventoryWake(inventory);
}
void Backend::disconnectTarget() {
    { std::lock_guard<std::mutex> lock(mutex); command = 3; } InventoryWake(inventory);
}
void Backend::run() {
    using Clock = std::chrono::steady_clock;
    void* session = nullptr; DWORD target = 0;
    ClockProcess clock;
    std::vector<ProcessRow> rows(256); std::vector<AudioRow> audio(64);
    QVector<quint32> sentPids; QVector<float> sentGains;
    bool haveSent = false;
    uint64_t lastInventory = 0; quint64 lastSettings = ~quint64(0);
    ViewState state;
    auto nextHealth = Clock::now();
    QString previousError;
    auto request = [&](transport::Request data) {
        transport::Response response;
        checked(TransportRequest(session, &data, &response)); return response;
    };
    auto simple = [&](transport::Command cmd) { transport::Request r; r.command = cmd; return request(r); };
    auto release = [&] {
        if(session) { try { simple(transport::Shutdown); } catch(...) {} TransportClose(session); }
        session = nullptr; target = 0; clock.stop(); haveSent = false; lastSettings = ~quint64(0);
    };
    while(!stopping) {
        try {
            Settings current; quint64 currentRevision; int action;
            { std::lock_guard<std::mutex> lock(mutex); current = settings; currentRevision = revision; action = command; command = 0; }
            if(session && !TransportAlive(session)) { release(); emit connection(false, false, "Соединение потеряно. Перезапустите трансляцию после подключения."); }
            if(session && !clock.alive()) throw std::runtime_error("Фоновый процесс завершился. Подключитесь заново и перезапустите трансляцию.");
            if(action == 3) { release(); emit connection(false, false, "Отключено. Для обычного звука перезапустите трансляцию."); }
            else if(action == 1 && !current.automatic && !session) emit connection(false, false, "Автоподключение выключено");
            else if(((action == 1 && current.automatic) || action == 2) && !session) {
                DWORD found = 0; const auto result = TransportFindTarget(&found);
                if(action == 1 && result == ERROR_NOT_FOUND) emit connection(false, false, "Ожидание TeamSpeak");
                else {
                    checked(result); emit connection(false, true, "Подключение…");
                    clock.start();
                    try {
                        const auto library = cachedMixer(directory).toStdWString();
                        checked(TransportOpen(found, library.c_str(), clock.pid, FALSE, &session));
                        simple(transport::SelfTest); target = found; haveSent = false; lastSettings = ~quint64(0);
                        emit connection(true, false, "Подключено. Начните трансляцию; уже идущую перезапустите.");
                        nextHealth = Clock::now();
                    } catch(...) { release(); throw; }
                }
            }
            if(stopping) break;
            unsigned pc = 0, ac = 0; uint64_t inventoryRevision = 0; HRESULT hr;
            do {
                hr = InventoryRead(inventory, rows.data(), unsigned(rows.size()), audio.data(), unsigned(audio.size()), &pc, &ac, &inventoryRevision);
                if(hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA)) { rows.resize(std::max<size_t>(pc, rows.size())); audio.resize(std::max<size_t>(ac, audio.size())); }
            } while(hr == HRESULT_FROM_WIN32(ERROR_MORE_DATA));
            if(FAILED(hr) && hr != E_PENDING) throw std::runtime_error("Не удалось обновить список аудиосессий.");
            if(hr != E_PENDING && (inventoryRevision != lastInventory || currentRevision != lastSettings)) {
                state.processes.clear(); state.audio.clear();
                QMap<quint32, QString> names;
                for(unsigned i = 0; i < pc; ++i) {
                    Process p{rows[i].pid, rows[i].parent, QString::fromWCharArray(rows[i].name).toLower()};
                    names[p.pid] = p.name; state.processes.append(p);
                }
                for(unsigned i = 0; i < ac; ++i) if(names.contains(audio[i].pid))
                    state.audio[names[audio[i].pid]] = std::max(state.audio.value(names[audio[i].pid]), int(audio[i].state));
                state.selection = resolve(state.processes, current, {GetCurrentProcessId(), target, clock.pid});
                if(session && (!haveSent || sentPids != state.selection.pids || sentGains != state.selection.gains)) {
                    transport::Request config; config.command = transport::Configure; config.count = unsigned(state.selection.pids.size());
                    for(unsigned i = 0; i < config.count; ++i) { config.pids[i] = state.selection.pids[i]; config.gains[i] = state.selection.gains[i]; }
                    request(config); sentPids = state.selection.pids; sentGains = state.selection.gains; haveSent = true;
                }
                lastInventory = inventoryRevision; lastSettings = currentRevision;
                emit snapshot(state);
            }
            if(session && Clock::now() >= nextHealth) {
                simple(transport::Heartbeat); const auto reply = simple(transport::Stats);
                QVector<qulonglong> counters; for(auto n : reply.stats) counters.append(n);
                emit statistics(counters); nextHealth = Clock::now() + std::chrono::seconds(1);
            }
            previousError.clear();
        } catch(const std::exception& e) {
            // Any failed snapshot/control operation must invalidate the old allow-list.
            release(); emit connection(false, false, "Не подключено");
            const auto message = QString::fromUtf8(e.what());
            if(message != previousError) emit error(message);
            previousError = message;
            lastInventory = 0; nextHealth = Clock::now() + std::chrono::seconds(1);
        }
        if(stopping) break;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(nextHealth - Clock::now()).count();
        const DWORD timeout = session ? DWORD(std::max<long long>(0, remaining)) : 1000;
        if(InventoryWait(inventory, timeout) == WAIT_FAILED) { release(); emit error("Ошибка ожидания Core Audio."); break; }
    }
    release(); emit stopped();
}
