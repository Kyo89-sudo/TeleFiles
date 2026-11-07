// TeleFiles - accesso ai file con percorsi UTF-8 (portabile Windows/Linux)
#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace tf {

namespace fs = std::filesystem;

inline fs::path u8p(const std::string& s) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}

inline std::string p8(const fs::path& p) {
#if defined(__cpp_char8_t)
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
#else
    return p.u8string();
#endif
}

class InFile {
public:
    bool open(const std::string& path) {
        f_.open(u8p(path), std::ios::binary);
        return f_.is_open();
    }
    bool seek(int64_t off) {
        f_.clear();
        f_.seekg(off, std::ios::beg);
        return (bool)f_;
    }
    // byte letti, 0 a fine file, -1 errore
    int64_t read(char* dst, int64_t n) {
        f_.read(dst, n);
        auto got = f_.gcount();
        if (got == 0 && f_.bad()) return -1;
        return (int64_t)got;
    }

private:
    std::ifstream f_;
};

class OutFile {
public:
    bool open(const std::string& path, bool append) {
        f_.open(u8p(path), std::ios::binary | (append ? std::ios::app : std::ios::trunc));
        return f_.is_open();
    }
    bool write(const char* src, int64_t n) {
        f_.write(src, n);
        return (bool)f_;
    }
    bool close() {
        f_.flush();
        bool ok = (bool)f_;
        f_.close();
        return ok;
    }

private:
    std::ofstream f_;
};

inline int64_t fileSize(const std::string& path) {
    std::error_code ec;
    auto s = fs::file_size(u8p(path), ec);
    return ec ? -1 : (int64_t)s;
}
inline bool fileExists(const std::string& path) {
    std::error_code ec;
    return fs::exists(u8p(path), ec);
}
inline bool removeFile(const std::string& path) {
    std::error_code ec;
    return fs::remove(u8p(path), ec);
}
inline bool makeDirs(const std::string& path) {
    std::error_code ec;
    fs::create_directories(u8p(path), ec);
    return !ec;
}
// Sposta un file; se rename fallisce (volumi diversi) copia e cancella.
inline bool moveFile(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::rename(u8p(from), u8p(to), ec);
    if (!ec) return true;
    ec.clear();
    fs::copy_file(u8p(from), u8p(to), fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    fs::remove(u8p(from), ec);
    return true;
}
inline std::string joinPath(const std::string& dir, const std::string& name) {
    return p8(u8p(dir) / u8p(name));
}
inline std::string fileNameOf(const std::string& path) { return p8(u8p(path).filename()); }

// "film.mkv" -> "film (1).mkv" se esiste già
inline std::string uniquePath(const std::string& dir, const std::string& name) {
    std::string p = joinPath(dir, name);
    if (!fileExists(p) && !fileExists(p + ".tfpart")) return p;
    fs::path n = u8p(name);
    std::string stem = p8(n.stem()), ext = p8(n.extension());
    for (int i = 1; i < 10000; ++i) {
        p = joinPath(dir, stem + " (" + std::to_string(i) + ")" + ext);
        if (!fileExists(p) && !fileExists(p + ".tfpart")) return p;
    }
    return joinPath(dir, name);
}

// Rende un nome file valido su Windows
inline std::string sanitizeFileName(std::string s) {
    for (char& c : s)
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
            c == '?' || c == '*' || (unsigned char)c < 32)
            c = '_';
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    if (s.empty()) s = "file";
    return s;
}

} // namespace tf
