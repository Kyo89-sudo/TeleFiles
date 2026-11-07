// TeleFiles - divisione automatica dei file oltre il limite di Telegram
// e ricomposizione delle parti.
//
// Formato: le parti sono una divisione binaria semplice (nome.ext.001, .002, ...),
// ricomponibili anche con 7-Zip o "copy /b". Ogni parte porta una didascalia:
//
//   film.mkv · parte 2 di 3
//   #tf7f3a2b9c01d4 p2/3 s5798205440
//   h<sha256 della parte>
//   H<sha256 del file intero>
//
// L'hashtag rende ritrovabili tutte le parti con una ricerca nella chat.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "Sha256.h"

namespace tf {

struct SplitTag {
    std::string gid;       // 14 caratteri esadecimali
    int index = 0;         // 1-based
    int count = 0;
    int64_t total = 0;     // dimensione del file intero
    std::string partSha;   // opzionale
    std::string fullSha;   // opzionale
    std::string name;      // nome originale (prima riga della didascalia), se presente
};

constexpr int64_t kMiB = 1024 * 1024;
// Limiti per singolo file di Telegram: 2000 MiB senza Premium, 4000 MiB con Premium.
constexpr int64_t kLimitFree = 2000 * kMiB;
constexpr int64_t kLimitPremium = 4000 * kMiB;
// Margine di sicurezza sotto il limite.
constexpr int64_t kPartMargin = 8 * kMiB;

std::string randomGroupId();
std::string makeCaption(const std::string& origName, const SplitTag& t);
bool parseCaption(const std::string& caption, SplitTag& out);
std::string partFileName(const std::string& origName, int index); // film.mkv.002
// "film.mkv.002" -> "film.mkv" (se il suffisso è numerico a 3+ cifre)
std::string originalNameFromPart(const std::string& partName);
int partCountFor(int64_t size, int64_t partSize);

// Estensione ricavata dal contenuto (firma dei primi byte), es. ".mkv", ".zip"; "" se sconosciuta.
std::string guessExtension(const std::string& path);
// true se il nome non ha un'estensione utile (nessuna, oppure solo cifre come ".001")
bool lacksExtension(const std::string& fileName);

using Progress = std::function<bool(int64_t bytesDone)>; // false = interrompi

// Copia [offset, offset+len) di src in dst (sovrascrive) calcolando lo SHA-256 della parte.
// Se 'running' non è nullo viene aggiornato anche l'hash del file intero.
bool extractPart(const std::string& src, int64_t offset, int64_t len, const std::string& dst,
                 std::string& partShaHex, const Progress& progress, std::string& err, Sha256* running = nullptr);

// Accoda src in fondo a dst; aggiorna 'running' (hash del file intero) e calcola
// lo SHA-256 della sola parte. Non cancella src.
bool appendPart(const std::string& src, const std::string& dst, Sha256& running,
                std::string& partShaHex, const Progress& progress, std::string& err);

} // namespace tf
