<p align="center">
  <img src="docs/icon.png" width="128" height="128" alt="Icona di TeleFiles">
</p>

<h1 align="center">TeleFiles</h1>

<p align="center">
  Client Telegram per Windows dedicato solo a <b>caricare e scaricare file</b>.
</p>

Niente chat, niente messaggi: una lista di file per ogni chat, una coda di trasferimenti con pausa
e ripresa, e la **divisione automatica** dei file oltre il limite di Telegram (2 GB, 4 GB con
Premium).

Basato su [TDLib](https://github.com/tdlib/td), la libreria ufficiale di Telegram per i client.

## Funzioni

- Accesso con **codice QR** o con **numero + codice** (anche con verifica in due passaggi); si fa una volta sola.
- Profilo con stato **Premium / gratuito** e pulsante **Passa a Premium**.
- Elenco file per chat con **ricerca**, filtri (Documenti, Video, Audio, Foto) e selezione multipla.
- **Crea ed elimina canali/gruppi**, **elimina file** (anche divisi, tutte le parti).
- **Carica cartelle intere**: la struttura delle sottocartelle viene ricreata al download.
- **Download e upload** in coda, paralleli (1–5), con **pausa / ripresa** anche dopo un riavvio dell'app.
- **Divisione automatica**: i file troppo grandi vengono caricati in parti (`nome.001`, `.002`, …) e
  ricomposti da soli al download, con verifica SHA-256 di ogni parte e del file finale.
  Le parti si possono riunire anche a mano con 7-Zip ("Unisci file") o con `copy /b`.
- Trascina i file nella finestra per caricarli nella chat aperta.
- Tema chiaro / scuro / come il sistema, stile Windows 11.

## Compilazione

Requisiti: Visual Studio 2022 (C++), CMake ≥ 3.20, [vcpkg](https://github.com/microsoft/vcpkg).

```bat
set VCPKG_ROOT=C:\vcpkg
build.bat
```

Le dipendenze (TDLib, nlohmann-json) le installa vcpkg dal file `vcpkg.json` con il triplet
`x64-windows-static`: l'eseguibile non richiede DLL. **La prima compilazione di TDLib è lunga**
(20–40 minuti); le successive usano la cache di vcpkg.

## Credenziali API

Ogni client non ufficiale deve usare un proprio `api_id`/`api_hash`, gratuiti:

1. vai su <https://my.telegram.org> → *API development tools* e crea un'applicazione;
2. copia `api.ini.example` come `api.ini` accanto a `TeleFiles.exe` (oppure in `%APPDATA%\TeleFiles\`)
   e inserisci i due valori.

In alternativa si possono fissare in compilazione:
`-DTELEFILES_API_ID=123456 -DTELEFILES_API_HASH=...`.

## Versione portable

`build.bat` crea anche `dist\TeleFiles-0.2.0-portable.zip`: lo stesso exe (unico file, senza DLL)
più il file marcatore `TeleFiles.portable`. Con il marcatore accanto all'exe TeleFiles tiene **tutto
nella sua cartella** e non scrive nel registro: si può usare da una chiavetta USB. I percorsi dentro la
cartella vengono salvati relativi, quindi coda e impostazioni restano valide anche se cambia la lettera
dell'unità. Cancellando il marcatore lo stesso exe torna a usare `%LOCALAPPDATA%`.
Con `-DTELEFILES_PORTABLE=ON` si ottiene un exe sempre portable.

La cartella `Data` contiene la sessione del tuo account: non condividerla.

## Dove salva i dati

| Cosa | Installata | Portable |
|---|---|---|
| Sessione e cache TDLib | `%LOCALAPPDATA%\TeleFiles\td`, `...\files` | `Data\td`, `Data\files` |
| Impostazioni (incluso il tema), coda | `%LOCALAPPDATA%\TeleFiles\settings.json`, `queue.json` | `Data\settings.json`, `Data\queue.json` |
| Parti temporanee in upload | `%LOCALAPPDATA%\TeleFiles\upload-temp` | `Data\upload-temp` |
| Download (predefinito) | `Download\TeleFiles` | `Download` accanto all'exe |
| Credenziali API | `api.ini` accanto all'exe o `%APPDATA%\TeleFiles\api.ini` | `api.ini` accanto all'exe |

## Formato delle parti

Ogni parte è un pezzo binario del file originale; la didascalia del messaggio contiene:

```
film.mkv · parte 2 di 3
#tf7f3a2b9c01d4 p2/3 s5798205440
h<sha256 della parte>
H<sha256 del file intero>
```

L'hashtag serve a TeleFiles per ritrovare tutte le parti con una ricerca nella chat.

## Limiti noti

- Telegram limita la dimensione **per singolo file** (2000 MiB, 4000 MiB con Premium); la divisione
  automatica lo aggira, ma ogni parte occupa un messaggio.
- Senza Premium Telegram può limitare la velocità di download: un client diverso non cambia questo.
- La pausa di un upload riparte dalla parte in corso; la ripresa parziale della stessa parte dipende da TDLib.
- Lo sfondo della barra laterale è un colore pieno in stile Mica, non l'effetto traslucido di sistema.

## Aiuto

- **Problemi o richieste**: apri una segnalazione su
  [github.com/Kyo89-sudo/TeleFiles/issues](https://github.com/Kyo89-sudo/TeleFiles/issues).
  Se un trasferimento si blocca o fallisce, allega il registro `telefiles.log`
  (`%LOCALAPPDATA%\TeleFiles\telefiles.log`, oppure `Data\telefiles.log` nella versione portable):
  annota invii, parti, riavvii ed errori, non contiene messaggi né credenziali.
- **Codice sorgente e aggiornamenti**: [github.com/Kyo89-sudo/TeleFiles](https://github.com/Kyo89-sudo/TeleFiles).
- Altri progetti: [github.com/Kyo89-sudo](https://github.com/Kyo89-sudo).

## Licenza

TeleFiles è distribuito con licenza [MIT](LICENSE). Usa [TDLib](https://github.com/tdlib/td)
(Boost Software License 1.0), [nlohmann/json](https://github.com/nlohmann/json) (MIT) e
[QR Code generator](https://github.com/nayuki/QR-Code-generator) di Nayuki (MIT).
TeleFiles non è un'app ufficiale di Telegram.
