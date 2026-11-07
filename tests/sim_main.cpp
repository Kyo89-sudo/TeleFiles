// Simulazione completa di TeleFiles contro il server finto (fake_td.cpp).
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <thread>

#include "../src/core/Engine.h"
#include "../src/core/FileIO.h"
#include "../src/core/Split.h"

namespace fakectl {
void setUploadLimit(int64_t b);
void setPremium(bool p);
void corruptPartOfGroup(int64_t chat, int partIndex, bool on);
void deletePartOfGroup(int64_t chat, int partIndex);
std::string expectedContent(const std::string& name);
std::vector<std::string> captionsIn(int64_t chat);
int64_t maxUploadedFileSize(int64_t chat);
void setChunk(int64_t c);
void setEarlySendUpdate(bool on);
int chatCount();
int messagesIn(int64_t chat);
void setStallAfter(int64_t b);
void setUploadStallAfter(int64_t b);
int pendingDeletedCount();
int pendingCount();
void addGroup(int64_t chat, const std::string& part1, const std::string& part2, const std::string& nameLine,
              const std::string& gid, const std::string& content);
}

using namespace tf;
namespace fs = std::filesystem;

static int failures = 0, checks = 0;
#define CHECK(c, msg) do { ++checks; if (c) std::printf("  ok   %s\n", msg); \
    else { ++failures; std::printf("  FAIL %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while (0)

static bool waitFor(std::function<bool()> cond, int ms = 15000, Engine* e = nullptr) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    auto lastTick = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (e && std::chrono::steady_clock::now() - lastTick > std::chrono::milliseconds(50)) {
            e->tick();
            lastTick = std::chrono::steady_clock::now();
        }
    }
    return cond();
}

static std::string readAll(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

static TransferView tv(Engine& e, int id) {
    for (auto& t : e.transfers()) if (t.id == id) return t;
    return {};
}

static const FileEntry* findEntry(const std::vector<FileEntry>& v, const std::string& name) {
    for (auto& x : v) if (x.name == name) return &x;
    return nullptr;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::string root = (fs::temp_directory_path() / "telefiles_sim").string();
    fs::remove_all(root);
    Engine::Config cfg;
    cfg.apiId = 12345;
    cfg.apiHash = "0123456789abcdef";
    cfg.dataDir = root + "/data";
    cfg.tempDir = root + "/temp";
    cfg.defaultDownloadDir = root + "/Download";

    const int64_t KB = 1024;
    std::string bigName = "archivio-progetti.7z";
    std::string bigSrc = root + "/" + bigName;
    std::string original;
    int64_t savedChat = 0;

    {
        Engine e(cfg);
        e.start();

        std::printf("\n[1] Accesso con numero, codice e password\n");
        CHECK(waitFor([&] { return e.auth().state == AuthState::WaitPhone; }, 5000, &e), "richiesta del numero");
        e.submitPhone("3331234567");
        CHECK(waitFor([&] { return !e.auth().error.empty(); }, 5000, &e), "numero senza prefisso rifiutato");
        std::printf("       messaggio: %s\n", e.auth().error.c_str());
        e.submitPhone("+393331234567");
        CHECK(waitFor([&] { return e.auth().state == AuthState::WaitCode; }, 5000, &e), "richiesta del codice");
        std::printf("       %s\n", e.auth().codeInfo.c_str());
        e.submitCode("11111");
        CHECK(waitFor([&] { return e.auth().error == "Codice errato."; }, 5000, &e), "codice errato segnalato");
        e.submitCode("12345");
        CHECK(waitFor([&] { return e.auth().state == AuthState::WaitPassword; }, 5000, &e), "richiesta password 2FA");
        CHECK(e.auth().passwordHint == "il solito", "suggerimento password");
        e.submitPassword("segreta");
        CHECK(waitFor([&] { return e.auth().state == AuthState::Ready; }, 5000, &e), "accesso completato");

        std::printf("\n[2] Profilo, chat ed elenco file\n");
        CHECK(waitFor([&] { return e.profile().valid && e.chats().size() == 4; }, 5000, &e), "profilo e 4 chat caricati");
        auto p = e.profile();
        std::printf("       %s %s @%s premium=%d limite=%s\n", p.firstName.c_str(), p.lastName.c_str(),
                    p.username.c_str(), p.premium, formatSize(e.uploadLimit()).c_str());
        CHECK(!p.premium && e.uploadLimit() == kLimitFree, "account gratuito: limite 2000 MiB");
        auto ch = e.chats();
        CHECK(ch[0].kind == ChatKind::Saved && ch[0].title == "Messaggi salvati", "Messaggi salvati in cima");
        savedChat = ch[0].id;
        CHECK(waitFor([&] { return e.selectedChat() == savedChat && e.entries().size() == 1; }, 5000, &e),
              "Messaggi salvati aperto con 1 file");

        e.selectChat(-1001);
        CHECK(waitFor([&] { return !e.listLoading() && e.entries().size() == 5; }, 5000, &e),
              "Linux ISO: 5 file (i 30 messaggi di testo sono saltati)");
        CHECK(waitFor([&] { return e.totalCount() == 5; }, 5000, &e), "conteggio file della chat");
        e.setQuery("ubuntu");
        CHECK(waitFor([&] { return !e.listLoading() && e.entries().size() == 3; }, 5000, &e), "ricerca 'ubuntu': 3 risultati");
        e.setFilter(Filter::Video);
        CHECK(waitFor([&] { return !e.listLoading() && e.entries().size() == 1; }, 5000, &e), "filtro Video: 1 risultato");
        e.setFilter(Filter::Documents);
        CHECK(waitFor([&] { return !e.listLoading() && e.entries().size() == 2; }, 5000, &e), "filtro Documenti: 2 risultati");
        e.setQuery("");
        e.setFilter(Filter::All);
        waitFor([&] { return !e.listLoading() && e.entries().size() == 5; }, 5000, &e);

        std::printf("\n[3] Download con pausa e ripresa\n");
        fakectl::setChunk(16 * KB);
        auto ents = e.entries();
        const FileEntry* iso = findEntry(ents, "ubuntu-26.04-desktop-amd64.iso");
        CHECK(iso != nullptr, "file trovato");
        int d1 = e.download(iso->key);
        CHECK(waitFor([&] { auto t = tv(e, d1); return t.status == TStatus::Active && t.done > 200 * KB; }, 5000, &e),
              "download avviato");
        e.pause(d1);
        int64_t at = tv(e, d1).done;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(tv(e, d1).status == TStatus::Paused && tv(e, d1).done <= at + 16 * KB, "in pausa: l'avanzamento si ferma");
        std::printf("       fermo a %s di %s\n", formatSize(tv(e, d1).done).c_str(), formatSize(tv(e, d1).total).c_str());
        e.resume(d1);
        CHECK(waitFor([&] { return tv(e, d1).status == TStatus::Done; }, 15000, &e), "ripreso e completato");
        std::string dest = cfg.defaultDownloadDir + "/ubuntu-26.04-desktop-amd64.iso";
        CHECK(readAll(dest) == fakectl::expectedContent("ubuntu-26.04-desktop-amd64.iso"), "contenuto identico all'originale");
        CHECK(e.alreadyDownloaded(*iso), "riga segnata come 'Scaricato'");

        std::printf("\n[4] Download paralleli limitati dall'impostazione\n");
        Settings s = e.settings();
        s.maxParallel = 2;
        e.setSettings(s);
        ents = e.entries();
        std::vector<int> ids;
        for (auto& x : ents) if (x.name != "ubuntu-26.04-desktop-amd64.iso") ids.push_back(e.download(x.key));
        int maxActive = 0;
        waitFor([&] {
            int a = 0, done = 0;
            for (int id : ids) { auto t = tv(e, id); if (t.status == TStatus::Active) ++a; if (t.status == TStatus::Done) ++done; }
            maxActive = std::max(maxActive, a);
            return done == (int)ids.size();
        }, 20000, &e);
        CHECK(maxActive <= 2 && maxActive >= 1, "mai più di 2 download insieme");
        bool allOk = true;
        for (int id : ids) allOk = allOk && tv(e, id).status == TStatus::Done;
        CHECK(allOk, "4 download completati");
        e.clearCompleted();
        CHECK(e.transfers().empty(), "pulizia dei completati");

        std::printf("\n[5] Upload con divisione automatica (limite simulato 1 MiB)\n");
        fakectl::setUploadLimit(1024 * KB);
        e.setSplitLimitForTesting(1024 * KB);
        {
            std::mt19937 g(42);
            original.resize(3500 * KB + 777);
            for (auto& c : original) c = (char)(g() & 0xFF);
            std::ofstream(bigSrc, std::ios::binary).write(original.data(), (std::streamsize)original.size());
        }
        e.selectChat(savedChat);
        int u1 = e.upload(savedChat, bigSrc);
        CHECK(waitFor([&] { auto t = tv(e, u1); return t.status == TStatus::Active && t.part == 2; }, 10000, &e),
              "caricamento in parti avviato");
        std::printf("       parti previste: %d\n", tv(e, u1).partCount);
        CHECK(tv(e, u1).partCount == 4, "3,4 MB divisi in 4 parti da 1 MiB");
        e.pause(u1);
        CHECK(waitFor([&] { return tv(e, u1).status == TStatus::Paused; }, 5000, &e), "upload in pausa");
        int64_t uat = tv(e, u1).done;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(tv(e, u1).done == uat, "in pausa non invia altri dati");
        CHECK(fakectl::pendingDeletedCount() >= 1 && fakectl::pendingCount() == 0,
              "la pausa ritira il messaggio non ancora inviato");
        e.resume(u1);
        CHECK(waitFor([&] { return tv(e, u1).status == TStatus::Done; }, 20000, &e), "upload completato");
        auto caps = fakectl::captionsIn(savedChat);
        CHECK(caps.size() == 4, "4 messaggi con didascalia di parte (nessun doppione dopo la pausa)");
        CHECK(fakectl::pendingCount() == 0, "nessun messaggio rimasto in sospeso");
        if (!caps.empty()) std::printf("       didascalia:\n         %s\n", caps[0].substr(0, caps[0].find('\n', caps[0].find('\n') + 1)).c_str());
        CHECK(fakectl::maxUploadedFileSize(savedChat) <= 1024 * KB, "nessuna parte supera il limite");
        {
            int withFull = 0;
            bool lastHasFull = false;
            for (auto& c : caps) {
                SplitTag tg;
                if (parseCaption(c, tg) && !tg.fullSha.empty()) { ++withFull; lastHasFull = tg.index == tg.count; }
            }
            CHECK(withFull == 1 && lastHasFull, "impronta del file intero solo nell'ultima parte (niente lettura preliminare)");
            SplitTag tg;
            for (auto& c : caps) if (parseCaption(c, tg) && !tg.fullSha.empty()) break;
            CHECK(tg.fullSha == Sha256::fileHex(bigSrc), "impronta calcolata durante l'estrazione corretta");
        }
        CHECK(!fs::exists(cfg.tempDir) || fs::is_empty(cfg.tempDir), "parti temporanee cancellate");

        std::printf("\n[6] Il gruppo appare come un file solo\n");
        CHECK(waitFor([&] { return findEntry(e.entries(), bigName) != nullptr; }, 5000, &e), "riga unica nella lista");
        auto grpList = e.entries();
        const FileEntry* grp = findEntry(grpList, bigName);
        if (grp) {
            CHECK(grp->isGroup && grp->partCount == 4 && grp->size == (int64_t)original.size(), "4 parti, dimensione totale corretta");
            std::printf("       %s  %s  %d parti\n", grp->name.c_str(), formatSize(grp->size).c_str(), grp->partCount);
        }

        std::printf("\n[7] Download del gruppo: parte corrotta, poi ricomposizione\n");
        fakectl::corruptPartOfGroup(savedChat, 3, true);
        int d2 = e.download(grp->key);
        CHECK(waitFor([&] { return tv(e, d2).status == TStatus::Error; }, 20000, &e), "parte 3 corrotta individuata");
        std::printf("       errore: %s\n", tv(e, d2).error.c_str());
        CHECK(tv(e, d2).part == 3, "si ferma alla parte 3 (le prime 2 restano)");
        fakectl::corruptPartOfGroup(savedChat, 3, false);
        e.resume(d2);
        CHECK(waitFor([&] { return tv(e, d2).status == TStatus::Done; }, 20000, &e), "riprende dalla parte 3 e completa");
        std::string got = readAll(tv(e, d2).path);
        CHECK(got == original, "file ricomposto identico all'originale (byte per byte)");
        CHECK(Sha256::fileHex(tv(e, d2).path) == Sha256::fileHex(bigSrc), "SHA-256 identico");
        CHECK(!fs::exists(tv(e, d2).path + ".tfpart"), "nessun file parziale residuo");
        std::printf("       salvato in %s\n", fs::path(tv(e, d2).path).filename().string().c_str());

        std::printf("\n[8] Interruzione e riavvio dell'app a metà gruppo\n");
        fakectl::setChunk(8 * KB);
        int d3 = e.download(grp->key);
        CHECK(waitFor([&] { auto t = tv(e, d3); return t.part == 3 && t.status == TStatus::Active; }, 20000, &e),
              "secondo download alla parte 3");
        e.pause(d3);
        waitFor([&] { return tv(e, d3).status == TStatus::Paused; }, 5000, &e);
        e.shutdown();
    }
    {
        Engine e(cfg);
        e.setSplitLimitForTesting(1024 * KB);
        e.start();
        CHECK(waitFor([&] { return e.auth().state == AuthState::Ready; }, 5000, &e), "sessione salvata: niente login");
        CHECK(waitFor([&] { return e.transfers().size() == 1; }, 5000, &e), "coda ripristinata");
        auto t = e.transfers()[0];
        CHECK(t.status == TStatus::Paused && t.part == 3, "ripristinato in pausa alla parte 3");
        e.resume(t.id);
        CHECK(waitFor([&] { return tv(e, t.id).status == TStatus::Done; }, 30000, &e), "ricalcola l'hash e completa");
        CHECK(readAll(tv(e, t.id).path) == original, "file identico all'originale");
        std::printf("       salvato come %s\n", fs::path(tv(e, t.id).path).filename().string().c_str());

        std::printf("\n[9] Parte mancante nella chat\n");
        fakectl::deletePartOfGroup(savedChat, 2);
        e.selectChat(-1002);
        waitFor([&] { return !e.listLoading(); }, 3000, &e);
        e.selectChat(savedChat);
        CHECK(waitFor([&] { return findEntry(e.entries(), bigName) != nullptr; }, 5000, &e), "gruppo ancora elencato");
        auto l4 = e.entries();
        int d4 = e.download(findEntry(l4, bigName)->key);
        CHECK(waitFor([&] { return tv(e, d4).status == TStatus::Error; }, 5000, &e), "download rifiutato");
        std::printf("       errore: %s\n", tv(e, d4).error.c_str());

        std::printf("\n[10] Premium\n");
        fakectl::setPremium(true);
        e.setSplitLimitForTesting(0);
        CHECK(waitFor([&] { return e.profile().premium; }, 5000, &e), "passaggio a Premium rilevato");
        CHECK(e.uploadLimit() == kLimitPremium, "limite portato a 4000 MiB");

        std::printf("\n[11] Logout e accesso con codice QR\n");
        e.logout();
        CHECK(waitFor([&] { return e.auth().state == AuthState::WaitPhone; }, 8000, &e), "disconnesso, schermata di accesso");
        CHECK(e.transfers().empty() && e.chats().empty(), "dati del vecchio account rimossi");
        e.requestQr();
        CHECK(waitFor([&] { return e.auth().state == AuthState::WaitQr; }, 5000, &e), "QR mostrato");
        std::printf("       link: %s\n", e.auth().qrLink.c_str());
        CHECK(waitFor([&] { return e.auth().state == AuthState::Ready; }, 8000, &e), "QR scansionato: accesso");
        e.shutdown();
    }

    std::printf("\n[12] Portable: la cartella cambia posizione (es. nuova lettera della chiavetta)\n");
    {
        std::string p1 = root + "/usbE/TeleFiles", p2 = root + "/usbF/TeleFiles";
        fs::create_directories(p1);
        auto mk = [&](const std::string& base) {
            Engine::Config c = cfg;
            c.portableBase = base;
            c.dataDir = base + "/Data";
            c.tempDir = base + "/Data/upload-temp";
            c.defaultDownloadDir = base + "/Download";
            return c;
        };
        std::string gkey;
        {
            Engine e(mk(p1));
            e.setSplitLimitForTesting(1024 * KB);
            e.start();
            CHECK(waitFor([&] { return e.auth().state == AuthState::Ready; }, 5000, &e), "sessione attiva");
            Settings st = e.settings();
            st.theme = 2;
            e.setSettings(st);
            e.selectChat(savedChat);
            waitFor([&] { return findEntry(e.entries(), "appunti.pdf") != nullptr; }, 5000, &e);
            // carica un nuovo file diviso dalla cartella portable
            std::string src = p1 + "/video-vacanze.mkv";
            std::ofstream(src, std::ios::binary).write(original.data(), (std::streamsize)original.size());
            fakectl::setChunk(64 * KB);
            int u = e.upload(savedChat, src);
            CHECK(waitFor([&] { return tv(e, u).status == TStatus::Done; }, 20000, &e), "upload dalla chiavetta");
            CHECK(waitFor([&] { return findEntry(e.entries(), "video-vacanze.mkv") != nullptr; }, 5000, &e), "gruppo in lista");
            auto l = e.entries();
            gkey = findEntry(l, "video-vacanze.mkv")->key;
            fakectl::setChunk(8 * KB);
            int d = e.download(gkey);
            CHECK(waitFor([&] { auto t = tv(e, d); return t.part == 3 && t.status == TStatus::Active; }, 20000, &e), "download a metà (parte 3)");
            e.pause(d);
            waitFor([&] { return tv(e, d).status == TStatus::Paused; }, 5000, &e);
            e.shutdown();
        }
        std::string sj = readAll(p1 + "/Data/settings.json"), qj = readAll(p1 + "/Data/queue.json");
        CHECK(sj.find("\"download_dir\": \"Download\"") != std::string::npos, "settings.json: cartella download relativa");
        CHECK(qj.find(root) == std::string::npos, "queue.json senza percorsi assoluti");
        fs::create_directories(root + "/usbF");
        fs::rename(p1, p2);
        {
            Engine e(mk(p2));
            e.setSplitLimitForTesting(1024 * KB);
            e.start();
            CHECK(waitFor([&] { return e.auth().state == AuthState::Ready && e.transfers().size() == 1; }, 5000, &e),
                  "nuova posizione: sessione e coda ritrovate");
            CHECK(e.settings().theme == 2 && e.settings().downloadDir == p2 + "/Download", "impostazioni riportate sulla nuova posizione");
            auto t = e.transfers()[0];
            e.resume(t.id);
            CHECK(waitFor([&] { return tv(e, t.id).status == TStatus::Done; }, 30000, &e), "ripresa e completata");
            std::string out = tv(e, t.id).path;
            CHECK(out.rfind(p2 + "/Download/", 0) == 0, "salvato nella Download della nuova posizione");
            CHECK(readAll(out) == original, "file identico all'originale");
            e.shutdown();
        }
    }

    std::printf("\n[13] Canali, cartelle ed eliminazione\n");
    {
        Engine e(cfg);
        e.setSplitLimitForTesting(1024 * KB);
        e.start();
        CHECK(waitFor([&] { return e.auth().state == AuthState::Ready && e.chats().size() >= 4; }, 5000, &e), "pronto");
        auto drain = [&] { Notice n; std::string last; while (e.popNotice(n)) last = n.text; return last; };
        drain();
        auto chatById = [&](int64_t id) { for (auto& c : e.chats()) if (c.id == id) return c; return Chat{}; };
        CHECK(!chatById(-1001).owner && !chatById(-1001).admin, "Linux ISO: semplice iscritto");
        CHECK(chatById(-1002).admin && !chatById(-1002).owner, "Progetti lavoro: amministratore");

        int before = fakectl::chatCount();
        e.createChannel("Archivio TeleFiles", "file di prova");
        CHECK(waitFor([&] { return fakectl::chatCount() == before + 1 && e.chats().size() >= 5; }, 5000, &e), "canale creato");
        int64_t ch = 0;
        for (auto& c : e.chats()) if (c.title == "Archivio TeleFiles") ch = c.id;
        CHECK(ch != 0 && chatById(ch).owner && chatById(ch).kind == ChatKind::Channel, "sono il proprietario del canale");
        CHECK(e.selectedChat() == ch, "il nuovo canale viene aperto");
        CHECK(e.chats().size() > 1 && e.chats()[1].id == ch, "in cima all'elenco (dopo Messaggi salvati)");
        std::printf("       avviso: %s\n", drain().c_str());

        // cartella con sottocartelle; risposta di invio in anticipo (gara)
        fakectl::setEarlySendUpdate(true);
        fakectl::setChunk(64 * KB);
        std::string dir = root + "/Vacanze 2024";
        fs::create_directories(dir + "/Mare");
        std::ofstream(dir + "/note.txt") << "ciao";
        std::ofstream(dir + "/Mare/spiaggia.jpg", std::ios::binary).write(original.data(), 300 * KB);
        std::ofstream(dir + "/Mare/grande.bin", std::ios::binary).write(original.data(), (std::streamsize)original.size());
        std::ofstream(dir + "/Thumbs.db") << "x";
        int n = e.uploadFolder(ch, dir);
        CHECK(n == 3, "3 file in coda (Thumbs.db ignorato)");
        CHECK(waitFor([&] { int d = 0; for (auto& t : e.transfers()) if (t.dir == Direction::Upload && t.status == TStatus::Done) ++d; return d == 3; }, 30000, &e),
              "tutti caricati e segnati come completati (niente upload fermi al 100%)");
        fakectl::setEarlySendUpdate(false);
        CHECK(waitFor([&] { return e.entries().size() == 3; }, 5000, &e), "3 righe nel canale (il file diviso conta 1)");
        auto list = e.entries();
        const FileEntry* sp = findEntry(list, "spiaggia.jpg");
        const FileEntry* big = findEntry(list, "grande.bin");
        CHECK(sp && sp->folder == "Vacanze 2024/Mare", "percorso della cartella letto dalla didascalia");
        CHECK(big && big->isGroup && big->folder == "Vacanze 2024/Mare", "anche il file diviso conserva la cartella");
        int d1 = e.download(sp->key), d2 = e.download(big->key);
        CHECK(waitFor([&] { return tv(e, d1).status == TStatus::Done && tv(e, d2).status == TStatus::Done; }, 30000, &e), "riscaricati");
        std::string dl = e.settings().downloadDir;
        CHECK(readAll(dl + "/Vacanze 2024/Mare/spiaggia.jpg") == original.substr(0, 300 * KB), "struttura di cartelle ricreata");
        CHECK(readAll(dl + "/Vacanze 2024/Mare/grande.bin") == original, "file diviso ricomposto nella sua cartella");

        // eliminazione: file singolo e gruppo di parti (tutte le parti)
        int msgsBefore = fakectl::messagesIn(ch);
        e.deleteEntries({findEntry(e.entries(), "note.txt")->key});
        CHECK(waitFor([&] { return e.entries().size() == 2; }, 5000, &e), "file eliminato dalla lista");
        std::printf("       avviso: %s\n", drain().c_str());
        e.deleteEntries({findEntry(e.entries(), "grande.bin")->key});
        CHECK(waitFor([&] { return e.entries().size() == 1; }, 5000, &e), "gruppo eliminato dalla lista");
        CHECK(fakectl::messagesIn(ch) == msgsBefore - 5, "eliminate tutte e 4 le parti più il file singolo");

        // senza permessi
        e.selectChat(-1001);
        waitFor([&] { return !e.listLoading() && !e.entries().empty(); }, 5000, &e);
        size_t cnt = e.entries().size();
        drain();
        e.deleteEntries({e.entries()[0].key});
        std::string msg;
        CHECK(waitFor([&] { Notice nn; if (e.popNotice(nn)) msg = nn.text; return !msg.empty(); }, 5000, &e), "errore mostrato");
        std::printf("       avviso: %s\n", msg.c_str());
        CHECK(e.entries().size() == cnt && msg.find("permessi") != std::string::npos, "nessun file tolto, messaggio chiaro");

        // uscire da un canale non proprio, eliminare il proprio
        e.deleteChat(-1003);
        CHECK(waitFor([&] { return chatById(-1003).id == 0; }, 5000, &e), "uscito da Podcast Italia");
        std::printf("       avviso: %s\n", drain().c_str());
        e.selectChat(ch);
        e.deleteChat(ch);
        CHECK(waitFor([&] { return chatById(ch).id == 0; }, 5000, &e), "canale eliminato");
        CHECK(e.selectedChat() == savedChat, "si torna a Messaggi salvati");
        std::printf("       avviso: %s\n", drain().c_str());
        e.shutdown();
    }

    std::printf("\n[14] Download e upload che si bloccano a metà\n");
    {
        Engine e(cfg);
        e.setStallTimeoutForTesting(800);
        e.start();
        CHECK(waitFor([&] { return e.auth().state == AuthState::Ready; }, 5000, &e), "pronto");
        e.selectChat(-1001);
        CHECK(waitFor([&] { return !e.listLoading() && e.entries().size() == 5; }, 5000, &e), "lista caricata");
        fakectl::setChunk(16 * KB);
        fakectl::setStallAfter(300 * KB);  // il server smette di inviare dopo 300 KB
        auto l = e.entries();
        const FileEntry* f = findEntry(l, "fedora-43-workstation.iso");
        std::string dest = e.settings().downloadDir + "/fedora-43-workstation.iso";
        removeFile(dest);
        int d = e.download(f->key);
        CHECK(waitFor([&] { return tv(e, d).done >= 300 * KB; }, 10000, &e), "arriva al punto di blocco");
        CHECK(waitFor([&] { return tv(e, d).status == TStatus::Done; }, 15000, &e), "riavviato da solo e completato");
        CHECK(readAll(tv(e, d).path) == fakectl::expectedContent("fedora-43-workstation.iso"), "file corretto");
        fakectl::setStallAfter(-1);

        std::printf("\n[14a] Nome ed estensione del file ricomposto\n");
        {
            std::string mkv = std::string("\x1A\x45\xDF\xA3", 4) + std::string(200 * KB, 'v');
            std::string zip = std::string("PK\x03\x04", 4) + std::string(150 * KB, 'z');
            // nome della parte cambiato, ma la didascalia porta quello originale
            fakectl::addGroup(-1003, "document.001", "document.002", "concerto.mkv", "aaaaaaaaaaaa01", mkv);
            // nessun nome nella didascalia e parti senza estensione originale: si ricava dal contenuto
            fakectl::addGroup(-1003, "archivio.001", "archivio.002", "", "aaaaaaaaaaaa02", zip);
            e.selectChat(-1003);
            CHECK(waitFor([&] { auto v = e.entries(); return !e.listLoading() && findEntry(v, "concerto.mkv") && findEntry(v, "archivio"); }, 5000, &e), "gruppi elencati");
            auto l3 = e.entries();
            const FileEntry* g1 = findEntry(l3, "concerto.mkv");
            const FileEntry* g2 = findEntry(l3, "archivio");
            CHECK(g1 != nullptr, "nome preso dalla didascalia (concerto.mkv)");
            CHECK(g2 != nullptr, "gruppo senza nome in didascalia (archivio)");
            if (g1 && g2) {
                int a = e.download(g1->key), b = e.download(g2->key);
                CHECK(waitFor([&] { return tv(e, a).status == TStatus::Done && tv(e, b).status == TStatus::Done; }, 15000, &e),
                      "ricomposti");
                CHECK(fs::path(tv(e, a).path).filename() == "concerto.mkv" && readAll(tv(e, a).path) == mkv, "concerto.mkv corretto");
                CHECK(fs::path(tv(e, b).path).filename() == "archivio.zip" && readAll(tv(e, b).path) == zip,
                      "estensione .zip ricavata dal contenuto");
                std::printf("       salvati come %s e %s\n", fs::path(tv(e, a).path).filename().string().c_str(),
                            fs::path(tv(e, b).path).filename().string().c_str());
            }
            e.clearCompleted();
        }

        std::printf("\n[14b] Upload diviso che si blocca a metà parte\n");
        {
            fakectl::setUploadLimit(512 * KB);
            e.setSplitLimitForTesting(512 * KB);
            std::string src = cfg.dataDir + "/stall_src.bin";
            std::string data(1300 * KB, '\0');
            std::mt19937 g(7);
            for (auto& c : data) c = (char)(g() & 0xFF);
            std::ofstream(src, std::ios::binary).write(data.data(), (std::streamsize)data.size());
            int64_t saved = e.chats()[0].id;
            int before = fakectl::pendingDeletedCount();
            fakectl::setUploadStallAfter(200 * KB);  // ogni parte si ferma una volta a 200 KB
            int u = e.upload(saved, src);
            CHECK(waitFor([&] { return tv(e, u).status == TStatus::Done; }, 30000, &e), "riavviato da solo e completato");
            CHECK(fakectl::pendingDeletedCount() - before >= 3, "ogni parte bloccata è stata ritirata e rimandata");
            CHECK(fakectl::pendingCount() == 0, "nessun messaggio rimasto in sospeso");
            int parts = 0;
            for (auto& c : fakectl::captionsIn(saved)) if (c.find("stall_src.bin") != std::string::npos) ++parts;
            CHECK(parts == 3, "3 parti inviate, senza doppioni");
            fakectl::setUploadStallAfter(-1);

            std::printf("\n[14c] Annullare un upload appena avviato\n");
            int u2 = e.upload(saved, src);
            e.cancel(u2);  // la risposta a sendMessage non è ancora arrivata
            CHECK(waitFor([&] { return tv(e, u2).id == 0; }, 5000, &e), "rimosso dall'elenco");
            CHECK(waitFor([&] { return fakectl::pendingCount() == 0; }, 5000, &e), "messaggio ritirato anche se annullato in volo");
            int parts2 = 0;
            for (auto& c : fakectl::captionsIn(saved)) if (c.find("stall_src.bin") != std::string::npos) ++parts2;
            CHECK(parts2 == 3, "nessuna parte in più inviata");

            std::printf("\n[14d] Esito dell'invio prima della risposta\n");
            fakectl::setEarlySendUpdate(true);
            std::string small = cfg.dataDir + "/small.txt";
            std::ofstream(small) << "ciao";
            int u3 = e.upload(saved, small);
            CHECK(waitFor([&] { return tv(e, u3).status == TStatus::Done; }, 5000, &e), "completato comunque");
            fakectl::setEarlySendUpdate(false);
            e.clearCompleted();
        }

        std::printf("\n[15] Annulla tutti\n");
        fakectl::setChunk(4 * KB);
        Settings st = e.settings();
        st.maxParallel = 2;
        e.setSettings(st);
        auto l2 = e.entries();
        for (auto& x : l2) e.download(x.key);
        CHECK(waitFor([&] { int a = 0; for (auto& t : e.transfers()) if (t.status == TStatus::Active) ++a; return a == 2; }, 5000, &e),
              "2 attivi e gli altri in coda");
        e.pause(e.transfers().back().id);
        int n = e.cancelAll();
        std::printf("       annullati: %d\n", n);
        CHECK(waitFor([&] { for (auto& t : e.transfers()) if (t.status != TStatus::Done) return false; return true; }, 5000, &e),
              "restano solo i completati");
        bool leftovers = false;
        for (auto& p2 : fs::directory_iterator(e.settings().downloadDir))
            if (p2.path().extension() == ".tfpart") leftovers = true;
        CHECK(!leftovers, "nessun file parziale lasciato in Download");
        e.shutdown();
    }

    std::printf("\nRisultato: %d controlli, %d falliti\n", checks, failures);
    return failures ? 1 : 0;
}
