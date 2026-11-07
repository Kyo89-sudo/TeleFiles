#include "TdClient.h"

#include <td/telegram/td_json_client.h>

namespace tf {

TdClient::TdClient() {
    // Riduce il log di TDLib al minimo (solo errori).
    execute({{"@type", "setLogVerbosityLevel"}, {"new_verbosity_level", 1}});
    clientId_ = td_create_client_id();
}

TdClient::~TdClient() { stop(); }

void TdClient::start(Handler onUpdate) {
    onUpdate_ = std::move(onUpdate);
    running_ = true;
    thread_ = std::thread([this] { loop(); });
    // La prima richiesta "sveglia" il client: TDLib risponde con updateAuthorizationState.
    send({{"@type", "getOption"}, {"name", "version"}});
}

void TdClient::stop() {
    running_ = false;
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
}

void TdClient::send(json req, Handler cb) {
    uint64_t id;
    {
        std::lock_guard<std::mutex> g(mu_);
        id = nextId_++;
        if (cb) pending_[id] = std::move(cb);
    }
    req["@extra"] = id;
    td_send(clientId_, req.dump().c_str());
}

json TdClient::execute(const json& req) {
    const char* r = td_execute(req.dump().c_str());
    if (!r) return json();
    return json::parse(r, nullptr, false);
}

void TdClient::loop() {
    while (running_) {
        const char* r = td_receive(0.5);
        if (!r) continue;
        json j = json::parse(r, nullptr, false);
        if (j.is_discarded()) continue;
        if (j.contains("@client_id") && j["@client_id"].get<int>() != clientId_) continue;
        if (j.contains("@extra") && j["@extra"].is_number_unsigned()) {
            uint64_t id = j["@extra"].get<uint64_t>();
            Handler h;
            {
                std::lock_guard<std::mutex> g(mu_);
                auto it = pending_.find(id);
                if (it != pending_.end()) {
                    h = std::move(it->second);
                    pending_.erase(it);
                }
            }
            if (h) h(j);
            continue;
        }
        if (onUpdate_) onUpdate_(j);
        if (j.value("@type", "") == "updateAuthorizationState" &&
            j["authorization_state"].value("@type", "") == "authorizationStateClosed") {
            // Il client è chiuso: il thread può terminare.
            running_ = false;
        }
    }
}

} // namespace tf
