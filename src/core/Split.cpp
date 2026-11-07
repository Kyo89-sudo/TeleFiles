#include "Split.h"

#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>

#include "FileIO.h"

namespace tf {

// Legge un intero dall'inizio di [p, end); restituisce il puntatore dopo il numero o nullptr.
template <class T>
static const char* readNum(const char* p, const char* end, T& out) {
    auto r = std::from_chars(p, end, out);
    return r.ec == std::errc() ? r.ptr : nullptr;
}

static bool isHex(const std::string& s) {
    for (char c : s)
        if (!std::isxdigit((unsigned char)c)) return false;
    return !s.empty();
}

std::string randomGroupId() {
    std::random_device rd;
    std::mt19937_64 g(((uint64_t)rd() << 32) ^ rd());
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 14; ++i) s += hx[g() & 15];
    return s;
}

std::string makeCaption(const std::string& origName, const SplitTag& t) {
    std::ostringstream o;
    o << origName << " \xC2\xB7 parte " << t.index << " di " << t.count << "\n";
    o << "#tf" << t.gid << " p" << t.index << "/" << t.count << " s" << t.total;
    if (!t.partSha.empty()) o << "\nh" << t.partSha;
    if (!t.fullSha.empty()) o << "\nH" << t.fullSha;
    return o.str();
}

// "film.mkv · parte 2 di 3" -> "film.mkv"
static std::string nameFromCaption(const std::string& caption) {
    static const std::string mark = " \xC2\xB7 parte ";
    size_t pos = 0;
    while (pos < caption.size()) {
        size_t nl = caption.find('\n', pos);
        std::string line = caption.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        size_t m = line.rfind(mark);
        if (m != std::string::npos && m > 0) return line.substr(0, m);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return {};
}

bool parseCaption(const std::string& caption, SplitTag& out) {
    SplitTag t;
    t.name = nameFromCaption(caption);
    std::istringstream in(caption);
    std::string tok;
    bool haveTag = false, haveP = false;
    while (in >> tok) {
        if (tok.size() == 3 + 14 && tok.compare(0, 3, "#tf") == 0 && isHex(tok.substr(3))) {
            t.gid = tok.substr(3);
            haveTag = true;
        } else if (haveTag && tok.size() > 3 && tok[0] == 'p' && tok.find('/') != std::string::npos) {
            // "p2/3": indice e totale, senza sscanf
            int a = 0, b = 0;
            const char* end = tok.data() + tok.size();
            const char* q = readNum(tok.data() + 1, end, a);
            if (q && q < end && *q == '/') q = readNum(q + 1, end, b);
            else q = nullptr;
            if (q == end && a >= 1 && b >= a && b < 10000) {
                t.index = a;
                t.count = b;
                haveP = true;
            }
        } else if (haveTag && tok.size() > 1 && tok[0] == 's' && std::isdigit((unsigned char)tok[1])) {
            int64_t v = 0;
            const char* end = tok.data() + tok.size();
            if (readNum(tok.data() + 1, end, v) == end && v > 0) t.total = v;  // niente eccezioni su valori strani
        } else if (tok.size() == 65 && tok[0] == 'h' && isHex(tok.substr(1))) {
            t.partSha = tok.substr(1);
        } else if (tok.size() == 65 && tok[0] == 'H' && isHex(tok.substr(1))) {
            t.fullSha = tok.substr(1);
        }
    }
    if (!haveTag || !haveP) return false;
    out = t;
    return true;
}

std::string partFileName(const std::string& origName, int index) {
    char buf[16];
    std::snprintf(buf, sizeof buf, ".%03d", index);
    return origName + buf;
}

std::string originalNameFromPart(const std::string& partName) {
    auto dot = partName.rfind('.');
    if (dot == std::string::npos || partName.size() - dot - 1 < 3) return partName;
    for (size_t i = dot + 1; i < partName.size(); ++i)
        if (!std::isdigit((unsigned char)partName[i])) return partName;
    return partName.substr(0, dot);
}

int partCountFor(int64_t size, int64_t partSize) {
    if (partSize <= 0) return 1;
    return (int)((size + partSize - 1) / partSize);
}

bool lacksExtension(const std::string& fileName) {
    auto dot = fileName.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= fileName.size()) return true;
    std::string ext = fileName.substr(dot + 1);
    if (ext.size() > 10) return true;
    for (char c : ext)
        if (!std::isdigit((unsigned char)c)) return false;
    return true;  // solo cifre: ".001"
}

std::string guessExtension(const std::string& path) {
    InFile in;
    if (!in.open(path)) return {};
    std::string b(64 * 1024, '\0');
    int64_t n = in.read(b.data(), (int64_t)b.size());
    if (n <= 0) return {};
    b.resize((size_t)n);
    auto at = [&](size_t off, const char* sig, size_t len) {
        return b.size() >= off + len && std::memcmp(b.data() + off, sig, len) == 0;
    };
    if (at(0, "\x1A\x45\xDF\xA3", 4)) return b.find("webm") != std::string::npos && b.find("webm") < 64 ? ".webm" : ".mkv";
    if (at(4, "ftyp", 4)) {
        std::string brand = b.size() >= 12 ? b.substr(8, 4) : "";
        if (brand == "qt  ") return ".mov";
        if (brand == "M4A ") return ".m4a";
        if (brand.compare(0, 3, "3gp") == 0) return ".3gp";
        if (brand == "heic" || brand == "heix" || brand == "mif1") return ".heic";
        return ".mp4";
    }
    if (at(0, "RIFF", 4) && at(8, "AVI ", 4)) return ".avi";
    if (at(0, "RIFF", 4) && at(8, "WAVE", 4)) return ".wav";
    if (at(0, "RIFF", 4) && at(8, "WEBP", 4)) return ".webp";
    if (at(0, "Rar!\x1A\x07", 6)) return ".rar";
    if (at(0, "7z\xBC\xAF\x27\x1C", 6)) return ".7z";
    if (at(0, "PK\x03\x04", 4)) {
        if (b.find("word/") != std::string::npos) return ".docx";
        if (b.find("xl/") != std::string::npos) return ".xlsx";
        if (b.find("ppt/") != std::string::npos) return ".pptx";
        if (b.find("AndroidManifest.xml") != std::string::npos) return ".apk";
        if (b.find("META-INF/container.xml") != std::string::npos || b.find("application/epub+zip") != std::string::npos) return ".epub";
        return ".zip";
    }
    if (at(0, "%PDF", 4)) return ".pdf";
    if (at(0, "\x1F\x8B", 2)) return ".gz";
    if (at(0, "\xFD" "7zXZ\x00", 6)) return ".xz";
    if (at(0, "BZh", 3)) return ".bz2";
    if (at(0, "\x28\xB5\x2F\xFD", 4)) return ".zst";
    if (at(257, "ustar", 5)) return ".tar";
    if (at(0, "\x89PNG", 4)) return ".png";
    if (at(0, "\xFF\xD8\xFF", 3)) return ".jpg";
    if (at(0, "GIF8", 4)) return ".gif";
    if (at(0, "fLaC", 4)) return ".flac";
    if (at(0, "OggS", 4)) return ".ogg";
    if (at(0, "ID3", 3) || (b.size() > 1 && (unsigned char)b[0] == 0xFF && ((unsigned char)b[1] & 0xE0) == 0xE0)) return ".mp3";
    if (at(0, "\x30\x26\xB2\x75\x8E\x66\xCF\x11", 8)) return ".wmv";
    if (at(0, "FLV", 3)) return ".flv";
    if (at(0, "\x00\x00\x01\xBA", 4)) return ".mpg";
    if (b.size() > 376 && b[0] == 0x47 && b[188] == 0x47 && b[376] == 0x47) return ".ts";
    if (at(0, "MZ", 2)) return ".exe";
    if (at(0, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8)) return b.find("I\x00n\x00s\x00t\x00a\x00l\x00l") != std::string::npos ? ".msi" : ".doc";
    if (at(0, "SQLite format 3", 15)) return ".db";
    if (at(0, "conectix", 8)) return ".vhd";
    if (at(0, "vhdxfile", 8)) return ".vhdx";
    if (at(0, "KDMV", 4)) return ".vmdk";
    // ISO 9660: "CD001" al settore 16 (oltre i primi 64 KB letti)
    {
        InFile in2;
        char sig[5];
        if (in2.open(path) && in2.seek(0x8001) && in2.read(sig, 5) == 5 && std::memcmp(sig, "CD001", 5) == 0) return ".iso";
    }
    return {};
}

bool extractPart(const std::string& src, int64_t offset, int64_t len, const std::string& dst,
                 std::string& partShaHex, const Progress& progress, std::string& err, Sha256* running) {
    InFile in;
    if (!in.open(src) || !in.seek(offset)) { err = "Impossibile leggere " + fileNameOf(src); return false; }
    OutFile out;
    if (!out.open(dst, false)) { err = "Impossibile creare la parte temporanea"; return false; }
    Sha256 h;
    std::string buf(4 * kMiB, '\0');
    int64_t done = 0;
    while (done < len) {
        int64_t want = std::min<int64_t>((int64_t)buf.size(), len - done);
        int64_t n = in.read(buf.data(), want);
        if (n <= 0) { err = "Lettura interrotta: " + fileNameOf(src); return false; }
        h.update(buf.data(), (size_t)n);
        if (running) running->update(buf.data(), (size_t)n);
        if (!out.write(buf.data(), n)) { err = "Spazio su disco insufficiente?"; return false; }
        done += n;
        if (progress && !progress(done)) { err = "Interrotto"; return false; }
    }
    if (!out.close()) { err = "Errore di scrittura della parte"; return false; }
    partShaHex = h.hex();
    return true;
}

bool appendPart(const std::string& src, const std::string& dst, Sha256& running,
                std::string& partShaHex, const Progress& progress, std::string& err) {
    InFile in;
    if (!in.open(src)) { err = "Parte scaricata non trovata"; return false; }
    OutFile out;
    if (!out.open(dst, true)) { err = "Impossibile scrivere " + fileNameOf(dst); return false; }
    Sha256 h;
    std::string buf(4 * kMiB, '\0');
    int64_t done = 0;
    for (;;) {
        int64_t n = in.read(buf.data(), (int64_t)buf.size());
        if (n < 0) { err = "Errore di lettura della parte"; return false; }
        if (n == 0) break;
        h.update(buf.data(), (size_t)n);
        running.update(buf.data(), (size_t)n);
        if (!out.write(buf.data(), n)) { err = "Spazio su disco insufficiente?"; return false; }
        done += n;
        if (progress && !progress(done)) { err = "Interrotto"; return false; }
    }
    if (!out.close()) { err = "Errore di scrittura"; return false; }
    partShaHex = h.hex();
    return true;
}

} // namespace tf
