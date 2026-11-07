// TeleFiles - involucro minimo dell'interfaccia JSON di TDLib (td_json_client.h)
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

namespace tf {

using json = nlohmann::json;

class TdClient {
public:
    using Handler = std::function<void(const json&)>;

    TdClient();
    ~TdClient();

    // Avvia il thread di ricezione. 'onUpdate' riceve gli update (senza @extra nostro).
    void start(Handler onUpdate);
    void stop();

    // Invia una richiesta; 'cb' (opzionale) riceve la risposta o l'oggetto "error".
    void send(json req, Handler cb = nullptr);

    // Richiesta sincrona (solo per i metodi che TDLib consente in td_execute).
    static json execute(const json& req);

    static bool isError(const json& j) { return j.value("@type", "") == "error"; }

private:
    void loop();

    int clientId_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mu_;
    std::map<uint64_t, Handler> pending_;
    uint64_t nextId_ = 1;
    Handler onUpdate_;
};

} // namespace tf
