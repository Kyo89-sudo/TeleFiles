// Server Telegram simulato che espone la stessa API JSON di TDLib (td_send/td_receive).
// Serve a verificare TeleFiles senza account: accesso, chat, ricerca, download/upload
// con avanzamento, pausa/ripresa, limite di dimensione, invio messaggi, logout.
#include <td/telegram/td_json_client.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <filesystem>
#include <cstdlib>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace fake {

struct File {
    int id = 0;
    std::string name;
    std::string content;          // contenuto lato server
    int64_t size = 0;
    // download locale
    bool downloading = false;
    int64_t downloaded = 0;
    std::string localPath;
    bool localDone = false;
    // upload locale
    bool uploading = false;
    int64_t uploaded = 0;
    std::string uploadSrc;
    bool uploadDone = false;       // tutte le parti inviate (preliminaryUploadFile)
    bool sentInMessage = false;    // usato in un messaggio: ha una posizione remota completa
    bool corrupt = false;
    bool stalled = false;          // fermo finché il client non richiede di nuovo il download
    bool stallUsed = false;          // se vero, il download consegna byte alterati
};

struct Msg {
    int64_t id = 0;
    int64_t chatId = 0;
    int64_t date = 0;
    std::string kind = "messageDocument";   // messageDocument / messageVideo / messageAudio / messageText
    int fileId = 0;
    std::string caption;
    std::string text;
};

struct Chat {
    int64_t id;
    std::string title;
    std::string type;  // private / channel / group
    std::string status = "chatMemberStatusMember";  // Creator / Administrator / Member
};

std::mutex mu;
std::condition_variable cv;
std::deque<std::string> out;
int clientCounter = 0;
int curClient = 0;

// stato "server"
std::map<int, File> files;
std::map<int64_t, Msg> msgs;
std::vector<Chat> chats;
int nextFile = 1000;
int64_t nextMsg = 5000;
int64_t meId = 777000;
bool earlySendUpdate = false;
int stallAfter = -1;            // blocca i download nuovi dopo N byte (-1 = mai)   // invia updateMessageSendSucceeded prima della risposta
bool authorized = false;     // sessione salvata
bool premium = false;
int64_t uploadLimit = 2000LL * 1024 * 1024;
std::string filesDir;
std::string authState = "authorizationStateWaitTdlibParameters";
int qrScanCountdown = -1;
bool havePassword = true;
int64_t chunk = 64 * 1024;   // byte per passo di simulazione
std::set<std::string> methodsSeen;

std::string deterministic(const std::string& seed, int64_t n) {
    uint32_t x = 2166136261u;
    for (char c : seed) x = (x ^ (uint8_t)c) * 16777619u;
    std::string s((size_t)n, '\0');
    for (int64_t i = 0; i < n; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        s[(size_t)i] = (char)(x & 0xFF);
    }
    return s;
}

void push(json j) {
    j["@client_id"] = curClient;
    out.push_back(j.dump());
    cv.notify_all();
}

void setAuth(const std::string& st, json extra = json::object()) {
    authState = st;
    json s = {{"@type", st}};
    for (auto& [k, v] : extra.items()) s[k] = v;
    push({{"@type", "updateAuthorizationState"}, {"authorization_state", s}});
}

json fileJson(const File& f) {
    return {{"@type", "file"},
            {"id", f.id},
            {"size", f.size},
            {"expected_size", f.size},
            {"local", {{"@type", "localFile"},
                       {"path", f.localDone ? f.localPath : ""},
                       {"is_downloading_active", f.downloading},
                       {"is_downloading_completed", f.localDone},
                       {"downloaded_prefix_size", f.downloaded},
                       {"downloaded_size", f.downloaded}}},
            {"remote", {{"@type", "remoteFile"},
                        {"id", f.content.empty() && !f.uploadDone ? "" : "R" + std::to_string(f.id)},
                        {"is_uploading_active", f.uploading},
                        // come TDLib: completato solo quando il file è stato usato in un messaggio
                        {"is_uploading_completed", f.sentInMessage},
                        {"uploaded_size", f.uploaded}}}};
}

json userJson() {
    return {{"@type", "user"}, {"id", meId}, {"first_name", "Kyo"}, {"last_name", "Test"},
            {"usernames", {{"active_usernames", {"kyo_test"}}}}, {"phone_number", "393331234567"},
            {"is_premium", premium}};
}

json msgJson(const Msg& m) {
    json c;
    if (m.kind == "messageText") {
        c = {{"@type", "messageText"}, {"text", {{"@type", "formattedText"}, {"text", m.text}}}};
    } else {
        const File& f = files[m.fileId];
        std::string key = m.kind == "messageVideo" ? "video" : (m.kind == "messageAudio" ? "audio" : "document");
        c = {{"@type", m.kind},
             {key, {{"file_name", f.name}, {key, fileJson(f)}}},
             {"caption", {{"@type", "formattedText"}, {"text", m.caption}}}};
    }
    return {{"@type", "message"}, {"id", m.id}, {"chat_id", m.chatId}, {"date", m.date}, {"content", c}};
}

int addFile(const std::string& name, int64_t size) {
    File f;
    f.id = nextFile++;
    f.name = name;
    f.content = deterministic(name, size);
    f.size = size;
    f.sentInMessage = true;
    files[f.id] = f;
    return f.id;
}

std::map<int64_t, Msg> pending;   // messaggi temporanei in caricamento (id temporaneo -> messaggio)
int pendingDeleted = 0;
int64_t uploadStallAfter = -1;     // blocca il caricamento dopo N byte (una volta per file)

// fine del caricamento: il messaggio temporaneo diventa definitivo
void finishUpload(File& f) {
    f.uploading = false;
    f.uploadDone = true;
    std::ifstream in(f.uploadSrc, std::ios::binary);
    f.content.assign(std::istreambuf_iterator<char>(in), {});
    f.size = (int64_t)f.content.size();
    f.sentInMessage = true;
    push({{"@type", "updateFile"}, {"file", fileJson(f)}});
    for (auto it = pending.begin(); it != pending.end(); ++it) {
        if (it->second.fileId != f.id) continue;
        Msg m = it->second;
        int64_t tempId = it->first;
        pending.erase(it);
        msgs[m.id] = m;
        push({{"@type", "updateMessageSendSucceeded"}, {"old_message_id", tempId}, {"message", msgJson(m)}});
        break;
    }
}

void addMsg(int64_t chat, const std::string& kind, int fileId, const std::string& caption = "") {
    Msg m;
    m.id = nextMsg++;
    m.chatId = chat;
    m.date = 1793800000 + m.id;
    m.kind = kind;
    m.fileId = fileId;
    m.caption = caption;
    msgs[m.id] = m;
}

void seed() {
    static bool done = false;
    if (done) return;
    done = true;
    // per provare l'interfaccia senza digitare il login
    if (const char* a = std::getenv("TF_FAKE_AUTOLOGIN")) authorized = a[0] == '1';
    if (const char* c = std::getenv("TF_FAKE_CHUNK")) chunk = std::atoll(c);
    if (const char* u = std::getenv("TF_FAKE_UPLOAD_LIMIT")) uploadLimit = std::atoll(u);
    chats = {{meId, "Kyo Test", "private"},
             {-1001, "Linux ISO", "channel"},
             {-1002, "Progetti lavoro", "group", "chatMemberStatusAdministrator"},
             {-1003, "Podcast Italia", "channel"}};
    addMsg(meId, "messageDocument", addFile("appunti.pdf", 180 * 1024));
    addMsg(-1001, "messageDocument", addFile("ubuntu-26.04-desktop-amd64.iso", 3 * 1024 * 1024 + 123));
    addMsg(-1001, "messageDocument", addFile("ubuntu-26.04-server-amd64.iso", 2 * 1024 * 1024));
    addMsg(-1001, "messageDocument", addFile("debian-13.2-amd64-netinst.iso", 900 * 1024));
    addMsg(-1001, "messageVideo", addFile("ubuntu-summit-keynote.mkv", 1500 * 1024));
    for (int i = 0; i < 30; ++i) {
        Msg m; m.id = nextMsg++; m.chatId = -1001; m.kind = "messageText"; m.text = "messaggio " + std::to_string(i);
        msgs[m.id] = m;
    }
    addMsg(-1001, "messageDocument", addFile("fedora-43-workstation.iso", 1200 * 1024));
    addMsg(-1002, "messageDocument", addFile("preventivo.xlsx", 40 * 1024));
    if (std::getenv("TF_FAKE_MANY"))  // chat con molti file, per provare lo scorrimento
        for (int i = 1; i <= 120; ++i)
            addMsg(-1002, i % 3 ? "messageDocument" : "messageVideo",
                   addFile("documento-" + std::to_string(1000 + i) + (i % 3 ? ".pdf" : ".mp4"), (40 + i * 7) * 1024));
    addMsg(-1003, "messageAudio", addFile("puntata-12.mp3", 700 * 1024));
}

bool contains(std::string h, std::string n) {
    std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    return h.find(n) != std::string::npos;
}

bool matchFilter(const Msg& m, const std::string& filter) {
    if (filter == "searchMessagesFilterEmpty" || filter.empty()) return true;
    if (filter == "searchMessagesFilterDocument") return m.kind == "messageDocument";
    if (filter == "searchMessagesFilterVideo") return m.kind == "messageVideo";
    if (filter == "searchMessagesFilterAudio") return m.kind == "messageAudio";
    return false;
}

void reply(const json& req, json res) {
    if (req.contains("@extra")) res["@extra"] = req["@extra"];
    push(res);
}

void error(const json& req, int code, const std::string& msg) {
    reply(req, {{"@type", "error"}, {"code", code}, {"message", msg}});
}

int64_t sgId(int64_t chatId) { return chatId < 0 ? -chatId : 0; }

json statusJson(const Chat& c) {
    json st = {{"@type", c.status}};
    if (c.status == "chatMemberStatusCreator") st["is_member"] = true;
    if (c.status == "chatMemberStatusAdministrator") st["rights"] = {{"can_delete_messages", true}};
    return st;
}

void announceChat(const Chat& c) {
    json type;
    if (c.type == "private") type = {{"@type", "chatTypePrivate"}, {"user_id", c.id}};
    else {
        type = {{"@type", "chatTypeSupergroup"}, {"supergroup_id", sgId(c.id)}, {"is_channel", c.type == "channel"}};
        // come TDLib: prima le informazioni del supergruppo (con il nostro stato), poi la chat
        push({{"@type", "updateSupergroup"}, {"supergroup", {{"@type", "supergroup"}, {"id", sgId(c.id)},
                                                             {"status", statusJson(c)},
                                                             {"is_channel", c.type == "channel"}}}});
    }
    push({{"@type", "updateNewChat"}, {"chat", {{"@type", "chat"}, {"id", c.id}, {"title", c.title},
                                                {"type", type}, {"unread_count", 0},
                                                {"positions", json::array()}}}});
}

Chat* findChat(int64_t id) {
    for (auto& c : chats) if (c.id == id) return &c;
    return nullptr;
}

void dropChat(int64_t id) {
    for (auto it = msgs.begin(); it != msgs.end();)
        it = it->second.chatId == id ? msgs.erase(it) : std::next(it);
    chats.erase(std::remove_if(chats.begin(), chats.end(), [&](const Chat& c) { return c.id == id; }), chats.end());
    push({{"@type", "updateChatPosition"}, {"chat_id", id},
          {"position", {{"@type", "chatPosition"}, {"list", {{"@type", "chatListMain"}}}, {"order", "0"}}}});
}

void becomeReady() {
    authorized = true;
    setAuth("authorizationStateReady");
    for (auto& c : chats) announceChat(c);
}

void handle(const json& req) {
    std::string t = req.value("@type", "");
    methodsSeen.insert(t);
    if (t == "getOption") {
        reply(req, {{"@type", "optionValueString"}, {"value", "fake"}});
        if (authState == "authorizationStateWaitTdlibParameters") setAuth(authState);
    } else if (t == "setTdlibParameters") {
        if (req.value("api_id", 0) == 0) return error(req, 400, "Valid api_id must be provided");
        filesDir = req.value("files_directory", "");
        fs::create_directories(filesDir);
        reply(req, {{"@type", "ok"}});
        if (authorized) becomeReady();
        else setAuth("authorizationStateWaitPhoneNumber");
    } else if (t == "setAuthenticationPhoneNumber") {
        std::string p = req.value("phone_number", "");
        if (p.size() < 8 || p[0] != '+') return error(req, 400, "PHONE_NUMBER_INVALID");
        reply(req, {{"@type", "ok"}});
        setAuth("authorizationStateWaitCode",
                {{"code_info", {{"type", {{"@type", "authenticationCodeTypeTelegramMessage"}, {"length", 5}}}}}});
    } else if (t == "checkAuthenticationCode") {
        if (req.value("code", "") != "12345") return error(req, 400, "PHONE_CODE_INVALID");
        reply(req, {{"@type", "ok"}});
        if (havePassword) setAuth("authorizationStateWaitPassword", {{"password_hint", "il solito"}});
        else becomeReady();
    } else if (t == "checkAuthenticationPassword") {
        if (req.value("password", "") != "segreta") return error(req, 400, "PASSWORD_HASH_INVALID");
        reply(req, {{"@type", "ok"}});
        becomeReady();
    } else if (t == "requestQrCodeAuthentication") {
        reply(req, {{"@type", "ok"}});
        setAuth("authorizationStateWaitOtherDeviceConfirmation", {{"link", "tg://login?token=FAKETOKEN123"}});
        qrScanCountdown = std::getenv("TF_FAKE_QR_STEPS") ? std::atoi(std::getenv("TF_FAKE_QR_STEPS")) : 20;
    } else if (t == "logOut") {
        reply(req, {{"@type", "ok"}});
        authorized = false;
        setAuth("authorizationStateLoggingOut");
        setAuth("authorizationStateClosed");
        authState = "authorizationStateWaitTdlibParameters";
    } else if (t == "close") {
        reply(req, {{"@type", "ok"}});
        setAuth("authorizationStateClosing");
        setAuth("authorizationStateClosed");
        authState = "authorizationStateWaitTdlibParameters";
    } else if (authState != "authorizationStateReady") {
        error(req, 401, "Unauthorized");
    } else if (t == "getMe") {
        reply(req, userJson());
    } else if (t == "createPrivateChat") {
        reply(req, {{"@type", "chat"}, {"id", req.value("user_id", (int64_t)0)}, {"title", "Kyo Test"}});
    } else if (t == "loadChats") {
        reply(req, {{"@type", "error"}, {"code", 404}, {"message", "Not Found"}});
    } else if (t == "getChats") {
        json ids = json::array();
        for (auto& c : chats) ids.push_back(c.id);
        reply(req, {{"@type", "chats"}, {"total_count", ids.size()}, {"chat_ids", ids}});
    } else if (t == "openChat") {
        reply(req, {{"@type", "ok"}});
    } else if (t == "searchChatMessages") {
        int64_t chat = req.value("chat_id", (int64_t)0);
        std::string q = req.value("query", "");
        int64_t from = req.value("from_message_id", (int64_t)0);
        int limit = req.value("limit", 100);
        std::string filter = req.contains("filter") ? req["filter"].value("@type", "") : "";
        std::vector<const Msg*> found;
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
            const Msg& m = it->second;
            if (m.chatId != chat || !matchFilter(m, filter)) continue;
            if (!q.empty()) {
                std::string hay = m.caption + " " + m.text + " " + (m.fileId ? files[m.fileId].name : "");
                if (!contains(hay, q)) continue;
            }
            found.push_back(&m);
        }
        json arr = json::array();
        int64_t next = 0;
        int n = 0;
        for (const Msg* m : found) {
            if (from && m->id >= from) continue;
            if (n == limit) { next = arr.back()["id"].get<int64_t>(); break; }
            arr.push_back(msgJson(*m));
            ++n;
        }
        reply(req, {{"@type", "foundChatMessages"}, {"total_count", found.size()}, {"messages", arr},
                    {"next_from_message_id", next}});
    } else if (t == "getChatMessageCount") {
        int64_t chat = req.value("chat_id", (int64_t)0);
        std::string filter = req["filter"].value("@type", "");
        int c = 0;
        for (auto& [id, m] : msgs) if (m.chatId == chat && matchFilter(m, filter) && m.kind != "messageText") ++c;
        reply(req, {{"@type", "count"}, {"count", c}});
    } else if (t == "getMessage") {
        auto it = msgs.find(req.value("message_id", (int64_t)0));
        if (it == msgs.end()) return error(req, 404, "Message not found");
        reply(req, msgJson(it->second));
    } else if (t == "downloadFile") {
        int id = req.value("file_id", 0);
        auto it = files.find(id);
        if (it == files.end()) return error(req, 404, "File not found");
        File& f = it->second;
        if (f.content.empty() && f.size) return error(req, 400, "File not on server");
        if (!f.localDone) f.downloading = true;
        if (f.stalled) { f.stalled = false; f.stallUsed = true; }  // la nuova richiesta lo sblocca
        reply(req, fileJson(f));
    } else if (t == "cancelDownloadFile") {
        auto it = files.find(req.value("file_id", 0));
        if (it != files.end()) {
            it->second.downloading = false;
            push({{"@type", "updateFile"}, {"file", fileJson(it->second)}});
        }
        reply(req, {{"@type", "ok"}});
    } else if (t == "deleteFile") {
        auto it = files.find(req.value("file_id", 0));
        if (it != files.end()) {
            File& f = it->second;
            if (!f.localPath.empty()) fs::remove(f.localPath);
            f.localPath.clear();
            f.localDone = false;
            f.downloaded = 0;
            f.downloading = false;
        }
        reply(req, {{"@type", "ok"}});
    } else if (t == "preliminaryUploadFile" || t == "cancelPreliminaryUploadFile") {
        error(req, 400, "TeleFiles non deve più usare " + t);
    } else if (t == "sendMessage") {
        // come TDLib: il messaggio con un file locale torna subito come temporaneo, il file viene
        // caricato e solo alla fine arriva updateMessageSendSucceeded
        int64_t chat = req.value("chat_id", (int64_t)0);
        const json& c = req["input_message_content"];
        // schema di TDLib 1.8.69: inputMessageDocument.document = inputDocument{document: InputFile}
        const json& wrap = c["document"];
        if (!wrap.is_object() || wrap.value("@type", "") != "inputDocument" || !wrap.contains("document"))
            return error(req, 400, "InputFile is not specified");
        const json& doc = wrap["document"];
        if (doc.value("@type", "") != "inputFileLocal") return error(req, 400, "InputFile is not specified");
        std::string path = doc.value("path", "");
        std::error_code ec;
        auto sz = fs::file_size(path, ec);
        if (ec) return error(req, 400, "File not found: " + path);
        if ((int64_t)sz > uploadLimit) return error(req, 400, "FILE_PARTS_INVALID (file troppo grande)");
        int fid = 0;
        // ripresa: stesso percorso e caricamento interrotto -> stesso file, si riparte da quanto inviato
        for (auto& [id, f] : files)
            if (f.uploadSrc == path && !f.uploadDone && !f.uploading && (int64_t)sz == f.size) { fid = id; break; }
        if (!fid) {
            File f;
            f.id = nextFile++;
            f.name = fs::path(path).filename().string();
            f.size = (int64_t)sz;
            f.uploadSrc = path;
            files[f.id] = f;
            fid = f.id;
        }
        File& f = files[fid];
        f.uploading = true;
        Msg m;
        m.id = nextMsg++;
        m.chatId = chat;
        m.date = 1793900000 + m.id;
        m.kind = "messageDocument";
        m.fileId = fid;
        if (c.contains("caption")) m.caption = c["caption"].value("text", "");
        int64_t tempId = 900000000 + m.id;
        pending[tempId] = m;
        json tmp = msgJson(m);
        tmp["id"] = tempId;
        tmp["sending_state"] = {{"@type", "messageSendingStatePending"}};
        if (earlySendUpdate) {
            // esito prima della risposta: caricamento istantaneo
            f.uploaded = f.size;
            finishUpload(f);
            reply(req, tmp);
        } else {
            reply(req, tmp);
        }
    } else if (t == "createNewSupergroupChat") {
        std::string title = req.value("title", "");
        if (title.empty()) return error(req, 400, "Title must be non-empty");
        static int64_t nextChat = -2001;
        Chat c{nextChat--, title, req.value("is_channel", false) ? "channel" : "group", "chatMemberStatusCreator"};
        chats.push_back(c);
        announceChat(c);
        reply(req, {{"@type", "chat"}, {"id", c.id}, {"title", c.title},
                    {"type", {{"@type", "chatTypeSupergroup"}, {"supergroup_id", sgId(c.id)}, {"is_channel", c.type == "channel"}}}});
    } else if (t == "deleteChat" || t == "leaveChat") {
        int64_t id = req.value("chat_id", (int64_t)0);
        Chat* c = findChat(id);
        if (!c || c->type == "private") return error(req, 400, "Chat not found");
        if (t == "deleteChat" && c->status != "chatMemberStatusCreator") return error(req, 400, "CHAT_ADMIN_REQUIRED");
        reply(req, {{"@type", "ok"}});
        dropChat(id);
    } else if (t == "deleteMessages") {
        int64_t chat = req.value("chat_id", (int64_t)0);
        Chat* c = findChat(chat);
        bool allowed = chat == meId || (c && c->status != "chatMemberStatusMember");
        if (!allowed) return error(req, 400, "Message can't be deleted");
        json ids = json::array();
        for (auto& v : req["message_ids"]) {
            int64_t id = v.get<int64_t>();
            auto pit = pending.find(id);
            if (pit != pending.end()) {
                files[pit->second.fileId].uploading = false;
                files[pit->second.fileId].stalled = false;  // il parziale resta, per la ripresa
                pending.erase(pit);
                ids.push_back(id);
                ++pendingDeleted;
                continue;
            }
            auto it = msgs.find(id);
            if (it != msgs.end() && it->second.chatId == chat) { msgs.erase(it); ids.push_back(id); }
        }
        reply(req, {{"@type", "ok"}});
        push({{"@type", "updateDeleteMessages"}, {"chat_id", chat}, {"message_ids", ids},
              {"is_permanent", true}, {"from_cache", false}});
    } else {
        error(req, 400, "Metodo non simulato: " + t);
    }
}

// un passo di simulazione: avanza download/upload attivi
bool step() {
    bool any = false;
    if (qrScanCountdown > 0 && --qrScanCountdown == 0 &&
        authState == "authorizationStateWaitOtherDeviceConfirmation") {
        becomeReady();
        any = true;
    }
    for (auto& [id, f] : files) {
        if (f.downloading && !f.stallUsed && stallAfter >= 0 && f.downloaded >= stallAfter && !f.stalled) f.stalled = true;
        if (f.downloading && f.stalled) continue;  // come una connessione appesa: nessun aggiornamento
        if (f.downloading) {
            any = true;
            f.downloaded = std::min(f.size, f.downloaded + chunk);
            if (f.downloaded == f.size) {
                f.downloading = false;
                f.localDone = true;
                f.localPath = (fs::path(filesDir) / ("dl_" + std::to_string(f.id) + "_" + f.name)).string();
                std::string data = f.content;
                if (f.corrupt && !data.empty()) data[data.size() / 2] ^= 0x5A;
                std::ofstream(f.localPath, std::ios::binary).write(data.data(), (std::streamsize)data.size());
            }
            push({{"@type", "updateFile"}, {"file", fileJson(f)}});
        }
        if (f.uploading && !f.stallUsed && uploadStallAfter >= 0 && f.uploaded >= uploadStallAfter) {
            f.stallUsed = true;    // una sola volta per file: dopo un nuovo invio riparte
            f.stalled = true;
        }
        if (f.uploading && f.stalled) continue;
        if (f.uploading) {
            any = true;
            f.uploaded = std::min(f.size, f.uploaded + chunk);
            if (f.uploaded == f.size) finishUpload(f);
            else push({{"@type", "updateFile"}, {"file", fileJson(f)}});
        }
    }
    return any;
}

} // namespace fake

using namespace fake;

extern "C" int td_create_client_id(void) {
    std::lock_guard<std::mutex> g(mu);
    seed();
    curClient = ++clientCounter;
    return curClient;
}

extern "C" void td_send(int client_id, const char* request) {
    std::lock_guard<std::mutex> g(mu);
    if (client_id != curClient) return;
    json req = json::parse(request, nullptr, false);
    if (req.is_discarded()) return;
    handle(req);
}

extern "C" const char* td_receive(double timeout) {
    static thread_local std::string last;
    std::unique_lock<std::mutex> g(mu);
    if (out.empty()) {
        static int delayMs = std::getenv("TF_FAKE_DELAY") ? std::atoi(std::getenv("TF_FAKE_DELAY")) : 0;
        if (!step()) cv.wait_for(g, std::chrono::milliseconds((int)(std::min(timeout, 0.02) * 1000)));
        else if (delayMs) { g.unlock(); std::this_thread::sleep_for(std::chrono::milliseconds(delayMs)); g.lock(); }
    }
    if (out.empty()) return nullptr;
    last = out.front();
    out.pop_front();
    return last.c_str();
}

extern "C" const char* td_execute(const char*) { return R"({"@type":"ok"})"; }

// ---- comandi per i test
namespace fakectl {
void setStallAfter(int64_t b) { std::lock_guard<std::mutex> g(mu); stallAfter = (int)b; }
void setEarlySendUpdate(bool on) { std::lock_guard<std::mutex> g(mu); earlySendUpdate = on; }
int chatCount() { std::lock_guard<std::mutex> g(mu); return (int)chats.size(); }
int messagesIn(int64_t chat) { std::lock_guard<std::mutex> g(mu); int n = 0; for (auto& kv : msgs) if (kv.second.chatId == chat) ++n; return n; }
void setUploadLimit(int64_t b) { std::lock_guard<std::mutex> g(mu); uploadLimit = b; }
void setPremium(bool p) {
    std::lock_guard<std::mutex> g(mu);
    premium = p;
    push({{"@type", "updateUser"}, {"user", userJson()}});
}
void corruptPartOfGroup(int64_t chat, int partIndex, bool on) {
    std::lock_guard<std::mutex> g(mu);
    std::string tag = " p" + std::to_string(partIndex) + "/";
    for (auto& [id, m] : msgs)
        if (m.chatId == chat && m.caption.find(tag) != std::string::npos) files[m.fileId].corrupt = on;
}
void deletePartOfGroup(int64_t chat, int partIndex) {
    std::lock_guard<std::mutex> g(mu);
    std::string tag = " p" + std::to_string(partIndex) + "/";
    for (auto it = msgs.begin(); it != msgs.end(); ++it)
        if (it->second.chatId == chat && it->second.caption.find(tag) != std::string::npos) { msgs.erase(it); return; }
}
// gruppo di 2 parti con nomi file e didascalia a scelta (per provare il nome ricomposto)
void addGroup(int64_t chat, const std::string& part1, const std::string& part2, const std::string& nameLine,
              const std::string& gid, const std::string& content) {
    std::lock_guard<std::mutex> g(mu);
    size_t half = content.size() / 2;
    std::string pieces[2] = {content.substr(0, half), content.substr(half)};
    std::string names[2] = {part1, part2};
    for (int i = 0; i < 2; ++i) {
        File f;
        f.id = nextFile++;
        f.name = names[i];
        f.content = pieces[i];
        f.size = (int64_t)pieces[i].size();
        f.sentInMessage = true;
        files[f.id] = f;
        std::string cap;
        if (!nameLine.empty()) cap = nameLine + " \xC2\xB7 parte " + std::to_string(i + 1) + " di 2\n";
        cap += "#tf" + gid + " p" + std::to_string(i + 1) + "/2 s" + std::to_string(content.size());
        addMsg(chat, "messageDocument", f.id, cap);
    }
}
std::string expectedContent(const std::string& name) {
    std::lock_guard<std::mutex> g(mu);
    for (auto& [id, f] : files) if (f.name == name) return f.content;
    return {};
}
std::vector<std::string> captionsIn(int64_t chat) {
    std::lock_guard<std::mutex> g(mu);
    std::vector<std::string> v;
    for (auto& [id, m] : msgs) if (m.chatId == chat && !m.caption.empty()) v.push_back(m.caption);
    return v;
}
int64_t maxUploadedFileSize(int64_t chat) {
    std::lock_guard<std::mutex> g(mu);
    int64_t mx = 0;
    for (auto& [id, m] : msgs) if (m.chatId == chat && m.fileId) mx = std::max(mx, files[m.fileId].size);
    return mx;
}
void setChunk(int64_t c) { std::lock_guard<std::mutex> g(mu); chunk = c; }
void setUploadStallAfter(int64_t b) { std::lock_guard<std::mutex> g(mu); uploadStallAfter = b; }
int pendingDeletedCount() { std::lock_guard<std::mutex> g(mu); return pendingDeleted; }
int pendingCount() { std::lock_guard<std::mutex> g(mu); return (int)pending.size(); }
} // namespace fakectl
