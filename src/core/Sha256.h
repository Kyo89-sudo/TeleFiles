// TeleFiles - SHA-256 portabile (usato per verificare le parti divise)
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tf {

class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    std::array<uint8_t, 32> digest();      // non modifica lo stato interno
    std::string hex();                     // digest() in esadecimale minuscolo

    // Hash di un intero file; 'progress' (opzionale) riceve i byte letti finora.
    // Restituisce stringa vuota in caso di errore di lettura.
    template <class F>
    static std::string fileHex(const std::string& utf8Path, F progress);
    static std::string fileHex(const std::string& utf8Path) {
        return fileHex(utf8Path, [](int64_t) { return true; });
    }

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t bufLen_ = 0;
    uint64_t total_ = 0;
};

} // namespace tf

#include "FileIO.h"

template <class F>
std::string tf::Sha256::fileHex(const std::string& utf8Path, F progress) {
    InFile in;
    if (!in.open(utf8Path)) return {};
    Sha256 s;
    std::string buf(1 << 20, '\0');
    int64_t done = 0;
    for (;;) {
        int64_t n = in.read(buf.data(), (int64_t)buf.size());
        if (n < 0) return {};
        if (n == 0) break;
        s.update(buf.data(), (size_t)n);
        done += n;
        if (!progress(done)) return {};
    }
    return s.hex();
}
