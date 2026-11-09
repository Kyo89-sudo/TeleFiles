// TeleFiles - logica dell'applicazione: account, chat, elenco file, coda trasferimenti.
// Portabile (nessuna dipendenza da Win32). Thread-safe: tutti i metodi pubblici
// possono essere chiamati dal thread dell'interfaccia.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "Sha256.h"
#include "TdClient.h"

namespace tf {

enum class AuthState { Starting, WaitPhone, WaitCode, WaitPassword, WaitQr, Ready, LoggingOut, Error };

struct AuthInfo {
    AuthState state = AuthState::Starting;
    std::string qrLink;        // tg://login?token=...
    std::string codeInfo;      // dove è stato inviato il codice
    std::string passwordHint;
    std::string error;
    bool busy = false;         // richiesta in corso
};

struct Profile {
    int64_t userId = 0;
    std::string firstName, lastName, username, phone;
    bool premium = false;
    std::string photoPath;     // foto profilo piccola, se scaricata
    bool valid = false;
};

enum class ChatKind { Saved, Private, Group, Channel, Bot };

struct Chat {
    int64_t id = 0;
    std::string title;
    ChatKind kind = ChatKind::Private;
    int64_t order = 0;
    int unread = 0;
    int64_t supergroupId = 0, basicGroupId = 0;
    bool owner = false;      // creatore: può eliminare il canale/gruppo per tutti
    bool admin = false;      // può eliminare i messaggi degli altri
};

enum class FileKind { Document, Video, Audio, Photo };
enum class Filter { All, Documents, Video, Audio, Photos };

struct PartRef {
    int index = 0;          // 1-based
    int64_t msgId = 0;
    int32_t fileId = 0;
    int64_t size = 0;
    std::string sha;
};

// Una riga dell'elenco file: un file normale oppure un gruppo di parti divise.
struct FileEntry {
    std::string key;        // "chat:msg" oppure "chat:g<gid>"
    int64_t chatId = 0;
    int64_t msgId = 0;      // per i gruppi: messaggio della parte più recente
    int32_t fileId = 0;     // 0 per i gruppi
    std::string name;
    int64_t size = 0;
    int64_t date = 0;       // unix time
    FileKind kind = FileKind::Document;
    bool isGroup = false;
    std::string gid, fullSha;
    int partCount = 0;
    std::map<int, PartRef> parts;
    std::string folder;     // sottocartella relativa (caricamento di cartelle), "" se nessuna
};

// Avviso da mostrare nell'interfaccia (esito di creazione/eliminazione, errori di permessi...)
struct Notice {
    std::string text;
    bool error = false;
};

enum class Direction { Download, Upload };
enum class TStatus { Queued, Preparing, Active, Paused, Joining, Verifying, Done, Error };

struct TransferView {
    int id = 0;
    Direction dir = Direction::Download;
    TStatus status = TStatus::Queued;
    std::string name, chatTitle, entryKey, error, path;
    int64_t total = 0, done = 0;
    double speed = 0;       // byte/s
    int part = 0, partCount = 0;
    double prep = -1;       // 0..1 durante l'estrazione di una parte, altrimenti -1
};

struct Settings {
    std::string downloadDir;
    int maxParallel = 3;
    bool keepParts = false;
    int theme = 0;           // 0 sistema, 1 chiaro, 2 scuro
    int transfersHeight = 210;   // altezza del pannello trasferimenti (DIP)
    bool transfersCollapsed = false;
    int sortColumn = 0;          // 0 data, 1 nome, 2 dimensione
    bool sortDescending = true;
};

class Engine {
public:
    using Notify = std::function<void()>;

    struct Config {
        int apiId = 0;
        std::string apiHash;
        std::string dataDir;     // database TDLib, impostazioni, coda
        std::string tempDir;     // parti temporanee in upload
        std::string defaultDownloadDir;
        // versione portable: i percorsi dentro questa cartella vengono salvati relativi,
        // così la chiavetta funziona anche se cambia la lettera dell'unità
        std::string portableBase;
        std::string appVersion = "0.1.0";
        std::string deviceModel = "Windows PC";
        std::string systemVersion = "Windows";
        std::string language = "it";
    };

    explicit Engine(Config cfg);
    ~Engine();

    void setNotify(Notify n) { notify_ = std::move(n); }
    void start();
    void shutdown();             // salva la coda e chiude TDLib
    void tick();                 // da chiamare ~1 volta al secondo (velocità, scheduler, salvataggio)

    // --- accesso
    AuthInfo auth();
    void submitPhone(const std::string& phone);
    void submitCode(const std::string& code);
    void submitPassword(const std::string& pwd);
    void requestQr();
    void usePhoneLogin();        // dal QR torna al numero
    void logout();

    // --- profilo
    Profile profile();
    int64_t uploadLimit();       // 2000 MiB o 4000 MiB

    // --- chat ed elenco file
    std::vector<Chat> chats();
    int64_t selectedChat();
    void selectChat(int64_t chatId);
    void setFilter(Filter f);
    void setQuery(const std::string& q);
    Filter filter();
    std::string query();
    std::vector<FileEntry> entries();
    int64_t totalCount();        // numero di file nella chat (filtro corrente), -1 se ignoto
    bool listLoading();
    bool listHasMore();
    void loadMore();

    // --- trasferimenti
    int download(const std::string& entryKey);
    int upload(int64_t chatId, const std::string& path, const std::string& folder = {});
    // Carica tutti i file di una cartella (anche sottocartelle); restituisce quanti file ha accodato.
    // Il percorso relativo finisce nella didascalia e viene ricreato al download.
    int uploadFolder(int64_t chatId, const std::string& dir);
    void pause(int id);
    void resume(int id);
    void cancel(int id);
    void pauseAll();
    void resumeAll();
    int cancelAll();               // annulla tutti i trasferimenti non completati; restituisce quanti
    void clearCompleted();
    std::vector<TransferView> transfers();
    // stato della riga di un file: -1 nessun trasferimento, altrimenti id
    int transferForEntry(const std::string& key);
    bool alreadyDownloaded(const FileEntry& e);
    void totalSpeed(double& down, double& up);

    // --- canali e file
    void createChannel(const std::string& title, const std::string& description, bool asGroup = false);
    void deleteChat(int64_t chatId);          // creatore: elimina per tutti; altrimenti esce
    void deleteEntries(const std::vector<std::string>& keys);  // elimina i messaggi (anche tutte le parti)
    bool popNotice(Notice& n);

    Settings settings();
    void setSettings(const Settings& s);

    // Per i test: limite di divisione forzato (0 = automatico)
    void setSplitLimitForTesting(int64_t bytes) { std::lock_guard<std::recursive_mutex> g(mu_); testLimit_ = bytes; }
    // Dopo quanti ms senza progressi un trasferimento attivo viene riavviato (predefinito 45 s)
    void setStallTimeoutForTesting(int ms) { std::lock_guard<std::recursive_mutex> g(mu_); stallMs_ = ms; }

private:
    struct Transfer;
    friend struct Transfer;

    void onUpdate(const json& u);
    void onAuthState(const json& st);
    void onFile(const json& f);
    void onUser(const json& u);
    void onMemberStatus(Chat& c, const json& status);
    void removeMessages(int64_t chatId, const std::set<int64_t>& ids);
    void removeChatLocal(int64_t chatId);
    void notice(const std::string& text, bool error = false);
    void log(const std::string& line);   // Data/telefiles.log: per capire dove si ferma qualcosa
    std::string entryPath(const FileEntry& e);
    void sendParams();
    void afterReady();
    void loadChatList();
    void reloadList();
    void requestPage();
    void handlePage(uint64_t gen, const json& res);
    void addMessage(const json& m, std::vector<FileEntry>& out);
    void mergeEntry(const FileEntry& e);
    void fetchCount();

    Transfer* find(int id);
    bool anySending();
    void schedule();
    void startTransfer(Transfer& t);
    void startDownloadPart(Transfer& t);
    void onDownloadPartDone(Transfer& t, const std::string& localPath);
    void finishDownload(Transfer& t);
    void startUploadStep(Transfer& t);
    void preparePart(Transfer& t);
    void uploadPath(Transfer& t, const std::string& path);
    void stopPendingUpload(Transfer& t);
    void onSendSucceeded(int64_t oldMsgId, const json& msg);
    void onSendFailed(int64_t oldMsgId, const std::string& err);
    void fail(Transfer& t, const std::string& err);
    void collectGroupParts(Transfer& t);
    void runWorker(int tid, std::function<void(Transfer&, std::function<bool(int64_t)>)> work);
    void saveQueue();
    void loadQueue();
    void restoreTransfers();
    void saveSettings();
    void loadSettings();
    void changed();

    Config cfg_;
    std::unique_ptr<TdClient> td_;
    Notify notify_;
    std::recursive_mutex mu_;

    AuthInfo auth_;
    Profile profile_;
    std::map<int64_t, Chat> chatMap_;
    std::vector<int64_t> chatOrder_;
    int64_t savedChatId_ = 0;

    int64_t selChat_ = 0;
    Filter filter_ = Filter::All;
    std::string query_;
    std::vector<FileEntry> entries_;
    std::map<std::string, size_t> entryIndex_;
    uint64_t listGen_ = 0;
    int64_t nextFromMsg_ = 0;
    bool searchNames_ = false;
    bool listLoading_ = false, listMore_ = false;
    int64_t totalCount_ = -1;

    std::vector<std::unique_ptr<Transfer>> transfers_;
    int nextTid_ = 1;
    std::map<int32_t, int> fileToTransfer_;     // file id TDLib -> trasferimento
    std::map<int64_t, int> sendingMsg_;         // id messaggio temporaneo -> trasferimento
    Settings settings_;
    int64_t testLimit_ = 0;
    int stallMs_ = 45000;
    std::mutex logMu_;
    std::map<std::string, bool> dlCache_;       // file già presente nella cartella download
    std::set<int32_t> photoFiles_;
    std::map<int64_t, json> supergroupStatus_, basicGroupStatus_;
    std::vector<Notice> notices_;
    std::map<int64_t, json> earlySend_;   // esiti di invio arrivati prima della risposta
    std::atomic<int> workers_{0};
    bool queueDirty_ = false;
    bool restored_ = false;
    bool recreateClient_ = false;
    bool shuttingDown_ = false;
    json pendingRestore_;
};

std::string formatSize(int64_t bytes);       // "6,1 GB"
std::string formatSpeed(double bps);         // "9,4 MB/s"
std::string formatEta(int64_t remaining, double bps);

} // namespace tf
