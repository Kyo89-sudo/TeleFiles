#include "Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <ctime>

#include "FileIO.h"
#include "Split.h"

namespace tf {

// ---------------------------------------------------------------- utilità JSON

static const json& jget(const json& j, const char* k) {
    static const json null;
    if (!j.is_object()) return null;
    auto it = j.find(k);
    return it == j.end() ? null : *it;
}
static std::string jstr(const json& j, const char* k) {
    const json& v = jget(j, k);
    return v.is_string() ? v.get<std::string>() : std::string();
}
static int64_t jint(const json& j, const char* k) {
    const json& v = jget(j, k);
    if (v.is_number_integer()) return v.get<int64_t>();
    if (v.is_string()) {  // TDLib serializza gli int64 come stringhe
        try { return std::stoll(v.get<std::string>()); } catch (...) { return 0; }
    }
    return 0;
}
static bool jbool(const json& j, const char* k) {
    const json& v = jget(j, k);
    return v.is_boolean() && v.get<bool>();
}
static std::string jtype(const json& j) { return jstr(j, "@type"); }

// ---------------------------------------------------------------- cartelle nella didascalia

// Riga della didascalia con il percorso relativo: "📁 Vacanze/2024"
static const char kFolderMark[] = "\xF0\x9F\x93\x81 ";

// Ripulisce un percorso relativo: niente "..", niente caratteri non validi su Windows, separatore "/".
static std::string cleanFolder(const std::string& raw) {
    std::string out, seg;
    auto flush = [&] {
        if (!seg.empty() && seg != "." && seg != "..") {
            if (!out.empty()) out += '/';
            out += sanitizeFileName(seg);
        }
        seg.clear();
    };
    for (char c : raw) {
        if (c == '/' || c == '\\') flush();
        else if (c != '\r' && c != '\n') seg += c;
    }
    flush();
    return out.size() > 400 ? out.substr(0, 400) : out;
}

static std::string folderFromCaption(const std::string& caption) {
    size_t pos = 0;
    while (pos < caption.size()) {
        size_t nl = caption.find('\n', pos);
        std::string line = caption.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (line.compare(0, sizeof(kFolderMark) - 1, kFolderMark) == 0)
            return cleanFolder(line.substr(sizeof(kFolderMark) - 1));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return {};
}

// ---------------------------------------------------------------- formattazione

static std::string fmtDec(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    std::string s = buf;
    for (char& c : s) if (c == '.') c = ',';
    return s;
}

std::string formatSize(int64_t b) {
    if (b < 0) return "?";
    if (b < 1024) return std::to_string(b) + " B";
    const char* u[] = {"KB", "MB", "GB", "TB"};
    double v = (double)b;
    int i = -1;
    while (v >= 1024 && i < 3) { v /= 1024; ++i; }
    return fmtDec(v, v < 10 ? 1 : (v < 100 ? 1 : 0)) + " " + u[i];
}

std::string formatSpeed(double bps) {
    if (bps < 1) return "0 KB/s";
    return formatSize((int64_t)bps) + "/s";
}

std::string formatEta(int64_t remaining, double bps) {
    if (bps < 1 || remaining <= 0) return "";
    int64_t s = (int64_t)((double)remaining / bps);
    if (s < 60) return std::to_string(s) + " s";
    if (s < 3600) return std::to_string(s / 60) + " min";
    return std::to_string(s / 3600) + " h " + std::to_string((s % 3600) / 60) + " min";
}

// ---------------------------------------------------------------- trasferimento

struct Engine::Transfer {
    int id = 0;
    Direction dir = Direction::Download;
    TStatus st = TStatus::Queued;
    std::string name, chatTitle, entryKey, error;
    int64_t chatId = 0;
    int64_t total = 0, done = 0, lastDone = 0;
    // controllo "bloccato": ultimo istante in cui i byte sono avanzati e quante volte si è riavviato
    std::chrono::steady_clock::time_point lastProgress = std::chrono::steady_clock::now();
    int64_t progressMark = -1;
    int stallRetries = 0;
    double speed = 0;
    bool pauseRequested = false, cancelRequested = false, workerBusy = false;
    std::shared_ptr<std::atomic<bool>> abort = std::make_shared<std::atomic<bool>>(false);

    // download
    FileEntry entry;
    std::vector<PartRef> parts;      // ordinate per indice (gruppi)
    int curPart = 0;
    int32_t curFileId = 0;
    std::string destPath, partialPath;
    int64_t baseDone = 0;
    Sha256 running;
    bool hashValid = true;
    bool partsCollected = false;

    // upload
    std::string srcPath;
    std::string folder;              // upload: percorso relativo da scrivere nella didascalia
    bool prepared = false, split = false;
    int64_t partSize = 0;
    int partCount = 1, partIdx = 0;
    std::string tempPart, gid, fullSha;
    Sha256 upRunning;                // SHA-256 del file intero, calcolato mentre si estraggono le parti
    int64_t upRunningBytes = 0;
    bool upRunningValid = true;
    int64_t prepDone = 0, prepTotal = 0;  // avanzamento dell'estrazione della parte
    std::vector<std::string> partShas;
    int32_t upFileId = 0;
    bool sending = false;            // messaggio con il file in corso (TDLib carica e poi invia)
    int64_t sendingMsgId = 0;        // id temporaneo del messaggio, noto dalla risposta a sendMessage
    bool stopWhenKnown = false;      // pausa/annulla chiesti prima di conoscere l'id: ritirarlo appena arriva

    bool active() const {
        return st == TStatus::Preparing || st == TStatus::Active || st == TStatus::Joining ||
               st == TStatus::Verifying;
    }
    int64_t partLen(int i) const {  // upload: lunghezza della parte i (0-based)
        if (!split) return total;
        return std::min(partSize, total - (int64_t)i * partSize);
    }
};

// ---------------------------------------------------------------- ciclo di vita

Engine::Engine(Config cfg) : cfg_(std::move(cfg)) {
    makeDirs(cfg_.dataDir);
    makeDirs(cfg_.tempDir);
    settings_.downloadDir = cfg_.defaultDownloadDir;
    loadSettings();
}

Engine::~Engine() { shutdown(); }

void Engine::start() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    loadQueue();
    td_ = std::make_unique<TdClient>();
    td_->start([this](const json& u) { onUpdate(u); });
}

void Engine::shutdown() {
    TdClient* td = nullptr;
    {
        std::lock_guard<std::recursive_mutex> g(mu_);
        if (shuttingDown_) return;
        shuttingDown_ = true;
        for (auto& t : transfers_) t->abort->store(true);
        saveQueue();
        saveSettings();
        td = td_.get();
        if (td) td->send({{"@type", "close"}});
    }
    // attende i thread di lavoro e la chiusura ordinata di TDLib (max ~5 s)
    for (int i = 0; i < 50 && workers_.load() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (td) {
        for (int i = 0; i < 30; ++i) {
            {
                std::lock_guard<std::recursive_mutex> g(mu_);
                if (recreateClient_) break;  // authorizationStateClosed ricevuto
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        td->stop();
    }
    std::lock_guard<std::recursive_mutex> g(mu_);
    td_.reset();
}

void Engine::changed() {
    if (notify_) notify_();
}

void Engine::tick() {
    bool recreate = false;
    {
        std::lock_guard<std::recursive_mutex> g(mu_);
        if (shuttingDown_) return;
        auto now = std::chrono::steady_clock::now();
        for (auto& t : transfers_) {
            double inst = (double)std::max<int64_t>(0, t->done - t->lastDone);
            t->lastDone = t->done;
            if (t->st == TStatus::Active)
                t->speed = t->speed <= 0 ? inst : t->speed * 0.6 + inst * 0.4;
            else
                t->speed = 0;
            // Un upload in corso appartiene a TDLib: ritirare il messaggio per un calo di
            // velocità può azzerare il progresso o sovrapporsi alla conferma dell'invio.
            if (t->dir != Direction::Download || t->st != TStatus::Active || t->workerBusy || t->sending) {
                t->progressMark = -1;
                continue;
            }
            if (t->done != t->progressMark) {
                t->progressMark = t->done;
                t->lastProgress = now;
                if (t->done > 0) t->stallRetries = 0;
                continue;
            }
            // prima dei primi byte di un passo TDLib può impiegare di più (connessione al datacenter
            // del file, file grandi): soglia più lunga; a metà trasferimento basta molto meno
            int limitMs = t->done == t->baseDone ? stallMs_ * 4 : stallMs_;
            if (now - t->lastProgress < std::chrono::milliseconds(limitMs)) continue;
            t->lastProgress = now;
            if (++t->stallRetries > 6) {
                log("fermo da troppo tempo, rinuncio: " + t->name);
                fail(*t, "Trasferimento fermo: nessun dato da diversi minuti. Riprova pi\u00F9 tardi.");
                continue;
            }
            log("nessun progresso da " + std::to_string(limitMs / 1000) + " s, riavvio (" + std::to_string(t->stallRetries) +
                "): " + t->name + " a " + std::to_string(t->done) + "/" + std::to_string(t->total));
            if (t->curFileId) {
                int32_t fid = t->curFileId;
                td_->send({{"@type", "cancelDownloadFile"}, {"file_id", fid}, {"only_if_pending", false}});
                td_->send({{"@type", "downloadFile"}, {"file_id", fid}, {"priority", 16},
                           {"offset", 0}, {"limit", 0}, {"synchronous", false}},
                          [this, fid](const json& r) {
                              std::lock_guard<std::recursive_mutex> lk1(mu_);
                              if (!TdClient::isError(r) && jint(r, "id") == fid) onFile(r);
                          });
            }
        }
        schedule();
        if (queueDirty_) saveQueue();
        recreate = recreateClient_;
        recreateClient_ = false;
    }
    if (recreate) {
        // Dopo il logout TDLib chiude il client: ne serve uno nuovo per un nuovo accesso.
        std::unique_ptr<TdClient> old;
        {
            std::lock_guard<std::recursive_mutex> g(mu_);
            old = std::move(td_);
        }
        if (old) old->stop();
        std::lock_guard<std::recursive_mutex> g(mu_);
        auth_ = AuthInfo();
        td_ = std::make_unique<TdClient>();
        td_->start([this](const json& u) { onUpdate(u); });
    }
    changed();
}

// ---------------------------------------------------------------- update TDLib

void Engine::onUpdate(const json& u) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    const std::string t = jtype(u);
    if (t == "updateAuthorizationState") {
        onAuthState(jget(u, "authorization_state"));
    } else if (t == "updateFile") {
        onFile(jget(u, "file"));
    } else if (t == "updateNewChat") {
        const json& c = jget(u, "chat");
        Chat ch;
        ch.id = jint(c, "id");
        ch.title = jstr(c, "title");
        const json& ty = jget(c, "type");
        std::string tt = jtype(ty);
        if (tt == "chatTypeSupergroup") {
            ch.kind = jbool(ty, "is_channel") ? ChatKind::Channel : ChatKind::Group;
            ch.supergroupId = jint(ty, "supergroup_id");
            auto st = supergroupStatus_.find(ch.supergroupId);
            if (st != supergroupStatus_.end()) onMemberStatus(ch, st->second);
        } else if (tt == "chatTypeBasicGroup") {
            ch.kind = ChatKind::Group;
            ch.basicGroupId = jint(ty, "basic_group_id");
            auto st = basicGroupStatus_.find(ch.basicGroupId);
            if (st != basicGroupStatus_.end()) onMemberStatus(ch, st->second);
        } else {
            ch.kind = ChatKind::Private;
        }
        ch.unread = (int)jint(c, "unread_count");
        if (ch.id == savedChatId_) ch.kind = ChatKind::Saved;
        chatMap_[ch.id] = ch;
        // le chat fuori dall'elenco principale (archiviate, abbandonate) arrivano con order 0
        bool listed = true;
        const json& positions = jget(c, "positions");
        if (positions.is_array()) {
            listed = positions.empty();  // senza posizioni: chat appena creata, la mostriamo
            for (auto& p : positions)
                if (jtype(jget(p, "list")) == "chatListMain" && jint(p, "order") != 0) listed = true;
        }
        if (listed && std::find(chatOrder_.begin(), chatOrder_.end(), ch.id) == chatOrder_.end())
            chatOrder_.push_back(ch.id);
        changed();
    } else if (t == "updateSupergroup" || t == "updateBasicGroup") {
        bool sg = t == "updateSupergroup";
        const json& grp = jget(u, sg ? "supergroup" : "basic_group");
        int64_t gid = jint(grp, "id");
        (sg ? supergroupStatus_ : basicGroupStatus_)[gid] = jget(grp, "status");
        for (auto& kv : chatMap_)
            if ((sg && kv.second.supergroupId == gid) || (!sg && kv.second.basicGroupId == gid))
                onMemberStatus(kv.second, jget(grp, "status"));
        changed();
    } else if (t == "updateChatPosition") {
        const json& p = jget(u, "position");
        if (jtype(jget(p, "list")) == "chatListMain") {
            int64_t id = jint(u, "chat_id");
            auto it = std::find(chatOrder_.begin(), chatOrder_.end(), id);
            if (jint(p, "order") == 0) {
                if (it != chatOrder_.end() && id != savedChatId_) chatOrder_.erase(it);
            } else if (it == chatOrder_.end() && chatMap_.count(id)) {
                chatOrder_.insert(chatOrder_.begin(), id);
            }
            changed();
        }
    } else if (t == "updateDeleteMessages") {
        if (jbool(u, "is_permanent") || !jbool(u, "from_cache")) {
            std::set<int64_t> ids;
            const json& arr = jget(u, "message_ids");
            if (arr.is_array())
                for (auto& v : arr) ids.insert(v.is_string() ? std::stoll(v.get<std::string>()) : v.get<int64_t>());
            removeMessages(jint(u, "chat_id"), ids);
        }
    } else if (t == "updateChatTitle") {
        auto it = chatMap_.find(jint(u, "chat_id"));
        if (it != chatMap_.end()) it->second.title = jstr(u, "title");
        changed();
    } else if (t == "updateChatReadInbox") {
        auto it = chatMap_.find(jint(u, "chat_id"));
        if (it != chatMap_.end()) it->second.unread = (int)jint(u, "unread_count");
        changed();
    } else if (t == "updateUser") {
        onUser(jget(u, "user"));
    } else if (t == "updateMessageSendSucceeded") {
        int64_t old = jint(u, "old_message_id");
        if (!sendingMsg_.count(old) && anySending()) earlySend_[old] = u;
        else onSendSucceeded(old, jget(u, "message"));
    } else if (t == "updateMessageSendFailed") {
        if (!sendingMsg_.count(jint(u, "old_message_id")) && anySending()) {
            earlySend_[jint(u, "old_message_id")] = u;
            return;
        }
        const json& e = jget(u, "error");
        std::string msg = jstr(e, "message");
        if (msg.empty()) msg = jstr(u, "error_message");
        onSendFailed(jint(u, "old_message_id"), msg);
    } else if (t == "updateNewMessage") {
        // Nuovo file nella chat aperta (es. caricato da un altro dispositivo)
        const json& m = jget(u, "message");
        if (jint(m, "chat_id") == selChat_ && query_.empty() && !jget(m, "sending_state").is_object()) {
            std::vector<FileEntry> v;
            addMessage(m, v);
            for (auto& e : v) mergeEntry(e);
            if (!v.empty()) {
                std::sort(entries_.begin(), entries_.end(),
                          [](const FileEntry& a, const FileEntry& b) { return a.msgId > b.msgId; });
                entryIndex_.clear();
                for (size_t i = 0; i < entries_.size(); ++i) entryIndex_[entries_[i].key] = i;
                changed();
            }
        }
    }
}

void Engine::onAuthState(const json& st) {
    const std::string t = jtype(st);
    auth_.busy = false;
    if (t == "authorizationStateWaitTdlibParameters") {
        sendParams();
    } else if (t == "authorizationStateWaitPhoneNumber") {
        auth_.state = AuthState::WaitPhone;
        auth_.error.clear();
    } else if (t == "authorizationStateWaitOtherDeviceConfirmation") {
        auth_.state = AuthState::WaitQr;
        auth_.qrLink = jstr(st, "link");
    } else if (t == "authorizationStateWaitCode") {
        auth_.state = AuthState::WaitCode;
        std::string ct = jtype(jget(jget(st, "code_info"), "type"));
        if (ct == "authenticationCodeTypeTelegramMessage")
            auth_.codeInfo = "Il codice è stato inviato nell'app Telegram sul tuo telefono.";
        else if (ct == "authenticationCodeTypeSms" || ct == "authenticationCodeTypeSmsWord" ||
                 ct == "authenticationCodeTypeSmsPhrase")
            auth_.codeInfo = "Il codice è stato inviato via SMS.";
        else if (ct == "authenticationCodeTypeCall")
            auth_.codeInfo = "Riceverai una chiamata con il codice.";
        else if (ct == "authenticationCodeTypeFragment")
            auth_.codeInfo = "Il codice è stato inviato su Fragment.";
        else
            auth_.codeInfo = "Inserisci il codice ricevuto.";
    } else if (t == "authorizationStateWaitPassword") {
        auth_.state = AuthState::WaitPassword;
        auth_.passwordHint = jstr(st, "password_hint");
    } else if (t == "authorizationStateReady") {
        auth_.state = AuthState::Ready;
        auth_.error.clear();
        afterReady();
    } else if (t == "authorizationStateLoggingOut") {
        auth_.state = AuthState::LoggingOut;
    } else if (t == "authorizationStateClosed") {
        recreateClient_ = true;
        if (!shuttingDown_) {
            chatMap_.clear();
            chatOrder_.clear();
            entries_.clear();
            entryIndex_.clear();
            profile_ = Profile();
            selChat_ = savedChatId_ = 0;
            fileToTransfer_.clear();
            sendingMsg_.clear();
            if (auth_.state == AuthState::LoggingOut) {
                // nuovo account: la coda del precedente non ha più senso
                transfers_.erase(std::remove_if(transfers_.begin(), transfers_.end(),
                                                [](const std::unique_ptr<Transfer>& x) { return !x->workerBusy; }),
                                 transfers_.end());
                queueDirty_ = true;
            }
        }
        auth_.state = AuthState::Starting;
    } else if (t == "authorizationStateWaitRegistration" || t == "authorizationStateWaitEmailAddress" ||
               t == "authorizationStateWaitEmailCode") {
        auth_.state = AuthState::Error;
        auth_.error = "Questo numero richiede la registrazione o la verifica email: completala con "
                      "l'app ufficiale di Telegram e poi riprova.";
    }
    changed();
}

void Engine::sendParams() {
    std::string db = joinPath(cfg_.dataDir, "td");
    std::string files = joinPath(cfg_.dataDir, "files");
    makeDirs(db);
    makeDirs(files);
    td_->send({{"@type", "setTdlibParameters"},
               {"use_test_dc", false},
               {"database_directory", db},
               {"files_directory", files},
               {"database_encryption_key", ""},
               {"use_file_database", true},
               {"use_chat_info_database", true},
               {"use_message_database", true},
               {"use_secret_chats", false},
               {"api_id", cfg_.apiId},
               {"api_hash", cfg_.apiHash},
               {"system_language_code", cfg_.language},
               {"device_model", cfg_.deviceModel},
               {"system_version", cfg_.systemVersion},
               {"application_version", cfg_.appVersion}},
              [this](const json& r) {
                  if (TdClient::isError(r)) {
                      std::lock_guard<std::recursive_mutex> g(mu_);
                      auth_.state = AuthState::Error;
                      auth_.error = "Parametri TDLib rifiutati: " + jstr(r, "message") +
                                    ". Controlla api_id e api_hash.";
                      changed();
                  }
              });
}

// ---------------------------------------------------------------- accesso

AuthInfo Engine::auth() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    return auth_;
}

static std::string authError(const json& r) {
    std::string m = jstr(r, "message");
    if (m == "PHONE_NUMBER_INVALID") return "Numero di telefono non valido. Usa il formato internazionale, es. +39...";
    if (m == "PHONE_CODE_INVALID") return "Codice errato.";
    if (m == "PHONE_CODE_EXPIRED") return "Il codice è scaduto: richiedine uno nuovo.";
    if (m == "PASSWORD_HASH_INVALID") return "Password errata.";
    if (m.rfind("Too Many Requests", 0) == 0 || m.rfind("FLOOD_WAIT", 0) == 0)
        return "Troppi tentativi: attendi qualche minuto e riprova.";
    return m.empty() ? "Errore sconosciuto" : m;
}

void Engine::submitPhone(const std::string& phone) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auth_.busy = true;
    auth_.error.clear();
    td_->send({{"@type", "setAuthenticationPhoneNumber"}, {"phone_number", phone}}, [this](const json& r) {
        std::lock_guard<std::recursive_mutex> lk1(mu_);
        auth_.busy = false;
        if (TdClient::isError(r)) auth_.error = authError(r);
        changed();
    });
    changed();
}

void Engine::submitCode(const std::string& code) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auth_.busy = true;
    auth_.error.clear();
    td_->send({{"@type", "checkAuthenticationCode"}, {"code", code}}, [this](const json& r) {
        std::lock_guard<std::recursive_mutex> lk1(mu_);
        auth_.busy = false;
        if (TdClient::isError(r)) auth_.error = authError(r);
        changed();
    });
    changed();
}

void Engine::submitPassword(const std::string& pwd) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auth_.busy = true;
    auth_.error.clear();
    td_->send({{"@type", "checkAuthenticationPassword"}, {"password", pwd}}, [this](const json& r) {
        std::lock_guard<std::recursive_mutex> lk1(mu_);
        auth_.busy = false;
        if (TdClient::isError(r)) auth_.error = authError(r);
        changed();
    });
    changed();
}

void Engine::requestQr() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auth_.busy = true;
    auth_.error.clear();
    td_->send({{"@type", "requestQrCodeAuthentication"}, {"other_user_ids", json::array()}}, [this](const json& r) {
        std::lock_guard<std::recursive_mutex> lk1(mu_);
        auth_.busy = false;
        if (TdClient::isError(r)) auth_.error = authError(r);
        changed();
    });
    changed();
}

void Engine::usePhoneLogin() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    // TDLib non permette di passare dal QR al numero: si chiude la sessione non
    // autorizzata e se ne apre una nuova, che chiederà il numero.
    if (auth_.state == AuthState::WaitQr) {
        auth_.state = AuthState::LoggingOut;
        auth_.qrLink.clear();
        td_->send({{"@type", "logOut"}});
    }
    changed();
}

void Engine::logout() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    for (auto& t : transfers_) {
        if (t->st == TStatus::Active) {
            if (t->dir == Direction::Download && t->curFileId)
                td_->send({{"@type", "cancelDownloadFile"}, {"file_id", t->curFileId}, {"only_if_pending", false}});
            if (t->dir == Direction::Upload && t->sending) stopPendingUpload(*t);
        }
        t->abort->store(true);
    }
    auth_.state = AuthState::LoggingOut;
    td_->send({{"@type", "logOut"}});
    changed();
}

// ---------------------------------------------------------------- profilo

void Engine::onUser(const json& u) {
    if (jint(u, "id") != profile_.userId || !profile_.userId) return;
    profile_.firstName = jstr(u, "first_name");
    profile_.lastName = jstr(u, "last_name");
    profile_.phone = jstr(u, "phone_number");
    const json& un = jget(jget(u, "usernames"), "active_usernames");
    profile_.username = (un.is_array() && !un.empty() && un[0].is_string()) ? un[0].get<std::string>()
                                                                            : jstr(u, "username");
    bool wasPremium = profile_.premium;
    profile_.premium = jbool(u, "is_premium");
    profile_.valid = true;
    const json& small = jget(jget(u, "profile_photo"), "small");
    if (small.is_object()) {
        int32_t fid = (int32_t)jint(small, "id");
        const json& loc = jget(small, "local");
        if (jbool(loc, "is_downloading_completed")) {
            profile_.photoPath = jstr(loc, "path");
        } else if (fid && !photoFiles_.count(fid)) {
            photoFiles_.insert(fid);
            td_->send({{"@type", "downloadFile"}, {"file_id", fid}, {"priority", 1},
                       {"offset", 0}, {"limit", 0}, {"synchronous", false}});
        }
    }
    (void)wasPremium;  // il limite di divisione si legge da uploadLimit() al momento dell'upload
    changed();
}

Profile Engine::profile() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    return profile_;
}

int64_t Engine::uploadLimit() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (testLimit_) return testLimit_;
    return profile_.premium ? kLimitPremium : kLimitFree;
}

void Engine::afterReady() {
    td_->send({{"@type", "getMe"}}, [this](const json& u) {
        if (TdClient::isError(u)) return;
        std::lock_guard<std::recursive_mutex> g(mu_);
        profile_.userId = jint(u, "id");
        onUser(u);
        td_->send({{"@type", "createPrivateChat"}, {"user_id", profile_.userId}, {"force", false}},
                  [this](const json& c) {
                      if (TdClient::isError(c)) return;
                      std::lock_guard<std::recursive_mutex> lk1(mu_);
                      savedChatId_ = jint(c, "id");
                      auto it = chatMap_.find(savedChatId_);
                      if (it != chatMap_.end()) it->second.kind = ChatKind::Saved;
                      if (!selChat_) selectChat(savedChatId_);
                      if (!restored_) {
                          restored_ = true;
                          restoreTransfers();
                      }
                      changed();
                  });
    });
    loadChatList();
}

void Engine::loadChatList() {
    td_->send({{"@type", "loadChats"}, {"chat_list", {{"@type", "chatListMain"}}}, {"limit", 200}},
              [this](const json&) {
                  std::lock_guard<std::recursive_mutex> g(mu_);
                  td_->send({{"@type", "getChats"}, {"chat_list", {{"@type", "chatListMain"}}}, {"limit", 500}},
                            [this](const json& r) {
                                std::lock_guard<std::recursive_mutex> lk1(mu_);
                                const json& ids = jget(r, "chat_ids");
                                if (!ids.is_array()) return;
                                std::vector<int64_t> order;
                                for (auto& v : ids) order.push_back(v.is_string() ? std::stoll(v.get<std::string>()) : v.get<int64_t>());
                                for (int64_t id : chatOrder_)
                                    if (std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
                                chatOrder_ = order;
                                changed();
                            });
              });
}

// ---------------------------------------------------------------- chat ed elenco

std::vector<Chat> Engine::chats() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::vector<Chat> v;
    auto it = chatMap_.find(savedChatId_);
    if (savedChatId_ && it != chatMap_.end()) {
        Chat c = it->second;
        c.kind = ChatKind::Saved;
        c.title = "Messaggi salvati";
        v.push_back(c);
    }
    for (int64_t id : chatOrder_) {
        if (id == savedChatId_) continue;
        auto ci = chatMap_.find(id);
        if (ci != chatMap_.end()) v.push_back(ci->second);
    }
    return v;
}

int64_t Engine::selectedChat() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    return selChat_;
}

void Engine::selectChat(int64_t chatId) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (selChat_ == chatId && !entries_.empty()) return;
    selChat_ = chatId;
    // apre la chat per TDLib (aggiorna i messaggi in tempo reale)
    td_->send({{"@type", "openChat"}, {"chat_id", chatId}});
    reloadList();
}

void Engine::setFilter(Filter f) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (filter_ == f) return;
    filter_ = f;
    reloadList();
}

void Engine::setQuery(const std::string& q) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (query_ == q) return;
    query_ = q;
    reloadList();
}

Filter Engine::filter() { std::lock_guard<std::recursive_mutex> g(mu_); return filter_; }
std::string Engine::query() { std::lock_guard<std::recursive_mutex> g(mu_); return query_; }
std::vector<FileEntry> Engine::entries() { std::lock_guard<std::recursive_mutex> g(mu_); return entries_; }
int64_t Engine::totalCount() { std::lock_guard<std::recursive_mutex> g(mu_); return totalCount_; }
bool Engine::listLoading() { std::lock_guard<std::recursive_mutex> g(mu_); return listLoading_; }
bool Engine::listHasMore() { std::lock_guard<std::recursive_mutex> g(mu_); return listMore_; }

static const char* filterType(Filter f) {
    switch (f) {
        case Filter::Documents: return "searchMessagesFilterDocument";
        case Filter::Video: return "searchMessagesFilterVideo";
        case Filter::Audio: return "searchMessagesFilterAudio";
        case Filter::Photos: return "searchMessagesFilterPhoto";
        default: return "searchMessagesFilterEmpty";
    }
}

void Engine::reloadList() {
    ++listGen_;
    entries_.clear();
    entryIndex_.clear();
    nextFromMsg_ = 0;
    listMore_ = false;
    totalCount_ = -1;
    if (!selChat_ || !td_) { changed(); return; }
    requestPage();
    fetchCount();
    changed();
}

void Engine::loadMore() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (listLoading_ || !listMore_) return;
    requestPage();
}

void Engine::requestPage() {
    listLoading_ = true;
    uint64_t gen = listGen_;
    // Solo i campi stabili tra le versioni di TDLib: quelli mancanti prendono il valore predefinito.
    td_->send({{"@type", "searchChatMessages"},
               {"chat_id", selChat_},
               {"query", query_},
               {"from_message_id", nextFromMsg_},
               {"offset", 0},
               {"limit", 100},
               {"filter", {{"@type", filterType(filter_)}}}},
              [this, gen](const json& r) { handlePage(gen, r); });
}

void Engine::handlePage(uint64_t gen, const json& r) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (gen != listGen_) return;  // risposta di una ricerca superata
    listLoading_ = false;
    if (TdClient::isError(r)) {
        listMore_ = false;
        changed();
        return;
    }
    const json& msgs = jget(r, "messages");
    size_t before = entries_.size();
    int64_t lastId = 0;
    if (msgs.is_array()) {
        for (auto& m : msgs) {
            std::vector<FileEntry> v;
            addMessage(m, v);
            for (auto& e : v) mergeEntry(e);
            lastId = jint(m, "id");
        }
    }
    int64_t next = jint(r, "next_from_message_id");
    if (!r.contains("next_from_message_id")) next = lastId;  // versioni vecchie
    listMore_ = msgs.is_array() && !msgs.empty() && next != 0;
    nextFromMsg_ = next;
    // Con "Tutti" molte righe sono testo: se la pagina ha portato pochi file, continua.
    if (listMore_ && entries_.size() - before < 20 && entries_.size() < 60) requestPage();
    changed();
}

void Engine::fetchCount() {
    uint64_t gen = listGen_;
    if (!query_.empty()) return;  // con una ricerca il totale lo dà la paginazione
    std::vector<Filter> fs;
    if (filter_ == Filter::All) fs = {Filter::Documents, Filter::Video, Filter::Audio, Filter::Photos};
    else fs = {filter_};
    auto acc = std::make_shared<std::pair<int, int64_t>>(0, 0);
    int n = (int)fs.size();
    for (Filter f : fs) {
        td_->send({{"@type", "getChatMessageCount"}, {"chat_id", selChat_},
                   {"filter", {{"@type", filterType(f)}}}, {"return_local", false}},
                  [this, gen, acc, n](const json& r) {
                      std::lock_guard<std::recursive_mutex> g(mu_);
                      if (gen != listGen_) return;
                      acc->first++;
                      if (!TdClient::isError(r)) acc->second += jint(r, "count");
                      if (acc->first == n) { totalCount_ = acc->second; changed(); }
                  });
    }
}

void Engine::addMessage(const json& m, std::vector<FileEntry>& out) {
    const json& c = jget(m, "content");
    const std::string ct = jtype(c);
    FileEntry e;
    e.chatId = jint(m, "chat_id");
    e.msgId = jint(m, "id");
    e.date = jint(m, "date");
    const json* file = nullptr;
    std::string name;
    if (ct == "messageDocument") {
        const json& d = jget(c, "document");
        name = jstr(d, "file_name");
        file = &jget(d, "document");
        e.kind = FileKind::Document;
    } else if (ct == "messageVideo") {
        const json& d = jget(c, "video");
        name = jstr(d, "file_name");
        file = &jget(d, "video");
        e.kind = FileKind::Video;
    } else if (ct == "messageAnimation") {
        const json& d = jget(c, "animation");
        name = jstr(d, "file_name");
        file = &jget(d, "animation");
        e.kind = FileKind::Video;
    } else if (ct == "messageAudio") {
        const json& d = jget(c, "audio");
        name = jstr(d, "file_name");
        if (name.empty()) name = jstr(d, "title") + ".mp3";
        file = &jget(d, "audio");
        e.kind = FileKind::Audio;
    } else if (ct == "messagePhoto") {
        const json& sizes = jget(jget(c, "photo"), "sizes");
        if (sizes.is_array() && !sizes.empty()) file = &jget(sizes.back(), "photo");
        char buf[64];
        std::snprintf(buf, sizeof buf, "foto_%lld.jpg", (long long)e.msgId);
        name = buf;
        e.kind = FileKind::Photo;
    } else {
        return;
    }
    if (!file || !file->is_object()) return;
    e.fileId = (int32_t)jint(*file, "id");
    e.size = jint(*file, "size");
    if (!e.size) e.size = jint(*file, "expected_size");
    if (name.empty()) name = "file_" + std::to_string(e.msgId);
    e.name = sanitizeFileName(name);

    SplitTag tag;
    std::string caption = jstr(jget(c, "caption"), "text");
    e.folder = folderFromCaption(caption);
    if (!caption.empty() && parseCaption(caption, tag)) {
        PartRef p;
        p.index = tag.index;
        p.msgId = e.msgId;
        p.fileId = e.fileId;
        p.size = e.size;
        p.sha = tag.partSha;
        e.isGroup = true;
        e.gid = tag.gid;
        e.fullSha = tag.fullSha;
        e.partCount = tag.count;
        e.size = tag.total;
        // nome originale: dalla didascalia (affidabile anche se Telegram cambia il nome del file),
        // altrimenti dal nome della parte senza ".001"
        std::string orig = tag.name.empty() ? originalNameFromPart(name) : tag.name;
        if (!tag.name.empty() && lacksExtension(tag.name) && !lacksExtension(originalNameFromPart(name)))
            orig = originalNameFromPart(name);
        e.name = sanitizeFileName(orig);
        e.fileId = 0;
        e.parts[p.index] = p;
        e.key = std::to_string(e.chatId) + ":g" + e.gid;
    } else {
        e.key = std::to_string(e.chatId) + ":" + std::to_string(e.msgId);
    }
    out.push_back(std::move(e));
}

void Engine::mergeEntry(const FileEntry& e) {
    auto it = entryIndex_.find(e.key);
    if (it == entryIndex_.end()) {
        entryIndex_[e.key] = entries_.size();
        entries_.push_back(e);
        (void)alreadyDownloaded(e);  // precalcola "già scaricato"
        return;
    }
    FileEntry& x = entries_[it->second];
    if (x.isGroup && e.isGroup) {
        for (auto& p : e.parts) x.parts[p.first] = p.second;
        if (x.fullSha.empty()) x.fullSha = e.fullSha;
        if (e.msgId > x.msgId) { x.msgId = e.msgId; x.date = e.date; }
    }
}

// ---------------------------------------------------------------- trasferimenti: API

bool Engine::anySending() {
    for (auto& t : transfers_)
        if (t->sending && !t->sendingMsgId) return true;
    return false;
}

Engine::Transfer* Engine::find(int id) {
    for (auto& t : transfers_)
        if (t->id == id) return t.get();
    return nullptr;
}

std::vector<TransferView> Engine::transfers() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::vector<TransferView> v;
    for (auto& t : transfers_) {
        TransferView x;
        x.id = t->id;
        x.dir = t->dir;
        x.status = t->st;
        if (t->pauseRequested && t->st != TStatus::Done && t->st != TStatus::Error) x.status = TStatus::Paused;
        x.name = t->name;
        x.chatTitle = t->chatTitle;
        x.entryKey = t->entryKey;
        x.error = t->error;
        x.total = t->total;
        x.done = t->done;
        x.speed = t->speed;
        x.path = t->dir == Direction::Download ? t->destPath : t->srcPath;
        if (t->prepTotal > 0) x.prep = (double)t->prepDone / (double)t->prepTotal;
        if (t->dir == Direction::Download && t->entry.isGroup) {
            x.part = t->curPart + 1;
            x.partCount = t->entry.partCount;
        } else if (t->dir == Direction::Upload && t->split) {
            x.part = t->partIdx + 1;
            x.partCount = t->partCount;
        }
        v.push_back(x);
    }
    return v;
}

int Engine::transferForEntry(const std::string& key) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    for (auto it = transfers_.rbegin(); it != transfers_.rend(); ++it)
        if ((*it)->entryKey == key && (*it)->dir == Direction::Download) return (*it)->id;
    return -1;
}

bool Engine::alreadyDownloaded(const FileEntry& e) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = dlCache_.find(e.key);
    if (it != dlCache_.end()) return it->second;
    std::string p = entryPath(e);
    bool ok = fileSize(p) == e.size && e.size > 0;
    dlCache_[e.key] = ok;
    return ok;
}

void Engine::totalSpeed(double& down, double& up) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    down = up = 0;
    for (auto& t : transfers_) (t->dir == Direction::Download ? down : up) += t->speed;
}

static std::string chatTitleFor(const std::map<int64_t, Chat>& m, int64_t id, int64_t saved) {
    if (id == saved) return "Messaggi salvati";
    auto it = m.find(id);
    return it == m.end() ? std::string() : it->second.title;
}

int Engine::download(const std::string& entryKey) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = entryIndex_.find(entryKey);
    if (it == entryIndex_.end()) return -1;
    // già in coda o attivo?
    for (auto& t : transfers_)
        if (t->entryKey == entryKey && t->dir == Direction::Download && t->st != TStatus::Done &&
            t->st != TStatus::Error)
            return t->id;
    auto t = std::make_unique<Transfer>();
    t->id = nextTid_++;
    t->dir = Direction::Download;
    t->entry = entries_[it->second];
    t->entryKey = entryKey;
    t->name = t->entry.name;
    t->chatId = t->entry.chatId;
    t->chatTitle = chatTitleFor(chatMap_, t->chatId, savedChatId_);
    t->total = t->entry.size;
    int id = t->id;
    transfers_.push_back(std::move(t));
    queueDirty_ = true;
    schedule();
    changed();
    return id;
}

int Engine::upload(int64_t chatId, const std::string& path, const std::string& folder) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    int64_t sz = fileSize(path);
    if (sz <= 0) return -1;
    auto t = std::make_unique<Transfer>();
    t->id = nextTid_++;
    t->dir = Direction::Upload;
    t->srcPath = path;
    t->folder = cleanFolder(folder);
    t->name = fileNameOf(path);
    t->chatId = chatId;
    t->chatTitle = chatTitleFor(chatMap_, chatId, savedChatId_);
    t->total = sz;
    int id = t->id;
    transfers_.push_back(std::move(t));
    queueDirty_ = true;
    schedule();
    changed();
    return id;
}

void Engine::pause(int id) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    Transfer* t = find(id);
    if (!t) return;
    if (t->st == TStatus::Queued) {
        t->st = TStatus::Paused;
    } else if (t->st == TStatus::Active && !t->workerBusy) {
        if (t->dir == Direction::Download && t->curFileId) {
            td_->send({{"@type", "cancelDownloadFile"}, {"file_id", t->curFileId}, {"only_if_pending", false}});
        } else if (t->dir == Direction::Upload) {
            // ritira il messaggio non ancora inviato: TDLib interrompe il caricamento e, alla
            // ripresa, riparte dalle parti già caricate dello stesso file
            stopPendingUpload(*t);
            t->done = t->baseDone;
        }
        t->st = TStatus::Paused;
    } else if (t->active()) {
        t->pauseRequested = true;  // al termine del passo in corso
    }
    queueDirty_ = true;
    schedule();
    changed();
}

void Engine::resume(int id) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    Transfer* t = find(id);
    if (!t) return;
    t->pauseRequested = false;
    if (t->st == TStatus::Paused || t->st == TStatus::Error) {
        t->st = TStatus::Queued;
        t->error.clear();
    }
    queueDirty_ = true;
    schedule();
    changed();
}

void Engine::cancel(int id) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    Transfer* t = find(id);
    if (!t) return;
    t->abort->store(true);
    if (t->dir == Direction::Download) {
        if (t->curFileId && t->st != TStatus::Done) {
            td_->send({{"@type", "cancelDownloadFile"}, {"file_id", t->curFileId}, {"only_if_pending", false}});
            td_->send({{"@type", "deleteFile"}, {"file_id", t->curFileId}});
            fileToTransfer_.erase(t->curFileId);
        }
        if (!t->workerBusy && !t->partialPath.empty() && t->st != TStatus::Done) removeFile(t->partialPath);
    } else {
        if (t->sending && t->st != TStatus::Done) stopPendingUpload(*t);
        if (!t->workerBusy && !t->tempPart.empty() && !t->stopWhenKnown) removeFile(t->tempPart);
    }
    if (t->workerBusy || t->stopWhenKnown) {
        t->cancelRequested = true;  // rimosso quando il passo in corso termina
    } else {
        transfers_.erase(std::remove_if(transfers_.begin(), transfers_.end(),
                                        [id](const std::unique_ptr<Transfer>& x) { return x->id == id; }),
                         transfers_.end());
    }
    queueDirty_ = true;
    schedule();
    changed();
}

void Engine::pauseAll() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::vector<int> ids;
    for (auto& t : transfers_)
        if (t->st != TStatus::Done && t->st != TStatus::Error && t->st != TStatus::Paused) ids.push_back(t->id);
    // prima i non attivi, così lo scheduler non ne avvia altri nel frattempo
    for (int id : ids) if (!find(id)->active()) pause(id);
    for (int id : ids) if (find(id) && find(id)->active()) pause(id);
}

void Engine::resumeAll() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::vector<int> ids;
    for (auto& t : transfers_)
        if (t->st == TStatus::Paused || t->pauseRequested) ids.push_back(t->id);
    for (int id : ids) resume(id);
}

int Engine::cancelAll() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    std::vector<int> ids;
    for (auto& t : transfers_)
        if (t->st != TStatus::Done) ids.push_back(t->id);
    // prima quelli in coda, così lo scheduler non avvia nulla mentre si annullano gli attivi
    for (int id : ids) if (Transfer* t = find(id)) if (!t->active()) cancel(id);
    for (int id : ids) if (find(id)) cancel(id);
    return (int)ids.size();
}

void Engine::clearCompleted() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    transfers_.erase(std::remove_if(transfers_.begin(), transfers_.end(),
                                    [](const std::unique_ptr<Transfer>& x) { return x->st == TStatus::Done; }),
                     transfers_.end());
    changed();
}

Settings Engine::settings() {
    std::lock_guard<std::recursive_mutex> g(mu_);
    return settings_;
}

void Engine::setSettings(const Settings& s) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (s.downloadDir != settings_.downloadDir) dlCache_.clear();
    settings_ = s;
    settings_.maxParallel = std::max(1, std::min(8, settings_.maxParallel));
    saveSettings();
    schedule();
    changed();
}

// ---------------------------------------------------------------- scheduler

void Engine::schedule() {
    if (shuttingDown_ || auth_.state != AuthState::Ready || !td_) return;
    int active = 0;
    for (auto& t : transfers_)
        if (t->active()) ++active;
    for (auto& t : transfers_) {
        if (active >= settings_.maxParallel) break;
        if (t->st == TStatus::Queued) {
            ++active;
            startTransfer(*t);
        }
    }
}

void Engine::log(const std::string& line) {
    std::lock_guard<std::mutex> lk(logMu_);
    std::string path = joinPath(cfg_.dataDir, "telefiles.log");
    if (fileSize(path) > 2 * 1024 * 1024) moveFile(path, joinPath(cfg_.dataDir, "telefiles.old.log"));
    OutFile o;
    if (!o.open(path, true)) return;
    std::time_t now = std::time(nullptr);
    char ts[32];
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S ", &lt);
    std::string l = ts + line + "\n";
    o.write(l.data(), (int64_t)l.size());
    o.close();
}

void Engine::fail(Transfer& t, const std::string& err) {
    log("errore [" + t.name + "]: " + err);
    t.st = TStatus::Error;
    t.error = err;
    t.speed = 0;
    if (t.curFileId) fileToTransfer_.erase(t.curFileId);
    if (t.upFileId) fileToTransfer_.erase(t.upFileId);
    queueDirty_ = true;
    schedule();
    changed();
}

void Engine::runWorker(int tid, std::function<void(Transfer&, std::function<bool(int64_t)>)> work) {
    Transfer* t = find(tid);
    if (!t) return;
    t->workerBusy = true;
    workers_++;
    auto abort = t->abort;
    std::thread([this, t, tid, work, abort] {
        // Il lavoro gira senza lock; 'progress' aggiorna l'avanzamento sotto lock.
        auto progress = [this, t, abort](int64_t v) {
            std::lock_guard<std::recursive_mutex> g(mu_);
            t->done = v;
            return !abort->load();
        };
        work(*t, progress);
        workers_--;
        (void)tid;
    }).detach();
}

void Engine::startTransfer(Transfer& t) {
    t.error.clear();
    t.abort->store(false);
    if (t.dir == Direction::Download) {
        if (t.entry.isGroup && !t.partsCollected) {
            t.st = TStatus::Preparing;
            collectGroupParts(t);
        } else {
            startDownloadPart(t);
        }
    } else {
        if (!t.prepared) {
            t.st = TStatus::Preparing;
            int64_t limit = uploadLimit();
            int tid = t.id;
            runWorker(tid, [this, tid, limit](Transfer& tr, std::function<bool(int64_t)>) {
                // L'impronta del file intero viene calcolata mentre si estraggono le parti (va solo
                // nella didascalia dell'ultima): niente lettura preliminare di tutto il file.
                int64_t sz = fileSize(tr.srcPath);
                std::string err;
                bool split = sz > limit;
                if (sz <= 0) err = "File non trovato o vuoto";
                std::lock_guard<std::recursive_mutex> g(mu_);
                tr.workerBusy = false;
                if (tr.cancelRequested) { cancel(tid); return; }
                tr.done = 0;
                tr.lastDone = 0;
                if (!err.empty()) {
                    if (tr.pauseRequested) { tr.pauseRequested = false; tr.st = TStatus::Paused; schedule(); changed(); }
                    else fail(tr, err);
                    return;
                }
                tr.total = sz;
                tr.split = split;
                tr.partSize = split ? (limit > 64 * kMiB ? limit - kPartMargin : limit) : sz;
                tr.partCount = split ? partCountFor(sz, tr.partSize) : 1;
                tr.gid = split ? randomGroupId() : "";
                tr.fullSha.clear();
                tr.upRunning.reset();
                tr.upRunningBytes = 0;
                tr.upRunningValid = true;
                tr.partIdx = 0;
                tr.baseDone = 0;
                tr.prepared = true;
                queueDirty_ = true;
                log("upload " + tr.name + ": " + std::to_string(sz) + " byte, " +
                    (split ? std::to_string(tr.partCount) + " parti da " + std::to_string(tr.partSize) : std::string("intero")));
                if (tr.pauseRequested) { tr.pauseRequested = false; tr.st = TStatus::Paused; schedule(); changed(); return; }
                startUploadStep(tr);
                changed();
            });
        } else if (t.split && !t.tempPart.empty() && fileSize(t.tempPart) == t.partLen(t.partIdx)) {
            uploadPath(t, t.tempPart);  // ripresa della stessa parte
        } else {
            startUploadStep(t);
        }
    }
    changed();
}

// ---------------------------------------------------------------- download

void Engine::collectGroupParts(Transfer& t) {
    int tid = t.id;
    td_->send({{"@type", "searchChatMessages"},
               {"chat_id", t.chatId},
               {"query", "#tf" + t.entry.gid},
               {"from_message_id", 0},
               {"offset", 0},
               {"limit", 100},
               {"filter", {{"@type", "searchMessagesFilterEmpty"}}}},
              [this, tid](const json& r) {
                  std::lock_guard<std::recursive_mutex> g(mu_);
                  Transfer* tx = find(tid);
                  if (!tx || tx->st != TStatus::Preparing) return;
                  if (TdClient::isError(r)) { fail(*tx, "Ricerca delle parti fallita: " + jstr(r, "message")); return; }
                  const json& msgs = jget(r, "messages");
                  if (msgs.is_array())
                      for (auto& m : msgs) {
                          std::vector<FileEntry> v;
                          addMessage(m, v);
                          for (auto& e : v)
                              if (e.isGroup && e.gid == tx->entry.gid) {
                                  for (auto& p : e.parts) tx->entry.parts[p.first] = p.second;
                                  if (tx->entry.fullSha.empty()) tx->entry.fullSha = e.fullSha;
                              }
                      }
                  int missing = 0;
                  for (int i = 1; i <= tx->entry.partCount; ++i)
                      if (!tx->entry.parts.count(i)) ++missing;
                  if (missing) {
                      fail(*tx, (missing == 1 ? std::string("Manca 1 parte su ")
                                             : "Mancano " + std::to_string(missing) + " parti su ") +
                                   std::to_string(tx->entry.partCount) + " nella chat");
                      return;
                  }
                  tx->parts.clear();
                  for (auto& p : tx->entry.parts) tx->parts.push_back(p.second);
                  tx->partsCollected = true;
                  if (tx->pauseRequested) { tx->pauseRequested = false; tx->st = TStatus::Paused; schedule(); changed(); return; }
                  startDownloadPart(*tx);
              });
}

void Engine::startDownloadPart(Transfer& t) {
    const bool group = t.entry.isGroup;
    if (t.destPath.empty()) {
        std::string dir = t.entry.folder.empty() ? settings_.downloadDir
                                                 : joinPath(settings_.downloadDir, t.entry.folder);
        makeDirs(dir);
        t.destPath = uniquePath(dir, t.name);
        if (group) {
            t.partialPath = t.destPath + ".tfpart";
            removeFile(t.partialPath);
            t.running.reset();
            t.hashValid = true;
            t.curPart = 0;
            t.baseDone = 0;
        }
        queueDirty_ = true;
    }
    if (group && !t.hashValid) {
        // ripresa dopo un riavvio: riallinea il file parziale e ricalcola l'hash progressivo
        t.st = TStatus::Verifying;
        int tid = t.id;
        runWorker(tid, [this, tid](Transfer& tr, std::function<bool(int64_t)> progress) {
            std::error_code ec;
            if (fileExists(tr.partialPath)) fs::resize_file(u8p(tr.partialPath), (uintmax_t)tr.baseDone, ec);
            bool ok = !ec;
            Sha256 h;
            if (ok && tr.baseDone > 0) {
                InFile in;
                ok = in.open(tr.partialPath);
                std::string buf(4 * kMiB, '\0');
                int64_t done = 0;
                while (ok) {
                    int64_t n = in.read(buf.data(), (int64_t)buf.size());
                    if (n < 0) ok = false;
                    if (n <= 0) break;
                    h.update(buf.data(), (size_t)n);
                    done += n;
                    if (!progress(done)) ok = false;
                }
                ok = ok && done == tr.baseDone;
            }
            std::lock_guard<std::recursive_mutex> g(mu_);
            tr.workerBusy = false;
            if (tr.cancelRequested) { cancel(tid); return; }
            if (!ok) {
                // il parziale non è utilizzabile: si riparte dalla prima parte
                removeFile(tr.partialPath);
                tr.curPart = 0;
                tr.baseDone = 0;
                h.reset();
            }
            tr.running = h;
            tr.hashValid = true;
            tr.done = tr.baseDone;
            if (tr.pauseRequested) { tr.pauseRequested = false; tr.st = TStatus::Paused; schedule(); changed(); return; }
            startDownloadPart(tr);
        });
        return;
    }
    int32_t fid = group ? t.parts[t.curPart].fileId : t.entry.fileId;
    if (!fid) { fail(t, "File non disponibile: riapri la chat e riprova"); return; }
    t.curFileId = fid;
    fileToTransfer_[fid] = t.id;
    t.st = TStatus::Active;
    int tid = t.id;
    td_->send({{"@type", "downloadFile"}, {"file_id", fid}, {"priority", 16},
               {"offset", 0}, {"limit", 0}, {"synchronous", false}},
              [this, tid](const json& r) {
                  std::lock_guard<std::recursive_mutex> g(mu_);
                  Transfer* tx = find(tid);
                  if (!tx) return;
                  if (TdClient::isError(r)) {
                      if (tx->st == TStatus::Active) fail(*tx, "Download rifiutato: " + jstr(r, "message"));
                      return;
                  }
                  onFile(r);  // il file potrebbe essere già completo in cache
              });
}

void Engine::onFile(const json& f) {
    int32_t fid = (int32_t)jint(f, "id");
    if (photoFiles_.count(fid)) {
        const json& loc = jget(f, "local");
        if (jbool(loc, "is_downloading_completed")) {
            profile_.photoPath = jstr(loc, "path");
            changed();
        }
        return;
    }
    auto it = fileToTransfer_.find(fid);
    if (it == fileToTransfer_.end()) return;
    Transfer* t = find(it->second);
    if (!t) { fileToTransfer_.erase(it); return; }
    if (t->dir == Direction::Download) {
        if (t->st != TStatus::Active || t->curFileId != fid) return;
        const json& loc = jget(f, "local");
        int64_t got = jint(loc, "downloaded_prefix_size");
        if (!got) got = jint(loc, "downloaded_size");
        t->done = t->baseDone + got;
        if (jbool(loc, "is_downloading_completed")) {
            std::string path = jstr(loc, "path");
            fileToTransfer_.erase(fid);
            onDownloadPartDone(*t, path);
        }
    } else {
        if (t->upFileId != fid || t->st != TStatus::Active) return;
        const json& rem = jget(f, "remote");
        int64_t up = std::min(jint(rem, "uploaded_size"), t->partLen(t->partIdx));
        t->done = t->baseDone + up;
        // la fine non si deduce da qui: arriva con updateMessageSendSucceeded
    }
    changed();
}

void Engine::onDownloadPartDone(Transfer& t, const std::string& localPath) {
    int tid = t.id;
    if (!t.entry.isGroup) {
        t.st = TStatus::Joining;  // sposta nella cartella di destinazione
        runWorker(tid, [this, tid, localPath](Transfer& tr, std::function<bool(int64_t)>) {
            bool ok = moveFile(localPath, tr.destPath);
            std::lock_guard<std::recursive_mutex> g(mu_);
            tr.workerBusy = false;
            if (!ok) { fail(tr, "Impossibile salvare in " + tr.destPath); return; }
            td_->send({{"@type", "deleteFile"}, {"file_id", tr.curFileId}});  // libera la cache TDLib
            finishDownload(tr);
        });
        return;
    }
    // Gruppo: accoda la parte al file parziale verificandone l'hash.
    t.st = TStatus::Joining;
    bool keep = settings_.keepParts;
    std::string keepDir = settings_.downloadDir;
    runWorker(tid, [this, tid, localPath, keep, keepDir](Transfer& tr, std::function<bool(int64_t)> progress) {
        const PartRef part = tr.parts[tr.curPart];
        int64_t base = tr.baseDone;
        std::string err, sha;
        Sha256 before = tr.running;
        bool ok = appendPart(localPath, tr.partialPath, tr.running, sha,
                             [&](int64_t v) { return progress(base + v); }, err);
        if (ok && !part.sha.empty() && part.sha != sha) {
            ok = false;
            err = "Parte " + std::to_string(part.index) + " corrotta (hash diverso): verrà riscaricata";
        }
        if (!ok) {
            // riporta il parziale allo stato precedente
            std::error_code ec;
            fs::resize_file(u8p(tr.partialPath), (uintmax_t)base, ec);
            tr.running = before;
            if (ec) tr.hashValid = false;
        }
        if (ok) {
            if (keep) moveFile(localPath, uniquePath(keepDir, partFileName(tr.name, part.index)));
            else removeFile(localPath);
        } else if (err.find("corrotta") != std::string::npos) {
            removeFile(localPath);
        }
        std::lock_guard<std::recursive_mutex> g(mu_);
        tr.workerBusy = false;
        td_->send({{"@type", "deleteFile"}, {"file_id", part.fileId}});
        if (tr.cancelRequested) { removeFile(tr.partialPath); cancel(tid); return; }
        if (!ok) { tr.done = base; fail(tr, err); return; }
        tr.baseDone = base + part.size;
        tr.done = tr.baseDone;
        tr.curPart++;
        queueDirty_ = true;
        if (tr.curPart < (int)tr.parts.size()) {
            if (tr.pauseRequested) { tr.pauseRequested = false; tr.st = TStatus::Paused; schedule(); changed(); return; }
            startDownloadPart(tr);
            changed();
            return;
        }
        // tutte le parti: verifica finale
        tr.st = TStatus::Verifying;
        std::string full = tr.running.hex();
        if (!tr.entry.fullSha.empty() && full != tr.entry.fullSha) {
            fail(tr, "Il file ricomposto non corrisponde all'originale (hash diverso)");
            return;
        }
        // senza estensione nel nome (es. parti caricate da altri programmi): la si ricava dal contenuto
        if (lacksExtension(fileNameOf(tr.destPath))) {
            std::string ext = guessExtension(tr.partialPath);
            if (!ext.empty()) {
                std::string dir = p8(u8p(tr.destPath).parent_path());
                std::string base = fileNameOf(tr.destPath);
                if (base.size() > 4 && base.compare(base.size() - 4, 4, ".001") == 0) base.resize(base.size() - 4);
                tr.destPath = uniquePath(dir, base + ext);
                tr.name = fileNameOf(tr.destPath);
                log("estensione ricavata dal contenuto: " + tr.name);
            }
        }
        if (!moveFile(tr.partialPath, tr.destPath)) { fail(tr, "Impossibile rinominare il file finale"); return; }
        finishDownload(tr);
    });
}

void Engine::finishDownload(Transfer& t) {
    t.st = TStatus::Done;
    t.done = t.total;
    t.speed = 0;
    t.pauseRequested = false;
    dlCache_[t.entryKey] = true;
    queueDirty_ = true;
    schedule();
    changed();
}

// ---------------------------------------------------------------- upload

void Engine::startUploadStep(Transfer& t) {
    if (!t.split) {
        uploadPath(t, t.srcPath);
        return;
    }
    preparePart(t);
}

void Engine::preparePart(Transfer& t) {
    t.st = TStatus::Preparing;
    std::string dir = joinPath(cfg_.tempDir, t.gid);
    makeDirs(dir);
    t.tempPart = joinPath(dir, partFileName(t.name, t.partIdx + 1));
    int tid = t.id;
    int64_t off = (int64_t)t.partIdx * t.partSize;
    int64_t len = t.partLen(t.partIdx);
    t.prepDone = 0;
    t.prepTotal = len;
    t.done = t.baseDone;
    // l'hash del file intero avanza solo se arriva esattamente fino all'inizio di questa parte
    bool feed = t.upRunningValid && t.upRunningBytes == off;
    if (t.upRunningValid && !feed && t.upRunningBytes != off + len) t.upRunningValid = false;
    auto running = std::make_shared<Sha256>(t.upRunning);
    log("parte " + std::to_string(t.partIdx + 1) + "/" + std::to_string(t.partCount) + " di " + t.name + ": estrazione");
    auto abort = t.abort;
    runWorker(tid, [this, tid, off, len, feed, running, abort](Transfer& tr, std::function<bool(int64_t)>) {
        std::string sha, err;
        bool ok = extractPart(tr.srcPath, off, len, tr.tempPart, sha,
                              [&](int64_t v) {
                                  std::lock_guard<std::recursive_mutex> lk(mu_);
                                  tr.prepDone = v;
                                  return !abort->load();
                              },
                              err, feed ? running.get() : nullptr);
        std::lock_guard<std::recursive_mutex> g(mu_);
        tr.workerBusy = false;
        tr.prepDone = tr.prepTotal = 0;
        if (ok && feed) {
            tr.upRunning = *running;
            tr.upRunningBytes = off + len;
        }
        if (!ok) log("estrazione fallita: " + err);
        if (tr.cancelRequested) { removeFile(tr.tempPart); cancel(tid); return; }
        if (!ok) {
            removeFile(tr.tempPart);
            tr.tempPart.clear();
            if (tr.pauseRequested) { tr.pauseRequested = false; tr.st = TStatus::Paused; schedule(); changed(); return; }
            fail(tr, err);
            return;
        }
        if ((int)tr.partShas.size() <= tr.partIdx) tr.partShas.resize(tr.partIdx + 1);
        tr.partShas[tr.partIdx] = sha;
        if (tr.pauseRequested) { tr.pauseRequested = false; tr.st = TStatus::Paused; schedule(); changed(); return; }
        uploadPath(tr, tr.tempPart);
        changed();
    });
}

void Engine::stopPendingUpload(Transfer& t) {
    if (t.sendingMsgId) {
        // il messaggio non è ancora stato inviato: eliminarlo annulla il caricamento
        json ids = json::array({t.sendingMsgId});
        td_->send({{"@type", "deleteMessages"}, {"chat_id", t.chatId}, {"message_ids", ids}, {"revoke", true}});
        sendingMsg_.erase(t.sendingMsgId);
        earlySend_.erase(t.sendingMsgId);
        log("upload interrotto (messaggio " + std::to_string(t.sendingMsgId) + " ritirato): " + t.name);
    } else if (t.sending) {
        t.stopWhenKnown = true;  // la risposta a sendMessage non è ancora arrivata
    }
    if (t.upFileId) fileToTransfer_.erase(t.upFileId);
    t.upFileId = 0;
    t.sendingMsgId = 0;
    if (!t.stopWhenKnown) t.sending = false;
}

// Upload = un messaggio con il file locale. TDLib carica il file (a parti, con più connessioni) e
// invia il messaggio da solo; l'avanzamento arriva con updateFile sul file del messaggio temporaneo,
// la fine con updateMessageSendSucceeded. (preliminaryUploadFile non completa mai il caricamento
// finché il file non viene usato in un messaggio: non va usato qui.)
void Engine::uploadPath(Transfer& t, const std::string& path) {
    const int64_t size = fileSize(path);
    if (size < 0) { fail(t, "File da inviare non accessibile"); return; }
    if (size == 0) { fail(t, "Il file da inviare è vuoto (0 byte)"); return; }
    if (size != t.partLen(t.partIdx)) {
        fail(t, "La dimensione del file da inviare è cambiata");
        return;
    }
    t.st = TStatus::Active;
    t.sending = true;
    t.sendingMsgId = 0;
    t.stopWhenKnown = false;
    t.done = t.baseDone;
    std::string caption;
    if (!t.folder.empty()) caption = std::string(kFolderMark) + t.folder;
    if (t.split) {
        SplitTag tag;
        tag.gid = t.gid;
        tag.index = t.partIdx + 1;
        tag.count = t.partCount;
        tag.total = t.total;
        tag.partSha = t.partIdx < (int)t.partShas.size() ? t.partShas[t.partIdx] : "";
        bool last = t.partIdx == t.partCount - 1;
        if (last && t.upRunningValid && t.upRunningBytes == t.total) t.fullSha = t.upRunning.hex();
        tag.fullSha = last ? t.fullSha : "";
        if (!caption.empty()) caption += "\n";
        caption += makeCaption(t.name, tag);
    }
    // TDLib 1.8.6x: inputMessageDocument.document è un inputDocument che contiene il file
    // (prima era direttamente l'InputFile: col formato vecchio TDLib legge "nessun file" e
    // risponde "InputFile is not specified")
    json content = {{"@type", "inputMessageDocument"},
                    {"document", {{"@type", "inputDocument"},
                                  {"document", {{"@type", "inputFileLocal"}, {"path", path}}},
                                  {"disable_content_type_detection", true}}}};
    if (!caption.empty()) content["caption"] = {{"@type", "formattedText"}, {"text", caption}};
    log("invio " + (t.split ? "parte " + std::to_string(t.partIdx + 1) + "/" + std::to_string(t.partCount) + " di " : std::string()) +
        t.name + " (" + std::to_string(size) + " byte)");
    int tid = t.id;
    td_->send({{"@type", "sendMessage"}, {"chat_id", t.chatId}, {"input_message_content", content}},
              [this, tid](const json& r) {
                  std::lock_guard<std::recursive_mutex> g(mu_);
                  Transfer* tx = find(tid);
                  if (!tx) return;
                  if (TdClient::isError(r)) {
                      tx->sending = false;
                      tx->stopWhenKnown = false;
                      if (tx->cancelRequested) { cancel(tid); return; }
                      fail(*tx, "Invio non riuscito: " + jstr(r, "message"));
                      return;
                  }
                  tx->sendingMsgId = jint(r, "id");
                  sendingMsg_[tx->sendingMsgId] = tid;
                  // il file del messaggio: da qui arrivano gli aggiornamenti del caricamento
                  const json& file = jget(jget(jget(r, "content"), "document"), "document");
                  if (file.is_object()) {
                      tx->upFileId = (int32_t)jint(file, "id");
                      if (tx->upFileId) fileToTransfer_[tx->upFileId] = tid;
                  }
                  log("messaggio temporaneo " + std::to_string(tx->sendingMsgId) + ", file " + std::to_string(tx->upFileId) +
                      ": " + tx->name);
                  if (tx->stopWhenKnown) {
                      // pausa o annullamento chiesti mentre la richiesta era in volo
                      tx->stopWhenKnown = false;
                      stopPendingUpload(*tx);
                      if (tx->cancelRequested) { cancel(tid); return; }
                      return;
                  }
                  if (file.is_object()) onFile(file);
                  // l'esito può essere arrivato prima di questa risposta
                  auto early = earlySend_.find(tx->sendingMsgId);
                  if (early != earlySend_.end()) {
                      json ev = early->second;
                      earlySend_.erase(early);
                      if (jtype(ev) == "updateMessageSendSucceeded")
                          onSendSucceeded(tx->sendingMsgId, jget(ev, "message"));
                      else
                          onSendFailed(tx->sendingMsgId, jstr(jget(ev, "error"), "message"));
                  }
              });
}

void Engine::onSendSucceeded(int64_t oldMsgId, const json& msg) {
    auto it = sendingMsg_.find(oldMsgId);
    if (it == sendingMsg_.end()) return;
    Transfer* t = find(it->second);
    sendingMsg_.erase(it);
    if (!t) return;
    t->sending = false;
    t->sendingMsgId = 0;
    fileToTransfer_.erase(t->upFileId);
    t->upFileId = 0;
    if (t->split && !t->tempPart.empty()) {
        removeFile(t->tempPart);
        t->tempPart.clear();
    }
    if (t->cancelRequested) {
        cancel(t->id);
        return;
    }
    t->baseDone += t->partLen(t->partIdx);
    t->done = t->baseDone;
    log("inviata parte " + std::to_string(t->partIdx + 1) + "/" + std::to_string(t->partCount) + ": " + t->name);
    t->partIdx++;
    queueDirty_ = true;
    if (t->partIdx < t->partCount) {
        if (t->pauseRequested) {
            t->pauseRequested = false;
            t->st = TStatus::Paused;
            schedule();
        } else {
            preparePart(*t);
        }
    } else {
        t->st = TStatus::Done;
        t->done = t->total;
        t->speed = 0;
        if (t->split) {
            std::error_code ec;
            fs::remove(u8p(joinPath(cfg_.tempDir, t->gid)), ec);
        }
        schedule();
        // mostra subito il nuovo file se la chat è aperta
        if (jint(msg, "chat_id") == selChat_ && t->chatId == selChat_) reloadList();
    }
    changed();
}

void Engine::onSendFailed(int64_t oldMsgId, const std::string& err) {
    auto it = sendingMsg_.find(oldMsgId);
    if (it == sendingMsg_.end()) return;
    Transfer* t = find(it->second);
    sendingMsg_.erase(it);
    if (!t) return;
    t->sending = false;
    if (t->cancelRequested) { cancel(t->id); return; }
    // al prossimo tentativo si ricarica la stessa parte
    t->done = t->baseDone;
    fail(*t, "Invio non riuscito: " + err);
}

// ---------------------------------------------------------------- canali, eliminazione, avvisi

void Engine::notice(const std::string& text, bool error) {
    notices_.push_back({text, error});
    if (notices_.size() > 8) notices_.erase(notices_.begin());
    changed();
}

bool Engine::popNotice(Notice& n) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (notices_.empty()) return false;
    n = notices_.front();
    notices_.erase(notices_.begin());
    return true;
}

void Engine::onMemberStatus(Chat& c, const json& status) {
    std::string st = jtype(status);
    c.owner = st == "chatMemberStatusCreator" && jbool(status, "is_member") != false;
    if (st == "chatMemberStatusCreator") c.owner = true;
    c.admin = c.owner ||
              (st == "chatMemberStatusAdministrator" && jbool(jget(status, "rights"), "can_delete_messages")) ||
              (st == "chatMemberStatusAdministrator" && jbool(status, "can_delete_messages"));
}

std::string Engine::entryPath(const FileEntry& e) {
    std::string dir = e.folder.empty() ? settings_.downloadDir : joinPath(settings_.downloadDir, e.folder);
    return joinPath(dir, e.name);
}

void Engine::removeMessages(int64_t chatId, const std::set<int64_t>& ids) {
    if (ids.empty()) return;
    bool any = false;
    for (size_t i = 0; i < entries_.size();) {
        FileEntry& e = entries_[i];
        bool drop = false;
        if (e.chatId == chatId) {
            if (e.isGroup) {
                for (auto it = e.parts.begin(); it != e.parts.end();)
                    it = ids.count(it->second.msgId) ? e.parts.erase(it) : std::next(it);
                drop = e.parts.empty();
            } else {
                drop = ids.count(e.msgId) > 0;
            }
        }
        if (drop) {
            dlCache_.erase(e.key);
            entries_.erase(entries_.begin() + (long)i);
            any = true;
        } else {
            ++i;
        }
    }
    if (any) {
        entryIndex_.clear();
        for (size_t i = 0; i < entries_.size(); ++i) entryIndex_[entries_[i].key] = i;
        if (totalCount_ > 0) totalCount_ = std::max<int64_t>(0, totalCount_ - (int64_t)ids.size());
        changed();
    }
}

void Engine::removeChatLocal(int64_t chatId) {
    chatOrder_.erase(std::remove(chatOrder_.begin(), chatOrder_.end(), chatId), chatOrder_.end());
    chatMap_.erase(chatId);
    if (selChat_ == chatId) {
        selChat_ = 0;
        if (savedChatId_) selectChat(savedChatId_);
        else reloadList();
    }
    changed();
}

void Engine::createChannel(const std::string& title, const std::string& description, bool asGroup) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!td_ || title.empty()) return;
    // solo i campi stabili tra le versioni di TDLib; gli altri prendono il valore predefinito
    td_->send({{"@type", "createNewSupergroupChat"},
               {"title", title},
               {"is_channel", !asGroup},
               {"description", description}},
              [this, title, asGroup](const json& r) {
                  std::lock_guard<std::recursive_mutex> lk1(mu_);
                  if (TdClient::isError(r)) {
                      std::string m = jstr(r, "message");
                      if (m.find("CHANNELS_TOO_MUCH") != std::string::npos)
                          m = "hai raggiunto il numero massimo di canali e gruppi";
                      else if (m.rfind("Too Many Requests", 0) == 0 || m.find("FLOOD") != std::string::npos)
                          m = "troppe richieste, riprova tra qualche minuto";
                      notice(std::string("Impossibile creare ") + (asGroup ? "il gruppo: " : "il canale: ") + m, true);
                      return;
                  }
                  int64_t id = jint(r, "id");
                  Chat& c = chatMap_[id];
                  c.id = id;
                  if (c.title.empty()) c.title = title;
                  c.kind = asGroup ? ChatKind::Group : ChatKind::Channel;
                  c.owner = c.admin = true;
                  chatOrder_.erase(std::remove(chatOrder_.begin(), chatOrder_.end(), id), chatOrder_.end());
                  chatOrder_.insert(chatOrder_.begin(), id);
                  selectChat(id);
                  notice((asGroup ? "Gruppo “" : "Canale “") + title + "” creato");
              });
}

void Engine::deleteChat(int64_t chatId) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    auto it = chatMap_.find(chatId);
    if (!td_ || it == chatMap_.end() || chatId == savedChatId_) return;
    Chat c = it->second;
    bool owner = c.owner && (c.kind == ChatKind::Channel || c.kind == ChatKind::Group);
    // i trasferimenti verso/da questa chat non hanno più senso
    std::vector<int> ids;
    for (auto& t : transfers_)
        if (t->chatId == chatId && t->st != TStatus::Done) ids.push_back(t->id);
    for (int id : ids) cancel(id);
    td_->send({{"@type", owner ? "deleteChat" : "leaveChat"}, {"chat_id", chatId}},
              [this, chatId, c, owner](const json& r) {
                  std::lock_guard<std::recursive_mutex> lk1(mu_);
                  bool channel = c.kind == ChatKind::Channel;
                  if (TdClient::isError(r)) {
                      std::string m = jstr(r, "message");
                      if (m.find("ADMIN_REQUIRED") != std::string::npos) m = "solo il proprietario pu\u00F2 farlo";
                      notice(std::string(owner ? (channel ? "Impossibile eliminare il canale: " : "Impossibile eliminare il gruppo: ")
                                               : (channel ? "Impossibile uscire dal canale: " : "Impossibile uscire dal gruppo: ")) + m,
                             true);
                      return;
                  }
                  removeChatLocal(chatId);
                  notice(owner ? "“" + c.title + "” eliminato per tutti"
                               : "Sei uscito da “" + c.title + "”");
              });
}

void Engine::deleteEntries(const std::vector<std::string>& keys) {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!td_) return;
    // raggruppa per chat; per i gruppi di parti servono tutti i messaggi, anche quelli non caricati
    std::map<int64_t, std::set<int64_t>> byChat;
    std::vector<FileEntry> groups;
    for (auto& k : keys) {
        auto it = entryIndex_.find(k);
        if (it == entryIndex_.end()) continue;
        const FileEntry& e = entries_[it->second];
        // annulla un eventuale download in corso di quel file
        for (auto& t : transfers_)
            if (t->entryKey == k && t->dir == Direction::Download && t->st != TStatus::Done) {
                int id = t->id;
                cancel(id);
                break;
            }
        if (e.isGroup) {
            groups.push_back(e);
            for (auto& p : e.parts) byChat[e.chatId].insert(p.second.msgId);
        } else {
            byChat[e.chatId].insert(e.msgId);
        }
    }
    auto total = std::make_shared<int>((int)keys.size());
    auto send = [this, total](int64_t chatId, const std::set<int64_t>& ids) {
        json arr = json::array();
        for (int64_t id : ids) arr.push_back(id);
        td_->send({{"@type", "deleteMessages"}, {"chat_id", chatId}, {"message_ids", arr}, {"revoke", true}},
                  [this, chatId, ids, total](const json& r) {
                      std::lock_guard<std::recursive_mutex> lk1(mu_);
                      if (TdClient::isError(r)) {
                          std::string m = jstr(r, "message");
                          if (m.find("can't be deleted") != std::string::npos || m.find("FORBIDDEN") != std::string::npos ||
                              m.find("ADMIN") != std::string::npos || m.find("rights") != std::string::npos)
                              m = "non hai i permessi per eliminare file in questa chat";
                          notice("Eliminazione non riuscita: " + m, true);
                          return;
                      }
                      removeMessages(chatId, ids);  // non aspetta updateDeleteMessages
                      if (*total > 0) {
                          notice(*total == 1 ? std::string("File eliminato") : std::to_string(*total) + " file eliminati");
                          *total = 0;  // un solo avviso anche con più chat
                      }
                  });
    };
    if (groups.empty()) {
        for (auto& kv : byChat) send(kv.first, kv.second);
        return;
    }
    // prima ritrova tutte le parti dei gruppi con l'hashtag, poi elimina
    auto pending = std::make_shared<int>((int)groups.size());
    auto acc = std::make_shared<std::map<int64_t, std::set<int64_t>>>(byChat);
    for (auto& e : groups) {
        td_->send({{"@type", "searchChatMessages"}, {"chat_id", e.chatId}, {"query", "#tf" + e.gid},
                   {"from_message_id", 0}, {"offset", 0}, {"limit", 100},
                   {"filter", {{"@type", "searchMessagesFilterEmpty"}}}},
                  [this, e, pending, acc, send](const json& r) {
                      std::lock_guard<std::recursive_mutex> lk1(mu_);
                      const json& msgs = jget(r, "messages");
                      if (msgs.is_array())
                          for (auto& m : msgs) {
                              SplitTag tag;
                              std::string cap = jstr(jget(jget(m, "content"), "caption"), "text");
                              if (parseCaption(cap, tag) && tag.gid == e.gid) (*acc)[e.chatId].insert(jint(m, "id"));
                          }
                      if (--*pending == 0)
                          for (auto& kv : *acc) send(kv.first, kv.second);
                  });
    }
}

int Engine::uploadFolder(int64_t chatId, const std::string& dir) {
    std::error_code ec;
    fs::path root = u8p(dir);
    if (!fs::is_directory(root, ec)) return 0;
    std::string base = sanitizeFileName(p8(root.filename()));
    std::vector<std::pair<std::string, std::string>> files;  // percorso, cartella relativa
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (it->is_symlink(ec)) { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(ec)) continue;
        std::string name = p8(it->path().filename());
        if (name == "desktop.ini" || name == "Thumbs.db" || name == ".DS_Store") continue;
        fs::path rel = it->path().parent_path().lexically_relative(root);
        std::string sub = p8(rel);
        std::string folder = (sub.empty() || sub == ".") ? base : base + "/" + sub;
        files.push_back({p8(it->path()), folder});
    }
    std::sort(files.begin(), files.end());
    int n = 0;
    for (auto& f : files)
        if (upload(chatId, f.first, f.second) > 0) ++n;
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (n == 0) notice("La cartella “" + base + "” è vuota", true);
    else notice("Cartella “" + base + "”: " + std::to_string(n) + (n == 1 ? " file in coda" : " file in coda"));
    return n;
}

// ---------------------------------------------------------------- persistenza

// Portable: "D:\\TeleFiles\\Download\\x" <-> "Download\\x" relativo alla cartella dell'exe.
static std::string toRel(const std::string& base, const std::string& path) {
    if (base.empty() || path.empty()) return path;
    std::error_code ec;
    fs::path b = fs::weakly_canonical(u8p(base), ec), p = fs::weakly_canonical(u8p(path), ec);
    auto r = p.lexically_relative(b);
    std::string rs = p8(r);
    if (r.empty() || rs.rfind("..", 0) == 0) return path;  // fuori dalla cartella portable
    return rs;
}
static std::string toAbs(const std::string& base, const std::string& path) {
    if (base.empty() || path.empty() || u8p(path).is_absolute()) return path;
    return p8(u8p(base) / u8p(path));
}

static std::string readText(const std::string& path) {
    InFile in;
    if (!in.open(path)) return {};
    std::string s, buf(65536, '\0');
    for (;;) {
        int64_t n = in.read(buf.data(), (int64_t)buf.size());
        if (n <= 0) break;
        s.append(buf.data(), (size_t)n);
    }
    return s;
}

static void writeText(const std::string& path, const std::string& s) {
    OutFile o;
    if (o.open(path + ".tmp", false) && o.write(s.data(), (int64_t)s.size()) && o.close())
        moveFile(path + ".tmp", path);
}

void Engine::saveSettings() {
    json j = {{"download_dir", toRel(cfg_.portableBase, settings_.downloadDir)},
              {"max_parallel", settings_.maxParallel},
              {"keep_parts", settings_.keepParts},
              {"theme", settings_.theme},
              {"transfers_height", settings_.transfersHeight},
              {"transfers_collapsed", settings_.transfersCollapsed},
              {"sort_column", settings_.sortColumn},
              {"sort_descending", settings_.sortDescending}};
    writeText(joinPath(cfg_.dataDir, "settings.json"), j.dump(2));
}

void Engine::loadSettings() {
    json j = json::parse(readText(joinPath(cfg_.dataDir, "settings.json")), nullptr, false);
    if (!j.is_object()) return;
    std::string d = jstr(j, "download_dir");
    if (!d.empty()) settings_.downloadDir = toAbs(cfg_.portableBase, d);
    if (jint(j, "max_parallel") > 0) settings_.maxParallel = (int)std::min<int64_t>(8, jint(j, "max_parallel"));
    settings_.keepParts = jbool(j, "keep_parts");
    settings_.theme = (int)std::max<int64_t>(0, std::min<int64_t>(2, jint(j, "theme")));
    if (jint(j, "transfers_height") > 0) settings_.transfersHeight = (int)std::min<int64_t>(2000, jint(j, "transfers_height"));
    settings_.transfersCollapsed = jbool(j, "transfers_collapsed");
    settings_.sortColumn = (int)std::max<int64_t>(0, std::min<int64_t>(2, jint(j, "sort_column")));
    if (j.contains("sort_descending")) settings_.sortDescending = jbool(j, "sort_descending");
}

void Engine::saveQueue() {
    queueDirty_ = false;
    // finché la coda salvata non è stata ripristinata non va sovrascritta
    if (!restored_ && pendingRestore_.is_array()) return;
    json arr = json::array();
    for (auto& tp : transfers_) {
        Transfer& t = *tp;
        if (t.st == TStatus::Done) continue;
        json j = {{"dir", t.dir == Direction::Download ? "down" : "up"},
                  {"paused", t.st == TStatus::Paused || t.pauseRequested},
                  {"name", t.name},
                  {"chat_id", t.chatId},
                  {"total", t.total}};
        if (t.dir == Direction::Download) {
            j["msg_id"] = t.entry.msgId;
            j["file_name"] = t.entry.name;
            j["folder"] = t.entry.folder;
            j["group"] = t.entry.isGroup;
            j["gid"] = t.entry.gid;
            j["part_count"] = t.entry.partCount;
            j["full_sha"] = t.entry.fullSha;
            j["dest"] = toRel(cfg_.portableBase, t.destPath);
            j["partial"] = toRel(cfg_.portableBase, t.partialPath);
            j["cur_part"] = t.curPart;
            j["base_done"] = t.baseDone;
            j["key"] = t.entryKey;
        } else {
            j["src"] = toRel(cfg_.portableBase, t.srcPath);
            j["folder"] = t.folder;
            j["prepared"] = t.prepared;
            j["split"] = t.split;
            j["part_size"] = t.partSize;
            j["part_count"] = t.partCount;
            j["part_idx"] = t.partIdx;
            j["gid"] = t.gid;
            j["full_sha"] = t.fullSha;
            j["part_shas"] = t.partShas;
            j["base_done"] = t.baseDone;
        }
        arr.push_back(j);
    }
    writeText(joinPath(cfg_.dataDir, "queue.json"), arr.dump(2));
}

void Engine::loadQueue() {
    json j = json::parse(readText(joinPath(cfg_.dataDir, "queue.json")), nullptr, false);
    if (j.is_array()) pendingRestore_ = j;
}

void Engine::restoreTransfers() {
    if (!pendingRestore_.is_array()) return;
    for (auto& j : pendingRestore_) {
        auto t = std::make_unique<Transfer>();
        t->id = nextTid_++;
        t->name = jstr(j, "name");
        t->chatId = jint(j, "chat_id");
        t->chatTitle = chatTitleFor(chatMap_, t->chatId, savedChatId_);
        t->total = jint(j, "total");
        t->st = TStatus::Paused;  // al riavvio tutto riparte in pausa: decidi tu cosa riprendere
        if (jstr(j, "dir") == "down") {
            t->dir = Direction::Download;
            FileEntry& e = t->entry;
            e.chatId = t->chatId;
            e.msgId = jint(j, "msg_id");
            e.name = jstr(j, "file_name");
            e.folder = cleanFolder(jstr(j, "folder"));
            e.size = t->total;
            e.isGroup = jbool(j, "group");
            e.gid = jstr(j, "gid");
            e.partCount = (int)jint(j, "part_count");
            e.fullSha = jstr(j, "full_sha");
            t->entryKey = jstr(j, "key");
            t->destPath = toAbs(cfg_.portableBase, jstr(j, "dest"));
            t->partialPath = toAbs(cfg_.portableBase, jstr(j, "partial"));
            t->curPart = (int)jint(j, "cur_part");
            t->baseDone = jint(j, "base_done");
            t->done = t->baseDone;
            if (e.isGroup) {
                t->partsCollected = false;               // file id TDLib non stabili: si ricercano
                t->hashValid = t->curPart == 0;          // hash progressivo da ricalcolare
                if (t->curPart == 0) t->destPath.clear(), t->partialPath.clear();
            } else {
                // il file id va riletto dal messaggio
                int tid = t->id;
                td_->send({{"@type", "getMessage"}, {"chat_id", e.chatId}, {"message_id", e.msgId}},
                          [this, tid](const json& m) {
                              std::lock_guard<std::recursive_mutex> g(mu_);
                              Transfer* tx = find(tid);
                              if (!tx) return;
                              if (TdClient::isError(m)) { fail(*tx, "Il messaggio non esiste più"); return; }
                              std::vector<FileEntry> v;
                              addMessage(m, v);
                              if (!v.empty()) tx->entry.fileId = v[0].fileId;
                          });
                t->destPath.clear();  // TDLib riprende dal suo parziale, la destinazione si ricalcola
            }
        } else {
            t->dir = Direction::Upload;
            t->srcPath = toAbs(cfg_.portableBase, jstr(j, "src"));
            t->folder = jstr(j, "folder");
            t->prepared = jbool(j, "prepared");
            t->split = jbool(j, "split");
            t->partSize = jint(j, "part_size");
            t->partCount = std::max<int>(1, (int)jint(j, "part_count"));
            t->partIdx = (int)jint(j, "part_idx");
            t->gid = jstr(j, "gid");
            t->fullSha = jstr(j, "full_sha");
            const json& ps = jget(j, "part_shas");
            if (ps.is_array())
                for (auto& s : ps) t->partShas.push_back(s.is_string() ? s.get<std::string>() : "");
            t->baseDone = jint(j, "base_done");
            t->done = t->baseDone;
            t->upRunningValid = t->partIdx == 0;
            if (!fileExists(t->srcPath)) {
                t->st = TStatus::Error;
                t->error = "File originale non trovato";
            }
        }
        transfers_.push_back(std::move(t));
    }
    pendingRestore_ = json();
    changed();
}

} // namespace tf
