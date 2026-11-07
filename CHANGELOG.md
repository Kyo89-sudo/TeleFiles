# Changelog

## 0.2.0 — 2026-11-06

### Correzioni
- **"InputFile is not specified" all'invio**: la TDLib di vcpkg (1.8.69) ha cambiato lo schema e
  `inputMessageDocument.document` ora è un oggetto `inputDocument` che contiene il file. Il file
  veniva passato nel formato vecchio e TDLib lo ignorava.
- **Upload riscritto** ("Invio non riuscito: Input file is not specified", divisione che si
  bloccava, upload fermi al 100%): non si usa più `preliminaryUploadFile`, che con i server veri
  non completa il caricamento finché il file non viene usato in un messaggio. Ora ogni file (o
  parte) parte subito come messaggio con `inputFileLocal`: TDLib carica e invia in un colpo solo,
  l'avanzamento arriva dal file del messaggio temporaneo, la fine da `updateMessageSendSucceeded`.
  Pausa, annulla e riavvio automatico ritirano il messaggio non ancora inviato (`deleteMessages`);
  alla ripresa TDLib riparte dalle parti già caricate. Funziona anche se pausa/annulla arrivano
  prima della risposta del server.
- Gara tra la risposta a `sendMessage` e `updateMessageSendSucceeded`: l'esito arrivato in anticipo
  viene conservato e applicato.
- **Controllo trasferimenti bloccati**: un download/upload attivo senza byte nuovi per 45 s
  (3 minuti se non è ancora arrivato nessun byte) viene riavviato da solo, al massimo 6 volte, poi
  va in errore con un messaggio chiaro.
- **Divisione più veloce**: niente più lettura preliminare dell'intero file per l'impronta; lo
  SHA-256 del file intero si calcola mentre si estraggono le parti e va solo nell'ultima. Nel
  pannello trasferimenti compare "Preparo la parte N · X%".
- **Scorrimento**: barra di scorrimento sovrapposta e trascinabile su chat, file e trasferimenti
  (spostata fuori dal bordo di ridimensionamento della finestra, dove prima i clic finivano);
  rotella inoltrata anche quando una casella di testo ha il focus; PagSu/PagGiù/Home/Fine/frecce.
- Il clic su un pulsante di un dialogo o di un menu non "passa" più alla riga sottostante.
- Didascalie con valori anomali non fanno più chiudere l'app (lettura numeri con `std::from_chars`).

### Novità
- **Nome ed estensione del file ricomposto**: il nome originale si legge dalla didascalia delle parti
  (resta giusto anche se Telegram cambia il nome del file); se manca l'estensione (parti caricate
  con altri programmi, es. `archivio.001`) viene ricavata dal contenuto: mkv, mp4, zip, rar, 7z,
  iso, pdf, exe e altri formati comuni.
- **Caratteri rivisti**: Segoe UI Variable nelle tre taglie ottiche (Small, Text, Display) come le
  app di Windows 11, dimensioni intere (niente mezzi pixel sfocati), ClearType come il resto del
  sistema e lo stesso carattere anche nei campi di testo.
- **Ordinamento**: clic su Nome, Dimensione o Data nell'intestazione (secondo clic inverte);
  ordine "naturale" (file-2 prima di file-10), salvato nelle impostazioni. Per Nome e Dimensione
  viene caricato l'elenco completo della chat.
- **Registro diagnostico** in `Data\telefiles.log` (ruotato a 2 MB): invii, parti, riavvii ed
  errori, utile per capire dove si ferma un trasferimento.
- **Canali e gruppi**: pulsante "+" → Nuovo canale / Nuovo gruppo (nome e descrizione);
  menu della chat (tasto destro o "…") → Elimina per tutti (se sei il proprietario) o Esci.
  Stato proprietario/amministratore letto da `updateSupergroup`/`updateBasicGroup`.
- **Eliminazione file**: "Elimina" sulla selezione, tasto Canc o menu contestuale, con conferma.
  Per i file divisi vengono eliminate tutte le parti (ritrovate con l'hashtag). Senza permessi
  compare un avviso chiaro. La lista si aggiorna anche con `updateDeleteMessages` (eliminazioni
  fatte da altri dispositivi).
- **Caricamento cartelle**: Carica ▸ Cartella… o trascinamento; sottocartelle comprese
  (Thumbs.db/desktop.ini ignorati). Il percorso relativo va nella didascalia (`📁 Cartella/Sotto`)
  e al download la struttura viene ricreata.
- **Pannello trasferimenti ridimensionabile** (trascina il bordo superiore) e richiudibile
  (freccia o doppio clic sul bordo); dimensione salvata nelle impostazioni.
- Avvisi a scomparsa per creazione/eliminazione/errori.
- **Annulla tutti** accanto a Pausa/Riprendi tutti (con conferma); con poco spazio i pulsanti
  globali diventano solo icone. Riprendi e Pausa tutti compaiono insieme se servono entrambi.

### Interfaccia
- Nuovo set di icone vettoriali disegnate in Direct2D (tratto arrotondato, alcune duotone),
  archi convertiti in curve di Bézier; non dipende più dai font di icone di Windows.
- Icone documento stile Finder con etichetta colorata per tipo di file.
- Nuova icona dell'app: riquadro arrotondato stile macOS, cielo azzurro, foglio con aeroplanino.
- Controlli segmentati per filtri, schede e tipo di chat; avatar con gradiente; dialoghi di conferma
  TaskDialog con "Annulla" predefinito per le azioni distruttive.
- Layout adattivo: nelle finestre strette spariscono la colonna Data e il testo dei pulsanti,
  la barra di avanzamento dei trasferimenti lascia spazio alla percentuale.

## 0.1.0 — 2026-11-05

Prima versione.

### Account
- Accesso con codice QR (predefinito) oppure numero + codice; supporto verifica in due passaggi.
- Sessione salvata in `%LOCALAPPDATA%\TeleFiles\td`: il login si fa una volta sola.
- Profilo nella barra laterale (nome, @username, foto) con stato Premium/gratuito e pulsante
  "Passa a Premium" (apre il bot ufficiale `t.me/PremiumBot`). Il passaggio a Premium viene
  rilevato al volo tramite `updateUser` e alza il limite dei file da 2000 a 4000 MiB.
- Logout dal menu impostazioni (annulla i trasferimenti dell'account).

### Elenco file
- Chat ordinate come in Telegram, "Messaggi salvati" sempre in cima; filtro locale delle chat.
- Ricerca lato server nella chat (`searchChatMessages`, debounce 350 ms) e filtri Tutti / Documenti /
  Video / Audio / Foto; caricamento a pagine durante lo scorrimento. Con "Tutti" i messaggi di
  solo testo vengono saltati e le pagine povere di file vengono concatenate.
- Gruppi di parti mostrati come un'unica riga con badge "N parti".
- Selezione multipla (casella, Ctrl+A) e "Scarica selezionati"; doppio clic per scaricare;
  menu contestuale (scarica, mostra nella cartella, copia nome).
- Stato per riga: avanzamento, in coda, in pausa, errore, già scaricato (verifica nome+dimensione
  nella cartella di destinazione).

### Trasferimenti
- Coda unica con limite di trasferimenti paralleli (1–5, predefinito 3).
- Pausa/ripresa: download con `cancelDownloadFile` + `downloadFile` (TDLib riprende dal
  parziale); upload con `preliminaryUploadFile` / `cancelPreliminaryUploadFile`, invio del
  messaggio solo a caricamento completato. Durante i passaggi non interrompibili (estrazione
  parte, invio messaggio, unione) la pausa viene applicata alla fine del passaggio.
- Velocità media mobile e tempo rimanente; totali in basso.
- Coda salvata in `queue.json` e ripristinata in pausa al riavvio.
- Trascina file nella finestra per caricarli nella chat aperta.

### Divisione automatica
- File oltre il limite (2000 MiB, o 4000 MiB con Premium) divisi in parti binarie
  `nome.ext.001…` da limite − 8 MiB, estratte una alla volta in `upload-temp` (spazio extra
  massimo: una parte) e cancellate dopo l'invio.
- Didascalia per parte: nome, `#tf<id> pI/N s<totale>`, SHA-256 della parte (`h…`) e del file
  intero (`H…`). L'hashtag permette di ritrovare tutte le parti con una ricerca.
- In download le parti vengono cercate per hashtag, scaricate in ordine e accodate a
  `nome.tfpart` con verifica dell'hash di ogni parte e hash progressivo del file intero; a fine
  verifica il parziale viene rinominato. Una parte corrotta viene scartata e riscaricata alla
  ripresa senza perdere le precedenti. Dopo un riavvio il parziale viene riallineato e l'hash
  ricalcolato.
- Opzione "Conserva le parti dopo la ricomposizione".

### Interfaccia
- Win32 + Direct2D/DirectWrite disegnata a mano, stile Windows 11: barra del titolo integrata
  (snap layout sul pulsante ingrandisci), angoli arrotondati DWM, icone Segoe Fluent Icons
  (Segoe MDL2 Assets su Windows 10), font Segoe UI Variable con ripiego su Segoe UI.
- Tema chiaro/scuro/come il sistema (salvato in `HKCU\Software\TeleFiles`).
- DPI per monitor (PerMonitorV2), istanza singola.

### Portable
- Modalità portable attivata dal file `TeleFiles.portable` accanto all'exe (o dall'opzione CMake
  `TELEFILES_PORTABLE`): sessione, impostazioni, coda e temporanei in `Data\`, download in `Download\`,
  `api.ini` accanto all'exe, nessuna scrittura nel registro.
- Percorsi sotto la cartella portable salvati relativi in `settings.json` e `queue.json`.
- Istanza singola per cartella dati: due copie portable in cartelle diverse possono convivere.
- Il tema passa dal registro a `settings.json`.
- `build.bat` crea anche `dist\TeleFiles-0.1.0-portable.zip`.

