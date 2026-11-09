#include "MainWindow.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cwctype>

#include "../../third_party/qrcodegen/qrcodegen.hpp"
#include "../core/FileIO.h"
#include "Text.h"

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace tf {

static const UINT WM_APP_REFRESH = WM_APP + 1;
static const UINT_PTR TIMER_TICK = 1, TIMER_SEARCH = 2;

// misure in DIP
static const float kTitleH = 32, kSideW = 240, kToolH = 48, kFilterH = 38, kHeadH = 26, kRowH = 34;
static const float kTRowH = 44, kTHeadH = 38, kStatusH = 24;
static const float kTransMin = kTHeadH + kTRowH + kStatusH + 6;   // pannello aperto: almeno una riga
static const float kTransCollapsed = kTHeadH + kStatusH;          // pannello chiuso: intestazione + stato

// ---------------------------------------------------------------- utilità

static std::wstring U(const std::string& s) { return utf8ToWide(s); }

static std::wstring fmtDate(int64_t unix) {
    if (!unix) return L"";
    static const wchar_t* mesi[] = {L"gen", L"feb", L"mar", L"apr", L"mag", L"giu",
                                    L"lug", L"ago", L"set", L"ott", L"nov", L"dic"};
    time_t t = (time_t)unix;
    tm lt{};
    localtime_s(&lt, &t);
    wchar_t buf[48];
    swprintf(buf, 48, L"%d %ls %d", lt.tm_mday, mesi[lt.tm_mon], lt.tm_year + 1900);
    return buf;
}

static std::wstring extOf(const std::string& name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos || name.size() - dot > 6 || dot + 1 == name.size()) return L"";
    std::wstring e = U(name.substr(dot + 1));
    for (auto& c : e) c = (wchar_t)towupper(c);
    return e;
}

static std::wstring initials(const std::wstring& t) {
    std::wstring r;
    bool start = true;
    for (wchar_t c : t) {
        if (iswspace(c) || c == L'-' || c == L'_') { start = true; continue; }
        if (start && iswalnum(c)) { r += (wchar_t)towupper(c); if (r.size() == 2) break; }
        start = false;
    }
    return r.empty() ? L"?" : r;
}

static D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
static bool inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Il ritaglio Direct2D non si applica alle aree cliccabili delle righe.
static void clipRowHits(std::vector<Hit>& hits, size_t first, const D2D1_RECT_F& area) {
    for (size_t i = first; i < hits.size(); ++i) {
        auto& r = hits[i].r;
        r = R(std::max(r.left, area.left), std::max(r.top, area.top),
              std::min(r.right, area.right), std::min(r.bottom, area.bottom));
    }
}

// geometrie condivise tra disegno e posizionamento dei controlli EDIT nativi
static D2D1_RECT_F chatSearchRect() { return R(12, kTitleH + 4, kSideW - 48, kTitleH + 34); }
static D2D1_RECT_F fileSearchRect(float W) {
    float right = W - 16 - 112 - 10;
    float w = clampf(W - kSideW - 16 - 112 - 10 - 220, 150, 300);
    return R(right - w, kTitleH + 10, right, kTitleH + 40);
}
static D2D1_RECT_F modalRect(float W, float H) {
    float w = 440, h = 318;
    float x = (W - w) / 2, y = std::max(kTitleH + 12, (H - h) / 2);
    return R(x, y, x + w, y + h);
}

// ---------------------------------------------------------------- creazione

MainWindow::MainWindow(Engine& engine, std::wstring configError, bool portable)
    : eng_(engine), configError_(std::move(configError)), portable_(portable) {}

bool MainWindow::create(HINSTANCE inst, int show) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dw_.GetAddressOf()));
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.GetAddressOf()));
    icons_.init(d2d_.Get());

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = nullptr;  // il cursore lo sceglie WM_SETCURSOR
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"TeleFilesMain";
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, portable_ ? L"TeleFiles Portable" : L"TeleFiles",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 760,
                            nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    dpi_ = GetDpiForWindow(hwnd_);
    scale_ = (float)dpi_ / 96.f;
    SetWindowPos(hwnd_, nullptr, 0, 0, (int)(1180 * scale_), (int)(760 * scale_), SWP_NOMOVE | SWP_NOZORDER);

    // bordi arrotondati di Windows 11 e ombra con la barra del titolo personalizzata
    DWORD corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    MARGINS m{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(hwnd_, &m);

    auto mkEdit = [&](DWORD extra) {
        HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | extra, 0, 0, 10, 10, hwnd_, nullptr,
                                 inst, nullptr);
        SetWindowSubclass(e, EditProc, 1, (DWORD_PTR)this);
        return e;
    };
    edChats_ = mkEdit(0);
    edFiles_ = mkEdit(0);
    edLogin_ = mkEdit(0);
    edPwd_ = mkEdit(ES_PASSWORD);
    edModalTitle_ = mkEdit(0);
    edModalDesc_ = mkEdit(0);
    SendMessageW(edChats_, EM_SETCUEBANNER, FALSE, (LPARAM)L"Cerca chat");
    SendMessageW(edFiles_, EM_SETCUEBANNER, FALSE, (LPARAM)L"Cerca file in questa chat");
    SendMessageW(edPwd_, EM_SETCUEBANNER, FALSE, (LPARAM)L"Password");
    SendMessageW(edModalTitle_, EM_LIMITTEXT, 128, 0);
    SendMessageW(edModalDesc_, EM_LIMITTEXT, 255, 0);

    Settings s = eng_.settings();
    themeMode_ = s.theme;  // salvato in settings.json (funziona anche in portable)
    transH_ = (float)s.transfersHeight;
    transCollapsed_ = s.transfersCollapsed;
    sortCol_ = s.sortColumn;
    sortDesc_ = s.sortDescending;
    applyTheme();
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    SetTimer(hwnd_, TIMER_TICK, 1000, nullptr);
    return true;
}

void MainWindow::requestRefresh() {
    if (!refreshPosted_.exchange(true)) PostMessageW(hwnd_, WM_APP_REFRESH, 0, 0);
}

bool MainWindow::systemDark() {
    DWORD v = 1, sz = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

void MainWindow::applyTheme() {
    bool dark = themeMode_ == 2 || (themeMode_ == 0 && systemDark());
    pal_ = dark ? darkPalette() : lightPalette();
    BOOL d = dark;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &d, sizeof d);
    if (editBrush_) DeleteObject(editBrush_);
    editBrush_ = CreateSolidBrush(pal_.editBg);
    createFormats();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

std::wstring MainWindow::pickFamily(std::initializer_list<const wchar_t*> names) {
    ComPtr<IDWriteFontCollection> coll;
    if (SUCCEEDED(dw_->GetSystemFontCollection(coll.GetAddressOf(), FALSE)))
        for (const wchar_t* n : names) {
            UINT32 idx;
            BOOL exists = FALSE;
            if (SUCCEEDED(coll->FindFamilyName(n, &idx, &exists)) && exists) return n;
        }
    return *names.begin();
}

void MainWindow::createFormats() {
    auto mk = [&](ComPtr<IDWriteTextFormat>& f, const wchar_t* family, float size, DWRITE_FONT_WEIGHT w,
                  bool wrap = false) {
        f.Reset();
        dw_->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"it-it",
                              f.GetAddressOf());
        if (!f) return;
        f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!wrap) {
            DWRITE_TRIMMING tr{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ell;
            dw_->CreateEllipsisTrimmingSign(f.Get(), ell.GetAddressOf());
            f->SetTrimming(&tr, ell.Get());
        }
    };
    // Windows 11: Segoe UI Variable con la "taglia ottica" giusta per ogni dimensione, come le app
    // di sistema (Small sotto i 12 px, Text per il corpo, Display per i titoli); Windows 10: Segoe UI.
    // Dimensioni intere: con i mezzi pixel le lettere cadono tra due pixel e appaiono sfocate.
    std::wstring smallF = pickFamily({L"Segoe UI Variable Small", L"Segoe UI", L"Tahoma"});
    std::wstring uiF = pickFamily({L"Segoe UI Variable Text", L"Segoe UI", L"Tahoma"});
    std::wstring dispF = pickFamily({L"Segoe UI Variable Display", L"Segoe UI", L"Tahoma"});
    const wchar_t* ui = uiF.c_str();
    const wchar_t* sm = smallF.c_str();
    mk(fBody_, ui, 13, DWRITE_FONT_WEIGHT_NORMAL);
    mk(fSemi_, ui, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    mk(fSmall_, sm, 12, DWRITE_FONT_WEIGHT_NORMAL);
    mk(fSmallSemi_, sm, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    mk(fTitle_, dispF.c_str(), 16, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    mk(fHeader_, dispF.c_str(), 22, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    mk(fBadge_, sm, 9, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    mk(fDoc_, sm, 7, DWRITE_FONT_WEIGHT_BOLD);
    mk(fWrap_, ui, 13, DWRITE_FONT_WEIGHT_NORMAL, true);
    mk(fWrapSm_, sm, 12, DWRITE_FONT_WEIGHT_NORMAL, true);

    // i campi di testo (GDI) usano lo stesso carattere del resto dell'interfaccia
    if (editFont_) DeleteObject(editFont_);
    editFont_ = CreateFontW(-(int)std::lround(13 * scale_), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                            CLEARTYPE_QUALITY, 0, ui);
    for (HWND e : {edChats_, edFiles_, edLogin_, edPwd_, edModalTitle_, edModalDesc_})
        if (e) SendMessageW(e, WM_SETFONT, (WPARAM)editFont_, TRUE);
}

void MainWindow::createDevice() {
    if (rt_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties();
    p.dpiX = p.dpiY = (float)dpi_;
    d2d_->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)),
                                 rt_.GetAddressOf());
    if (rt_) {
        rt_->CreateSolidColorBrush(pal_.text, brush_.GetAddressOf());
        // ClearType come il resto di Windows (testo più nitido a 100-125%); in scala di grigi
        // se l'utente l'ha disattivato o con DPI alti, dove non serve
        UINT smoothing = 0;
        BOOL smooth = TRUE;
        SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &smooth, 0);
        SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0, &smoothing, 0);
        bool cleartype = smooth && smoothing == FE_FONTSMOOTHINGCLEARTYPE && scale_ < 1.75f;
        rt_->SetTextAntialiasMode(cleartype ? D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE : D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    bitmaps_.clear();
    appIcons_.clear();
}

ComPtr<ID2D1Bitmap> MainWindow::loadBitmap(const std::string& path) {
    auto it = bitmaps_.find(path);
    if (it != bitmaps_.end()) return it->second;
    ComPtr<ID2D1Bitmap> bmp;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (wic_ && rt_ &&
        SUCCEEDED(wic_->CreateDecoderFromFilename(U(path).c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, dec.GetAddressOf())) &&
        SUCCEEDED(dec->GetFrame(0, frame.GetAddressOf())) &&
        SUCCEEDED(wic_->CreateFormatConverter(conv.GetAddressOf())) &&
        SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeMedianCut)))
        rt_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, bmp.GetAddressOf());
    bitmaps_[path] = bmp;
    return bmp;
}

// Icona dell'applicazione (dalle risorse dell'exe) come bitmap Direct2D, alla dimensione in pixel richiesta.
ID2D1Bitmap* MainWindow::appIcon(int px) {
    auto it = appIcons_.find(px);
    if (it != appIcons_.end()) return it->second.Get();
    ComPtr<ID2D1Bitmap> bmp;
    HICON hi = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, px, px, 0);
    ComPtr<IWICBitmap> wb;
    ComPtr<IWICFormatConverter> conv;
    if (hi && wic_ && rt_ && SUCCEEDED(wic_->CreateBitmapFromHICON(hi, wb.GetAddressOf())) &&
        SUCCEEDED(wic_->CreateFormatConverter(conv.GetAddressOf())) &&
        SUCCEEDED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeMedianCut)))
        rt_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, bmp.GetAddressOf());
    if (hi) DestroyIcon(hi);
    appIcons_[px] = bmp;
    return bmp.Get();
}

// ---------------------------------------------------------------- primitive

void MainWindow::fill(D2D1_RECT_F r, D2D1_COLOR_F c, float radius) {
    brush_->SetColor(c);
    if (radius > 0) rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
    else rt_->FillRectangle(r, brush_.Get());
}

void MainWindow::stroke(D2D1_RECT_F r, D2D1_COLOR_F c, float radius, float w) {
    brush_->SetColor(c);
    D2D1_RECT_F q = R(r.left + w / 2, r.top + w / 2, r.right - w / 2, r.bottom - w / 2);
    if (radius > 0) rt_->DrawRoundedRectangle(D2D1::RoundedRect(q, radius, radius), brush_.Get(), w);
    else rt_->DrawRectangle(q, brush_.Get(), w);
}

void MainWindow::line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float w) {
    brush_->SetColor(c);
    rt_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush_.Get(), w);
}

void MainWindow::text(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* f, D2D1_COLOR_F c,
                      DWRITE_TEXT_ALIGNMENT al, DWRITE_PARAGRAPH_ALIGNMENT va) {
    if (s.empty() || !f || r.right <= r.left) return;
    f->SetTextAlignment(al);
    f->SetParagraphAlignment(va);
    brush_->SetColor(c);
    rt_->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void MainWindow::icon(Ic ic, D2D1_RECT_F r, D2D1_COLOR_F c, float px, bool duo, float weight) {
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    icons_.draw(rt_.Get(), ic, R(cx - px / 2, cy - px / 2, cx + px / 2, cy + px / 2), c, weight, duo);
}

float MainWindow::measure(const std::wstring& s, IDWriteTextFormat* f) {
    ComPtr<IDWriteTextLayout> l;
    if (FAILED(dw_->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000, 100, l.GetAddressOf()))) return 0;
    DWRITE_TEXT_METRICS m;
    l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void MainWindow::progressBar(D2D1_RECT_F r, double frac, D2D1_COLOR_F fg) {
    float h = r.bottom - r.top;
    if (r.right - r.left < h * 2) return;
    fill(r, pal_.track, h / 2);
    frac = std::max(0.0, std::min(1.0, frac));
    if (frac > 0) fill(R(r.left, r.top, r.left + std::max(h, (float)((r.right - r.left) * frac)), r.bottom), fg, h / 2);
}

void MainWindow::addHit(D2D1_RECT_F r, Act a, int64_t p, const std::string& key) { hits_.push_back({r, a, p, key}); }

bool MainWindow::hovered(Act a, int64_t p, const std::string& key) const {
    return hoverAct_ == a && hoverParam_ == p && hoverKey_ == key;
}

bool MainWindow::button(D2D1_RECT_F r, const std::wstring& label, Act a, int64_t p, bool primary, Ic ic,
                        bool withIcon, bool danger) {
    bool hv = hovered(a, p);
    D2D1_COLOR_F tc;
    if (primary) {
        D2D1_COLOR_F c = pal_.accent;
        if (hv) c.a = 0.88f;
        fill(r, c, 6);
        tc = pal_.accentText;
    } else {
        fill(r, hv ? pal_.hoverSurface : pal_.surface, 6);
        stroke(r, danger && hv ? pal_.error : pal_.border, 6);
        tc = danger ? pal_.error : pal_.text;
    }
    IDWriteTextFormat* f = primary ? fSemi_.Get() : fBody_.Get();
    if (withIcon) {
        float tw = measure(label, f);
        float total = 15 + (label.empty() ? 0 : 6 + tw);
        float x = (r.left + r.right - total) / 2;
        icon(ic, R(x, r.top, x + 15, r.bottom), tc, 15, false, primary ? 2.0f : 1.8f);
        text(label, R(x + 21, r.top, r.right, r.bottom), f, tc);
    } else {
        text(label, r, f, tc, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    addHit(r, a, p);
    return hv;
}

bool MainWindow::iconButton(D2D1_RECT_F r, Ic ic, Act a, int64_t p, const std::string& key, const D2D1_COLOR_F* tint,
                            bool filled, float px, float weight) {
    bool hv = hovered(a, p, key);
    float rad = std::min(r.right - r.left, r.bottom - r.top) / 2;
    if (filled) fill(r, tint ? *tint : pal_.accent, rad);
    else if (hv) fill(r, pal_.hover, rad);
    D2D1_COLOR_F c = filled ? pal_.accentText : (tint ? *tint : pal_.text2);
    if (hv && !tint && !filled) c = pal_.text;
    icon(ic, r, c, px, false, weight);
    addHit(r, a, p, key);
    return hv;
}

void MainWindow::avatar(D2D1_RECT_F r, const std::wstring& title, int64_t seed, ID2D1Bitmap* bmp) {
    static const unsigned colors[][2] = {{0xFF8A7A, 0xE5574C}, {0xFFC36B, 0xF0922E}, {0x7ED69A, 0x3BAA6A},
                                         {0x72C3FF, 0x2F8FE0}, {0xB79BFF, 0x8361E0}, {0xFF8FC0, 0xE0579A},
                                         {0x6FD9D2, 0x2EAAA2}};
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, rad = (r.right - r.left) / 2;
    D2D1_ELLIPSE el = D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad);
    if (bmp) {
        ComPtr<ID2D1BitmapBrush> bb;
        rt_->CreateBitmapBrush(bmp, bb.GetAddressOf());
        if (bb) {
            D2D1_SIZE_F s = bmp->GetSize();
            float k = (r.right - r.left) / std::min(s.width, s.height);
            bb->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(r.left, r.top));
            rt_->FillEllipse(el, bb.Get());
            return;
        }
    }
    // gradiente verticale come gli avatar di Telegram
    const unsigned* c = colors[(uint64_t)(seed < 0 ? -seed : seed) % 7];
    D2D1_GRADIENT_STOP stops[2] = {{0.f, rgb(c[0])}, {1.f, rgb(c[1])}};
    ComPtr<ID2D1GradientStopCollection> sc;
    ComPtr<ID2D1LinearGradientBrush> lg;
    rt_->CreateGradientStopCollection(stops, 2, sc.GetAddressOf());
    if (sc) rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(cx, r.top), D2D1::Point2F(cx, r.bottom)),
                                           sc.Get(), lg.GetAddressOf());
    if (lg) rt_->FillEllipse(el, lg.Get());
    text(initials(title), r, rad > 18 ? fSemi_.Get() : fBadge_.Get(), rgb(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// Icona documento stile Finder: foglio con angolo piegato ed etichetta colorata con l'estensione.
void MainWindow::docIcon(D2D1_RECT_F r, const FileEntry& e) {
    std::wstring ext = extOf(e.name);
    unsigned tint = 0x7D8590;
    auto is = [&](std::initializer_list<const wchar_t*> l) {
        for (auto x : l) if (ext == x) return true;
        return false;
    };
    if (e.kind == FileKind::Video || is({L"MKV", L"MP4", L"AVI", L"MOV", L"WEBM", L"M4V", L"TS"})) tint = 0xEF7D38;
    else if (e.kind == FileKind::Audio || is({L"MP3", L"FLAC", L"WAV", L"M4A", L"OGG", L"OPUS", L"AAC"})) tint = 0xC25BD6;
    else if (e.kind == FileKind::Photo || is({L"JPG", L"JPEG", L"PNG", L"GIF", L"WEBP", L"HEIC", L"BMP"})) tint = 0x34A853;
    else if (is({L"ZIP", L"7Z", L"RAR", L"TAR", L"GZ", L"XZ", L"001"})) tint = 0x9A7B52;
    else if (is({L"PDF"})) tint = 0xE5483C;
    else if (is({L"DOC", L"DOCX", L"TXT", L"RTF", L"ODT", L"MD"})) tint = 0x2F7CF6;
    else if (is({L"XLS", L"XLSX", L"CSV", L"ODS"})) tint = 0x1E9E5A;
    else if (is({L"ISO", L"IMG", L"EXE", L"MSI", L"APK", L"DMG"})) tint = 0x5E6E85;
    if (e.isGroup) tint = 0x9A7B52;
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    D2D1_RECT_F page = R(cx - 9.5f, cy - 12.5f, cx + 9.5f, cy + 12.5f);
    fill(R(page.left + 1, page.top + 1, page.right - 1, page.bottom - 1), pal_.dark ? rgb(0x3A3A3A) : rgb(0xFFFFFF), 2.5f);
    icon(Ic::Doc, R(cx - 13, cy - 13, cx + 13, cy + 13), pal_.dark ? rgb(0x8A8A8A) : rgb(0xB9BEC6), 26, false, 1.1f);
    if (ext.size() > 4) ext = ext.substr(0, 4);
    if (ext.empty()) ext = L"FILE";
    D2D1_RECT_F band = R(cx - 10.5f, cy + 1.5f, cx + 10.5f, cy + 9);
    fill(band, rgb(tint), 2);
    text(ext, band, fDoc_.Get(), rgb(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
}

// ---------------------------------------------------------------- layout

// Ordine dell'elenco file. Nome: confronto "naturale" come Esplora file (file2 prima di file10),
// percorso della cartella compreso, così i file della stessa cartella restano vicini.
void MainWindow::sortEntries() {
    auto key = [](const FileEntry& e) { return U(e.folder.empty() ? e.name : e.folder + "/" + e.name); };
    bool desc = sortDesc_;
    if (sortCol_ == 1) {
        std::vector<std::pair<std::wstring, size_t>> k;
        k.reserve(entriesSnap_.size());
        for (size_t i = 0; i < entriesSnap_.size(); ++i) k.push_back({key(entriesSnap_[i]), i});
        std::stable_sort(k.begin(), k.end(), [desc](const auto& a, const auto& b) {
            int c = StrCmpLogicalW(a.first.c_str(), b.first.c_str());
            return desc ? c > 0 : c < 0;
        });
        std::vector<FileEntry> out;
        out.reserve(entriesSnap_.size());
        for (auto& p : k) out.push_back(std::move(entriesSnap_[p.second]));
        entriesSnap_.swap(out);
    } else if (sortCol_ == 2) {
        std::stable_sort(entriesSnap_.begin(), entriesSnap_.end(), [desc](const FileEntry& a, const FileEntry& b) {
            return desc ? a.size > b.size : a.size < b.size;
        });
    } else {
        std::stable_sort(entriesSnap_.begin(), entriesSnap_.end(), [desc](const FileEntry& a, const FileEntry& b) {
            if (a.date != b.date) return desc ? a.date > b.date : a.date < b.date;
            return desc ? a.msgId > b.msgId : a.msgId < b.msgId;
        });
    }
}

MainWindow::Layout MainWindow::computeLayout(float W, float H) {
    Layout l;
    l.W = W;
    l.H = H;
    l.collapsed = transCollapsed_;
    // spazio minimo per la lista: barra strumenti, filtri, intestazione e tre righe
    float maxT = H - (kTitleH + kToolH + kFilterH + kHeadH + 3 * kRowH);
    if (l.collapsed) l.transH = kTransCollapsed;
    else l.transH = clampf(transH_, kTransMin, std::max(kTransMin, maxT));
    if (l.transH > H - kTitleH - kToolH - kFilterH) l.transH = std::max(kTransCollapsed, H - kTitleH - kToolH - kFilterH);
    l.transTop = H - l.transH;
    l.narrow = W - kSideW < 760;
    return l;
}

void MainWindow::saveTransfersLayout() {
    Settings s = eng_.settings();
    s.transfersHeight = (int)std::lround(transH_);
    s.transfersCollapsed = transCollapsed_;
    eng_.setSettings(s);
}

// ---------------------------------------------------------------- disegno

void MainWindow::paint() {
    createDevice();
    if (!rt_) return;
    hits_.clear();
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float W = (float)rc.right / scale_, H = (float)rc.bottom / scale_;

    // avvisi del motore -> notifiche a scomparsa
    Notice n;
    while (eng_.popNotice(n)) {
        toasts_.push_back({U(n.text), n.error, GetTickCount64() + (n.error ? 6500 : 4000)});
    }
    ULONGLONG now = GetTickCount64();
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [now](const Toast& t) { return t.until <= now; }),
                  toasts_.end());
    if (toasts_.size() > 3) toasts_.erase(toasts_.begin(), toasts_.end() - 3);

    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(pal_.bg);

    AuthInfo a = eng_.auth();
    if (a.state != lastAuth_) {
        lastAuth_ = a.state;
        if (a.state == AuthState::WaitPhone) qrRequested_ = false;
        if (a.state != AuthState::Ready && modal_) closeModal();
        layoutEdits();
    }
    bool main = configError_.empty() && a.state == AuthState::Ready;
    if (!main) {
        drawLogin(W, H);
    } else {
        Layout lay = computeLayout(W, H);
        drawSidebar(W, H);
        drawContent(lay);
        drawToasts(W, lay.transTop - 10);
    }
    drawTitleBar(W);
    if (main && modal_) drawModal(W, H);

    if (rt_->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) {
        rt_.Reset();
        brush_.Reset();
    }
}

void MainWindow::drawTitleBar(float W) {
    if (ID2D1Bitmap* ic = appIcon((int)std::lround(16 * scale_)))
        rt_->DrawBitmap(ic, R(14, 8, 30, 24), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    text(portable_ ? L"TeleFiles Portable" : L"TeleFiles", R(40, 0, 300, kTitleH), fSmall_.Get(), pal_.text);
    struct B { Act a; Ic ic; };
    B bs[3] = {{Act::Min, Ic::WinMin}, {Act::Max, IsZoomed(hwnd_) ? Ic::WinRestore : Ic::WinMax}, {Act::Close, Ic::WinClose}};
    for (int i = 0; i < 3; ++i) {
        D2D1_RECT_F r = R(W - 46.f * (float)(3 - i), 0, W - 46.f * (float)(2 - i), kTitleH);
        bool hv = hoverCaption_ == bs[i].a, pr = pressedCaption_ == bs[i].a;
        D2D1_COLOR_F fg = pal_.text;
        if (bs[i].a == Act::Close && (hv || pr)) {
            fill(r, pr ? rgb(0xC42B1C, 0.9f) : rgb(0xC42B1C));
            fg = rgb(0xFFFFFF);
        } else if (hv || pr) {
            fill(r, pr ? pal_.selected : pal_.hover);
        }
        icon(bs[i].ic, r, fg, 16, false, 1.45f);
    }
}

void MainWindow::drawLogin(float W, float H) {
    float cw = 420, ch = 500;
    float x = (W - cw) / 2, y = kTitleH + std::max(16.f, (H - kTitleH - ch) / 2);
    D2D1_RECT_F card = R(x, y, x + cw, y + ch);
    fill(card, pal_.surface, 12);
    stroke(card, pal_.border, 12);

    if (ID2D1Bitmap* ic = appIcon((int)std::lround(64 * scale_)))
        rt_->DrawBitmap(ic, R(x + cw / 2 - 30, y + 28, x + cw / 2 + 30, y + 88), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    text(L"Accedi a TeleFiles", R(x, y + 96, x + cw, y + 128), fHeader_.Get(), pal_.text, DWRITE_TEXT_ALIGNMENT_CENTER);

    float cy = y + 140;
    D2D1_RECT_F body = R(x + 36, cy, x + cw - 36, y + ch - 24);

    if (!configError_.empty()) {
        icon(Ic::Alert, R(body.left, cy + 8, body.right, cy + 40), pal_.error, 26, true);
        text(configError_, R(body.left, cy + 52, body.right, body.bottom), fWrap_.Get(), pal_.text2,
             DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        return;
    }
    AuthInfo a = eng_.auth();

    if (a.state == AuthState::Starting || a.state == AuthState::LoggingOut) {
        text(a.state == AuthState::Starting ? L"Connessione a Telegram…" : L"Disconnessione…",
             R(body.left, cy + 60, body.right, cy + 100), fBody_.Get(), pal_.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    if (a.state == AuthState::Error) {
        text(U(a.error), R(body.left, cy + 30, body.right, cy + 200), fWrap_.Get(), pal_.error,
             DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        return;
    }

    bool phoneFlow = a.state == AuthState::WaitCode || a.state == AuthState::WaitPassword;
    if (!phoneFlow) {
        // controllo segmentato QR / numero
        const wchar_t* tabs[2] = {L"Codice QR", L"Numero di telefono"};
        D2D1_RECT_F seg = R(x + cw / 2 - 150, cy, x + cw / 2 + 150, cy + 32);
        fill(seg, pal_.track, 8);
        for (int i = 0; i < 2; ++i) {
            D2D1_RECT_F r = R(seg.left + 2 + (float)i * 148, seg.top + 2, seg.left + 2 + (float)(i + 1) * 148, seg.bottom - 2);
            if (loginTab_ == i) {
                fill(r, pal_.surface, 6);
                stroke(r, pal_.border, 6);
            }
            text(tabs[i], r, loginTab_ == i ? fSemi_.Get() : fBody_.Get(), loginTab_ == i ? pal_.text : pal_.text2,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
            addHit(r, Act::LoginTab, i);
        }
        cy += 48;
    }

    if (!phoneFlow && loginTab_ == 0) {
        if (a.state == AuthState::WaitPhone && !qrRequested_ && !a.busy) {
            qrRequested_ = true;
            eng_.requestQr();
        }
        D2D1_RECT_F q = R(x + cw / 2 - 100, cy, x + cw / 2 + 100, cy + 200);
        fill(q, rgb(0xFFFFFF), 10);
        stroke(q, pal_.border, 10);
        if (a.state == AuthState::WaitQr && !a.qrLink.empty()) drawQr(a.qrLink, R(q.left + 12, q.top + 12, q.right - 12, q.bottom - 12));
        else text(L"Generazione del codice…", q, fSmall_.Get(), rgb(0x5F6368), DWRITE_TEXT_ALIGNMENT_CENTER);
        text(L"Apri Telegram sul telefono → Impostazioni → Dispositivi → Collega dispositivo, poi inquadra il codice.",
             R(body.left, cy + 212, body.right, cy + 260), fWrapSm_.Get(), pal_.text2, DWRITE_TEXT_ALIGNMENT_CENTER,
             DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    } else {
        std::wstring label, hint;
        if (a.state == AuthState::WaitCode) {
            label = L"Codice di accesso";
            hint = U(a.codeInfo);
        } else if (a.state == AuthState::WaitPassword) {
            label = L"Verifica in due passaggi";
            hint = a.passwordHint.empty() ? L"Inserisci la password del tuo account."
                                          : L"Suggerimento: " + U(a.passwordHint);
        } else {
            label = L"Numero di telefono";
            hint = L"Con il prefisso internazionale. Riceverai un codice nell'app Telegram.";
        }
        text(label, R(body.left, cy, body.right, cy + 20), fSemi_.Get(), pal_.text);
        D2D1_RECT_F in = R(body.left, cy + 26, body.right, cy + 60);
        fill(in, pal_.inputBg, 6);
        stroke(in, pal_.accent, 6, 1.5f);
        text(hint, R(body.left, cy + 68, body.right, cy + 120), fWrapSm_.Get(), pal_.text2,
             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        button(R(body.left, cy + 130, body.right, cy + 164), a.busy ? L"Attendi…" : L"Continua", Act::LoginSubmit,
               0, true);
        if (phoneFlow) {
            D2D1_RECT_F back = R(body.left, cy + 172, body.right, cy + 200);
            text(L"Usa un altro numero", back, fSmall_.Get(), hovered(Act::LoginRetry, 0) ? pal_.text : pal_.accent,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
            addHit(back, Act::LoginRetry, 0);
        }
    }
    if (!a.error.empty()) {
        bool form = phoneFlow || loginTab_ == 1;
        D2D1_RECT_F er = form ? R(body.left, cy + 206, body.right, cy + 256) : R(body.left, body.bottom - 44, body.right, body.bottom);
        text(U(a.error), er, fWrapSm_.Get(), pal_.error, DWRITE_TEXT_ALIGNMENT_CENTER,
             form ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    }
}

void MainWindow::drawQr(const std::string& link, D2D1_RECT_F r) {
    using qrcodegen::QrCode;
    QrCode qr = QrCode::encodeText(link.c_str(), QrCode::Ecc::MEDIUM);
    int n = qr.getSize();
    float cell = std::floor((r.right - r.left) / (float)n * scale_) / scale_;
    float size = cell * (float)n;
    float ox = (r.left + r.right - size) / 2, oy = (r.top + r.bottom - size) / 2;
    brush_->SetColor(rgb(0x111111));
    for (int yy = 0; yy < n; ++yy)
        for (int xx = 0; xx < n; ++xx)
            if (qr.getModule(xx, yy))
                rt_->FillRectangle(R(ox + (float)xx * cell, oy + (float)yy * cell, ox + (float)(xx + 1) * cell + 0.3f, oy + (float)(yy + 1) * cell + 0.3f),
                                   brush_.Get());
}

void MainWindow::drawScrollbar(ScrollId id) {
    ScrollArea& s = scroll_[id];
    s.thumb = s.track = R(0, 0, 0, 0);
    float maxPos = s.content - s.view;
    if (maxPos <= 1 || s.view <= 0) return;
    // fuori dal bordo di ridimensionamento della finestra (6 px a destra)
    s.track = R(s.area.right - 17, s.area.top + 3, s.area.right - 7, s.area.bottom - 3);
    float trackH = s.track.bottom - s.track.top;
    float thumbH = std::max(30.f, trackH * s.view / s.content);
    float top = s.track.top + (trackH - thumbH) * (s.pos / maxPos);
    s.thumb = R(s.track.left, top, s.track.right, top + thumbH);
    bool active = dragScroll_ == id || hovered(Act::ScrollThumb, id) || hovered(Act::ScrollTrack, id);
    float w = active ? 7.f : 5.f;
    float cx = (s.track.left + s.track.right) / 2 + 1;
    if (active) fill(R(cx - w / 2 - 1, s.track.top, cx + w / 2 + 1, s.track.bottom), pal_.overlay, w / 2 + 1);
    D2D1_COLOR_F c = pal_.text3;
    c.a = active ? 0.85f : 0.5f;
    fill(R(cx - w / 2, s.thumb.top, cx + w / 2, s.thumb.bottom), c, w / 2);
    addHit(s.track, Act::ScrollTrack, id);
    addHit(s.thumb, Act::ScrollThumb, id);
}

void MainWindow::drawSidebar(float W, float H) {
    (void)W;
    // ricerca chat (il controllo EDIT nativo sta sopra) e pulsante "+"
    D2D1_RECT_F sr = chatSearchRect();
    fill(sr, pal_.inputBg, 8);
    stroke(sr, GetFocus() == edChats_ ? pal_.accent : pal_.inputBorder, 8, GetFocus() == edChats_ ? 1.5f : 1.f);
    icon(Ic::Search, R(sr.left + 4, sr.top, sr.left + 26, sr.bottom), pal_.text3, 14);
    iconButton(R(kSideW - 42, sr.top, kSideW - 12, sr.bottom), Ic::Plus, Act::NewChat, 0, {}, nullptr, false, 16, 1.9f);

    Profile prof = eng_.profile();
    float top = kTitleH + 44, bottom = H - ((!prof.premium && prof.valid) ? 108 : 64);
    auto chats = eng_.chats();
    int64_t sel = eng_.selectedChat();
    std::wstring flt = chatFilter_;
    for (auto& c : flt) c = (wchar_t)towlower(c);

    float rowH = 44;
    std::vector<const Chat*> vis;
    for (auto& c : chats) {
        if (!flt.empty()) {
            std::wstring t = U(c.title);
            for (auto& ch : t) ch = (wchar_t)towlower(ch);
            if (t.find(flt) == std::wstring::npos) continue;
        }
        vis.push_back(&c);
    }
    ScrollArea& sc = scroll_[SC_CHATS];
    sc.area = R(0, top, kSideW, bottom);
    sc.view = bottom - top;
    sc.content = (float)vis.size() * rowH + 8;
    sc.pos = clampf(sc.pos, 0, std::max(0.f, sc.content - sc.view));
    const size_t firstRowHit = hits_.size();
    rt_->PushAxisAlignedClip(sc.area, D2D1_ANTIALIAS_MODE_ALIASED);
    float y = top - sc.pos;
    for (auto* c : vis) {
        D2D1_RECT_F r = R(8, y + 2, kSideW - 10, y + rowH - 2);
        if (y + rowH > top && y < bottom) {
            bool isSel = c->id == sel;
            bool hv = hovered(Act::Chat, c->id) || hovered(Act::ChatMenu, c->id);
            if (isSel) fill(r, pal_.selected, 8);
            else if (hv) fill(r, pal_.hover, 8);
            if (isSel) fill(R(r.left, r.top + 12, r.left + 3, r.bottom - 12), pal_.accent, 1.5f);
            addHit(r, Act::Chat, c->id);
            D2D1_RECT_F av = R(r.left + 10, r.top + 6, r.left + 38, r.top + 34);
            if (c->kind == ChatKind::Saved) {
                D2D1_GRADIENT_STOP stops[2] = {{0.f, rgb(0x5AB8FF)}, {1.f, rgb(0x2B82E8)}};
                ComPtr<ID2D1GradientStopCollection> gs;
                ComPtr<ID2D1LinearGradientBrush> lg;
                rt_->CreateGradientStopCollection(stops, 2, gs.GetAddressOf());
                if (gs) rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, av.top), D2D1::Point2F(0, av.bottom)),
                                                       gs.Get(), lg.GetAddressOf());
                if (lg) rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F((av.left + av.right) / 2, (av.top + av.bottom) / 2), 14, 14), lg.Get());
                icon(Ic::Saved, av, rgb(0xFFFFFF), 15, false, 1.9f);
            } else {
                avatar(av, U(c->title), c->id);
            }
            std::wstring sub = c->kind == ChatKind::Saved ? L"Il tuo spazio file"
                             : c->kind == ChatKind::Channel ? (c->owner ? L"Canale · proprietario" : L"Canale")
                             : c->kind == ChatKind::Group   ? (c->owner ? L"Gruppo · proprietario" : L"Gruppo")
                                                            : L"Chat privata";
            float right = r.right - 8;
            bool menuable = c->kind == ChatKind::Channel || c->kind == ChatKind::Group;
            if (menuable && (hv || isSel)) {
                iconButton(R(right - 24, r.top + 8, right, r.top + 32), Ic::More, Act::ChatMenu, c->id, {}, nullptr, false, 16);
                right -= 28;
            } else if (c->unread > 0) {
                std::wstring u = c->unread > 99 ? L"99+" : std::to_wstring(c->unread);
                float bw = std::max(18.f, measure(u, fBadge_.Get()) + 10);
                D2D1_RECT_F b = R(right - bw, r.top + 13, right, r.top + 31);
                fill(b, pal_.accent, 9);
                text(u, b, fBadge_.Get(), pal_.accentText, DWRITE_TEXT_ALIGNMENT_CENTER);
                right -= bw + 6;
            }
            text(U(c->title), R(r.left + 48, r.top + 4, right, r.top + 22), fSemi_.Get(), pal_.text);
            text(sub, R(r.left + 48, r.top + 21, right, r.top + 37), fSmall_.Get(), pal_.text2);
        }
        y += rowH;
    }
    if (vis.empty())
        text(chats.empty() ? L"Caricamento chat…" : L"Nessuna chat trovata", R(16, top + 8, kSideW - 16, top + 40),
             fSmall_.Get(), pal_.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
    rt_->PopAxisAlignedClip();
    clipRowHits(hits_, firstRowHit, sc.area);
    drawScrollbar(SC_CHATS);

    // profilo
    float py = H - 60;
    if (!prof.premium && prof.valid) {
        // invito a Premium ben visibile: raddoppia il limite dei file
        D2D1_RECT_F ur = R(12, H - 100, kSideW - 12, H - 68);
        bool hv = hovered(Act::Upgrade, 0);
        fill(ur, pal_.goldSoft, 8);
        if (hv) stroke(ur, pal_.gold, 8);
        icon(Ic::Star, R(ur.left + 8, ur.top, ur.left + 28, ur.bottom), pal_.gold, 15, true);
        text(L"Passa a Premium", R(ur.left + 32, ur.top, ur.right - 60, ur.bottom), fSmallSemi_.Get(), pal_.gold);
        text(L"file 4 GB", R(ur.right - 70, ur.top, ur.right - 10, ur.bottom), fSmall_.Get(), pal_.gold,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
        addHit(ur, Act::Upgrade);
    }
    line(12, py, kSideW - 12, py, pal_.border);
    ComPtr<ID2D1Bitmap> photo = prof.photoPath.empty() ? nullptr : loadBitmap(prof.photoPath);
    std::wstring name = U(prof.firstName + (prof.lastName.empty() ? "" : " " + prof.lastName));
    avatar(R(14, py + 12, 50, py + 48), name.empty() ? L"?" : name, prof.userId, photo.Get());
    text(name, R(58, py + 10, kSideW - 44, py + 29), fSemi_.Get(), pal_.text);
    if (prof.premium) {
        icon(Ic::Star, R(58, py + 30, 72, py + 46), pal_.gold, 13, true);
        text(L"Premium", R(75, py + 29, kSideW - 44, py + 47), fSmallSemi_.Get(), pal_.gold);
    } else {
        std::wstring sub = L"Account gratuito";
        if (!prof.username.empty()) sub = L"@" + U(prof.username) + L" · gratuito";
        text(sub, R(58, py + 29, kSideW - 44, py + 47), fSmall_.Get(), pal_.text2);
    }
    iconButton(R(kSideW - 42, py + 15, kSideW - 12, py + 45), Ic::Gear, Act::Settings, 0, {}, nullptr, false, 17, 1.5f);
}

void MainWindow::drawContent(const Layout& lay) {
    float W = lay.W, H = lay.H;
    float L = kSideW, T = kTitleH;
    // pannello contenuto con l'angolo in alto a sinistra arrotondato
    fill(R(L, T, W + 20, H + 20), pal_.surface, 10);
    stroke(R(L, T, W + 20, H + 20), pal_.border, 10);

    // barra strumenti
    auto chats = eng_.chats();
    int64_t sel = eng_.selectedChat();
    std::wstring title;
    for (auto& c : chats) if (c.id == sel) title = U(c.title);
    entriesSnap_ = eng_.entries();
    sortEntries();
    // a elenco completo si contano le righe (un gruppo di parti vale 1), altrimenti il totale del server
    int64_t tot = eng_.listHasMore() ? eng_.totalCount() : (int64_t)entriesSnap_.size();
    if (eng_.listLoading() && entriesSnap_.empty()) tot = -1;
    std::wstring sub = tot >= 0 ? std::to_wstring(tot) + L" file" : L"";
    if ((sortCol_ != 0 || !sortDesc_) && eng_.listHasMore())
        sub += L" · carico l'elenco completo per ordinarlo (" + std::to_wstring(entriesSnap_.size()) + L")…";
    D2D1_RECT_F sr = fileSearchRect(W);
    text(title, R(L + 20, T + 6, sr.left - 12, T + 27), fTitle_.Get(), pal_.text);
    text(sub, R(L + 20, T + 27, sr.left - 12, T + 44), fSmall_.Get(), pal_.text2);

    D2D1_RECT_F up = R(W - 16 - 112, T + 10, W - 16, T + 40);
    button(up, L"Carica", Act::Upload, 0, true, Ic::Upload, true);
    icon(Ic::ChevronDown, R(up.right - 22, up.top, up.right - 8, up.bottom), pal_.accentText, 12, false, 2.0f);
    fill(sr, pal_.inputBg, 8);
    bool focus = GetFocus() == edFiles_;
    stroke(sr, focus ? pal_.accent : pal_.inputBorder, 8, focus ? 1.5f : 1.f);
    icon(Ic::Search, R(sr.left + 4, sr.top, sr.left + 26, sr.bottom), pal_.text3, 14);

    // filtri (controllo segmentato) e azioni sulla selezione
    float fy = T + kToolH;
    const wchar_t* fl[5] = {L"Tutti", L"Documenti", L"Video", L"Audio", L"Foto"};
    Filter cur = eng_.filter();
    float segW = 0;
    float wseg[5];
    for (int i = 0; i < 5; ++i) { wseg[i] = measure(fl[i], fSemi_.Get()) + (lay.narrow ? 16 : 22); segW += wseg[i]; }
    D2D1_RECT_F seg = R(L + 16, fy + 4, L + 16 + segW + 4, fy + 32);
    fill(seg, pal_.track, 8);
    float fx = seg.left + 2;
    for (int i = 0; i < 5; ++i) {
        D2D1_RECT_F r = R(fx, seg.top + 2, fx + wseg[i], seg.bottom - 2);
        bool on = (int)cur == i;
        if (on) {
            fill(r, pal_.surface, 6);
            stroke(r, pal_.border, 6);
        } else if (hovered(Act::Filter, i)) {
            fill(r, pal_.overlay, 6);
        }
        text(fl[i], r, on ? fSemi_.Get() : fBody_.Get(), on ? pal_.text : pal_.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
        addHit(r, Act::Filter, i);
        fx += wseg[i];
    }
    // rimuove dalla selezione ciò che non è più in lista
    {
        std::set<std::string> keep;
        for (auto& e : entriesSnap_) if (sel_.count(e.key)) keep.insert(e.key);
        sel_.swap(keep);
    }
    float actLeft = W - 16;
    if (!sel_.empty()) {
        std::wstring n = L" (" + std::to_wstring(sel_.size()) + L")";
        std::wstring dl = (lay.narrow ? L"Scarica" : L"Scarica selezionati") + n;
        float w1 = measure(dl, fBody_.Get()) + 46;
        float w2 = measure(L"Elimina", fBody_.Get()) + 46;
        button(R(actLeft - w1, fy + 5, actLeft, fy + 33), dl, Act::DownloadSel, 0, false, Ic::Download, true);
        actLeft -= w1 + 8;
        button(R(actLeft - w2, fy + 5, actLeft, fy + 33), L"Elimina", Act::DeleteSel, 0, false, Ic::Trash, true, true);
        actLeft -= w2 + 8;
    }
    std::string q = eng_.query();
    if (!q.empty() && actLeft - (seg.right + 12) > 80) {
        std::wstring info = std::to_wstring(entriesSnap_.size()) + (eng_.listHasMore() ? L"+" : L"") +
                            L" risultati per «" + U(q) + L"»";
        text(info, R(seg.right + 12, fy, actLeft - 8, fy + kFilterH), fSmall_.Get(), pal_.text2);
    }

    float listTop = fy + kFilterH;
    drawFileList(R(L, listTop, W, lay.transTop), lay.narrow);
    drawTransfers(R(L, lay.transTop, W, H), lay.collapsed);
    // maniglia per ridimensionare il pannello trasferimenti (sopra a tutto il resto della lista)
    if (!lay.collapsed) {
        D2D1_RECT_F grip = R(L, lay.transTop - 4, W, lay.transTop + 4);
        if (hovered(Act::Splitter, 0) || dragSplit_) fill(R(L, lay.transTop - 1, W, lay.transTop + 1), pal_.accent);
        addHit(grip, Act::Splitter, 0);
    }
}

void MainWindow::drawFileList(D2D1_RECT_F a, bool narrow) {
    float L = a.left + 12, Rr = a.right - 20;
    float cw = Rr - L;
    bool showDate = cw >= 640;
    float wStatus = narrow ? 118.f : 150.f, wDate = showDate ? 104.f : 0.f, wSize = 96.f;
    float cCheck = L + 8, cName = L + 38, cStatus = Rr - wStatus, cDate = cStatus - wDate, cSize = cDate - wSize;
    D2D1_RECT_F head = R(L, a.top, Rr, a.top + kHeadH);
    if (a.bottom - a.top < kHeadH + 4) return;
    // casella "seleziona tutto"
    bool all = !entriesSnap_.empty() && sel_.size() == entriesSnap_.size();
    D2D1_RECT_F cb = R(cCheck, head.top + 5, cCheck + 16, head.top + 21);
    if (all) { fill(cb, pal_.accent, 4); icon(Ic::Check, cb, pal_.accentText, 12, false, 2.4f); }
    else stroke(cb, hovered(Act::CheckAll, 0) ? pal_.text2 : pal_.text3, 4);
    addHit(R(cCheck - 6, head.top, cCheck + 22, head.bottom), Act::CheckAll);
    // intestazioni cliccabili: ordina per nome, dimensione o data (secondo clic = ordine inverso)
    auto header = [&](int col, const wchar_t* label, D2D1_RECT_F r, bool right) {
        bool on = sortCol_ == col, hv = hovered(Act::SortCol, col);
        if (hv) fill(R(r.left - 6, r.top + 2, r.right + 4, r.bottom - 2), pal_.hoverSurface, 4);
        float tw = measure(label, fSmallSemi_.Get());
        float tx = right ? r.right - tw - (on ? 16 : 0) : r.left;
        text(label, R(tx, r.top, tx + tw + 2, r.bottom), fSmallSemi_.Get(), on ? pal_.text : pal_.text2);
        if (on) icon(sortDesc_ ? Ic::ChevronDown : Ic::ChevronUp, R(tx + tw + 3, r.top, tx + tw + 15, r.bottom), pal_.accent, 11, false, 2.2f);
        addHit(R(r.left - 6, r.top, r.right + 4, r.bottom), Act::SortCol, col);
    };
    header(1, L"Nome", R(cName, head.top, cSize - 24, head.bottom), false);
    header(2, L"Dimensione", R(cSize - 10, head.top, cDate - 16, head.bottom), true);
    if (showDate) header(0, L"Data", R(cDate, head.top, cStatus - 12, head.bottom), false);
    text(L"Stato", R(cStatus, head.top, Rr, head.bottom), fSmallSemi_.Get(), pal_.text2);
    line(L, head.bottom - 0.5f, Rr, head.bottom - 0.5f, pal_.border);

    ScrollArea& sc = scroll_[SC_LIST];
    sc.area = R(a.left, head.bottom, a.right, a.bottom);
    sc.view = sc.area.bottom - sc.area.top;
    sc.content = (float)entriesSnap_.size() * kRowH + 8 + (eng_.listHasMore() ? kRowH : 0);
    sc.pos = clampf(sc.pos, 0, std::max(0.f, sc.content - sc.view));

    // stato dei trasferimenti per riga
    std::map<std::string, TransferView> tmap;
    for (auto& t : eng_.transfers())
        if (t.dir == Direction::Download) tmap[t.entryKey] = t;

    const size_t firstRowHit = hits_.size();
    rt_->PushAxisAlignedClip(sc.area, D2D1_ANTIALIAS_MODE_ALIASED);
    float y = sc.area.top + 2 - sc.pos;
    int idx = 0;
    for (auto& e : entriesSnap_) {
        D2D1_RECT_F r = R(L, y, Rr, y + kRowH - 2);
        if (r.bottom > sc.area.top && r.top < sc.area.bottom) {
            bool selRow = sel_.count(e.key) > 0;
            if (selRow) fill(r, pal_.accentSoft, 6);
            else if (hovered(Act::Row, idx, e.key) || hovered(Act::RowAction, idx, e.key) || hovered(Act::RowCheck, idx, e.key))
                fill(r, pal_.hoverSurface, 6);
            float cy = (r.top + r.bottom) / 2;
            addHit(r, Act::Row, idx, e.key);

            D2D1_RECT_F c = R(cCheck, cy - 8, cCheck + 16, cy + 8);
            if (selRow) { fill(c, pal_.accent, 4); icon(Ic::Check, c, pal_.accentText, 12, false, 2.4f); }
            else stroke(c, pal_.text3, 4);
            addHit(R(cCheck - 6, r.top, cCheck + 22, r.bottom), Act::RowCheck, idx, e.key);

            docIcon(R(cName, cy - 13, cName + 24, cy + 13), e);

            float nameRight = cSize - 12;
            if (e.isGroup) {
                std::wstring pt = std::to_wstring(e.partCount) + L" parti";
                float w = measure(pt, fSmall_.Get()) + 14;
                D2D1_RECT_F pr = R(nameRight - w, cy - 10, nameRight, cy + 10);
                fill(pr, pal_.track, 10);
                text(pt, pr, fSmall_.Get(), pal_.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
                nameRight -= w + 8;
            }
            float nx = cName + 34;
            if (!e.folder.empty()) {
                // percorso della cartella, attenuato, prima del nome (troncato se lungo)
                std::wstring f = U(e.folder) + L"/";
                float fw = std::min(measure(f, fBody_.Get()), (nameRight - nx) * 0.45f);
                icon(Ic::Folder, R(nx, cy - 8, nx + 16, cy + 8), pal_.text3, 14, true, 1.6f);
                text(f, R(nx + 19, r.top, nx + 19 + fw, r.bottom), fBody_.Get(), pal_.text3);
                nx += 19 + fw + 1;
            }
            text(U(e.name), R(nx, r.top, nameRight, r.bottom), fBody_.Get(), pal_.text);
            text(U(formatSize(e.size)), R(cSize, r.top, cDate - 16, r.bottom), fBody_.Get(), pal_.text2,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
            if (showDate) text(fmtDate(e.date), R(cDate, r.top, cStatus - 8, r.bottom), fBody_.Get(), pal_.text2);

            // colonna stato
            D2D1_RECT_F st = R(cStatus, r.top, Rr - 4, r.bottom);
            auto it = tmap.find(e.key);
            if (it != tmap.end() && it->second.status != TStatus::Done) {
                const TransferView& t = it->second;
                if (t.status == TStatus::Queued) {
                    text(L"In coda", st, fSmall_.Get(), pal_.text2);
                } else if (t.status == TStatus::Error) {
                    icon(Ic::Alert, R(st.left, cy - 8, st.left + 16, cy + 8), pal_.error, 15, true);
                    text(L"Errore", R(st.left + 21, r.top, st.right, r.bottom), fSmall_.Get(), pal_.error);
                } else {
                    double frac = t.total > 0 ? (double)t.done / (double)t.total : 0;
                    bool paused = t.status == TStatus::Paused;
                    progressBar(R(st.left, cy - 2.5f, st.right - 44, cy + 2.5f), frac, paused ? pal_.text3 : pal_.accent);
                    std::wstring pct = paused ? L"Pausa" : std::to_wstring((int)(frac * 100)) + L"%";
                    text(pct, R(st.right - 40, r.top, st.right, r.bottom), fSmallSemi_.Get(),
                         paused ? pal_.text2 : pal_.accent, DWRITE_TEXT_ALIGNMENT_TRAILING);
                }
            } else if ((it != tmap.end() && it->second.status == TStatus::Done) || eng_.alreadyDownloaded(e)) {
                icon(Ic::Check, R(st.left, cy - 8, st.left + 16, cy + 8), pal_.success, 14, false, 2.2f);
                text(L"Scaricato", R(st.left + 21, r.top, st.right, r.bottom), fSmall_.Get(), pal_.success);
            } else {
                float w = narrow ? 30.f : 92.f;
                D2D1_RECT_F b = R(st.left, cy - 12, st.left + w, cy + 12);
                bool hv = hovered(Act::RowAction, idx, e.key);
                fill(b, hv ? pal_.accent : (selRow ? pal_.surface : pal_.accentSoft), 12);
                D2D1_COLOR_F fg = hv ? pal_.accentText : pal_.accent;
                if (narrow) {
                    icon(Ic::Download, b, fg, 14, false, 2.0f);
                } else {
                    icon(Ic::Download, R(b.left + 9, b.top, b.left + 25, b.bottom), fg, 14, false, 2.0f);
                    text(L"Scarica", R(b.left + 29, b.top, b.right - 6, b.bottom), fSmallSemi_.Get(), fg);
                }
                addHit(b, Act::RowAction, idx, e.key);
            }
        }
        y += kRowH;
        ++idx;
    }
    if (entriesSnap_.empty()) {
        std::wstring msg = eng_.listLoading() ? L"Caricamento…"
                           : !eng_.query().empty() ? L"Nessun file corrisponde alla ricerca."
                                                   : L"Nessun file in questa chat.\nTrascina qui file o cartelle da caricare.";
        if (!eng_.listLoading() && eng_.query().empty())
            icon(Ic::FolderPlus, R(sc.area.left, sc.area.top + 30, sc.area.right, sc.area.top + 70), pal_.text3, 36, true, 1.4f);
        text(msg, R(sc.area.left, sc.area.top + 78, sc.area.right, sc.area.top + 130), fWrap_.Get(), pal_.text2,
             DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    } else if (eng_.listLoading() || eng_.listHasMore()) {
        text(L"Caricamento…", R(sc.area.left, y, sc.area.right, y + kRowH), fSmall_.Get(), pal_.text3,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    rt_->PopAxisAlignedClip();
    clipRowHits(hits_, firstRowHit, sc.area);
    drawScrollbar(SC_LIST);

    // pagina successiva quando ci si avvicina al fondo; ordinando per nome o dimensione serve tutto l'elenco
    bool needAll = sortCol_ != 0 || !sortDesc_;
    if ((needAll || sc.content - sc.view - sc.pos < 300) && eng_.listHasMore() && !eng_.listLoading()) eng_.loadMore();
}

void MainWindow::drawTransfers(D2D1_RECT_F a, bool collapsed) {
    fill(R(a.left, a.top, a.right + 20, a.bottom + 20), pal_.surfaceAlt);
    line(a.left, a.top + 0.5f, a.right, a.top + 0.5f, pal_.border);
    float L = a.left + 16, Rr = a.right - 16;
    bool narrow = a.right - a.left < 760;

    auto all = eng_.transfers();
    std::vector<TransferView> lists[3];
    for (auto& t : all) {
        if (t.status == TStatus::Done) lists[2].push_back(t);
        else if (t.status == TStatus::Queued) lists[1].push_back(t);
        else lists[0].push_back(t);
    }
    std::reverse(lists[2].begin(), lists[2].end());

    // intestazione: titolo, schede, azioni, apri/chiudi
    iconButton(R(Rr - 26, a.top + 6, Rr, a.top + 32), collapsed ? Ic::ChevronUp : Ic::ChevronDown, Act::TCollapse, 0,
               {}, nullptr, false, 14, 2.0f);
    float actRight = Rr - 34;
    text(L"Trasferimenti", R(L, a.top + 4, L + 110, a.top + 34), fSemi_.Get(), pal_.text);
    addHit(R(L, a.top + 4, L + 100, a.top + 34), Act::TCollapse, 0);
    const wchar_t* tabs[3] = {L"In corso", L"In coda", L"Completati"};
    float tx = L + 104;
    D2D1_RECT_F seg = R(tx, a.top + 7, tx, a.top + 31);
    float wt[3];
    for (int i = 0; i < 3; ++i) {
        std::wstring lbl = std::wstring(tabs[i]) + L" " + std::to_wstring(lists[i].size());
        wt[i] = measure(lbl, fSmallSemi_.Get()) + 18;
        seg.right += wt[i];
    }
    seg.right += 4;
    fill(seg, pal_.track, 7);
    tx += 2;
    for (int i = 0; i < 3; ++i) {
        std::wstring lbl = std::wstring(tabs[i]) + L" " + std::to_wstring(lists[i].size());
        D2D1_RECT_F r = R(tx, seg.top + 2, tx + wt[i], seg.bottom - 2);
        bool on = tTab_ == i && !collapsed;
        if (on) { fill(r, pal_.surface, 5); stroke(r, pal_.border, 5); }
        else if (hovered(Act::TTab, i)) fill(r, pal_.overlay, 5);
        text(lbl, r, on ? fSmallSemi_.Get() : fSmall_.Get(), on ? pal_.text : pal_.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
        addHit(r, Act::TTab, i);
        tx += wt[i];
    }
    // azioni globali (solo se c'è spazio a destra delle schede)
    bool anyRunning = false, anyPaused = false;
    for (auto& t : all) {
        if (t.status == TStatus::Paused || t.status == TStatus::Error) anyPaused = true;
        else if (t.status != TStatus::Done) anyRunning = true;
    }
    struct GA { std::wstring lbl; Act act; Ic ic; bool danger; };
    std::vector<GA> acts;  // da destra verso sinistra
    if (tTab_ == 2 && !collapsed) {
        if (!lists[2].empty()) acts.push_back({L"Pulisci elenco", Act::ClearDone, Ic::Trash, false});
    } else {
        if (anyRunning || anyPaused) acts.push_back({L"Annulla tutti", Act::CancelAll, Ic::Close, true});
        if (anyRunning) acts.push_back({L"Pausa tutti", Act::PauseAll, Ic::Pause, false});
        if (anyPaused) acts.push_back({L"Riprendi tutti", Act::ResumeAll, Ic::Play, false});
    }
    // con poco spazio i pulsanti diventano solo icone
    float need = 0;
    for (auto& g : acts) need += measure(g.lbl, fBody_.Get()) + 44 + 8;
    bool compact = actRight - need < seg.right + 12;
    for (auto& g : acts) {
        float w = compact ? 32 : measure(g.lbl, fBody_.Get()) + 44;
        if (actRight - w < seg.right + 12) break;
        button(R(actRight - w, a.top + 7, actRight, a.top + 31), compact ? L"" : g.lbl, g.act, 0, false, g.ic, true, g.danger);
        actRight -= w + 8;
    }

    // righe
    ScrollArea& sc = scroll_[SC_TRANSFERS];
    sc.area = collapsed ? R(0, 0, 0, 0) : R(a.left, a.top + kTHeadH, a.right, a.bottom - kStatusH);
    auto& list = lists[tTab_];
    sc.view = sc.area.bottom - sc.area.top;
    sc.content = collapsed ? 0 : (float)list.size() * kTRowH + 4;
    sc.pos = clampf(sc.pos, 0, std::max(0.f, sc.content - sc.view));
    if (!collapsed && sc.view > 0) {
        const size_t firstRowHit = hits_.size();
        rt_->PushAxisAlignedClip(sc.area, D2D1_ANTIALIAS_MODE_ALIASED);
        float y = sc.area.top - sc.pos;
        for (auto& t : list) {
            D2D1_RECT_F r = R(L - 4, y + 2, Rr - 6, y + kTRowH - 2);
            if (r.bottom > sc.area.top && r.top < sc.area.bottom) {
                fill(r, pal_.surface, 8);
                stroke(r, pal_.border, 8);
                addHit(r, Act::None, t.id, "t");  // prima dei pulsanti, che restano sopra
                float cy = (r.top + r.bottom) / 2;
                bool down = t.dir == Direction::Download;
                D2D1_COLOR_F dc = t.status == TStatus::Error ? pal_.error
                                : t.status == TStatus::Paused ? pal_.text3 : (down ? pal_.accent : pal_.success);
                // icona in un cerchio tenue
                D2D1_COLOR_F soft = dc;
                soft.a = 0.14f;
                brush_->SetColor(soft);
                rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.left + 20, cy), 12, 12), brush_.Get());
                icon(t.status == TStatus::Error ? Ic::Alert : (down ? Ic::Download : Ic::Upload),
                     R(r.left + 8, cy - 12, r.left + 32, cy + 12), dc, 15, false, 2.0f);

                float bx = r.right - 66;
                float statusW = narrow ? 94.f : 128.f;
                float barR = bx - 10 - statusW;
                float barL = std::max(r.left + 40 + 180, r.left + (r.right - r.left) * 0.42f);
                if (barR - barL < 50) barL = barR;  // poco spazio: niente barra, solo testo
                float textR = barL - 14;
                text(U(t.name), R(r.left + 40, r.top + 4, textR, r.top + 22), fBody_.Get(), pal_.text);
                std::wstring sub;
                if (t.status == TStatus::Error) sub = U(t.error);
                else {
                    if (t.status == TStatus::Done) sub = U(formatSize(t.total));
                    else sub = U(formatSize(t.done)) + L" di " + U(formatSize(t.total));
                    if (t.partCount > 1)
                        sub += t.status == TStatus::Done
                                   ? L" · " + std::to_wstring(t.partCount) + L" parti"
                                   : L" · parte " + std::to_wstring(std::min(t.part, t.partCount)) + L"/" +
                                         std::to_wstring(t.partCount);
                    if (!t.chatTitle.empty()) sub += L" · " + U(t.chatTitle);
                }
                text(sub, R(r.left + 40, r.top + 21, textR, r.top + 38), fSmall_.Get(),
                     t.status == TStatus::Error ? pal_.error : pal_.text2);

                if (t.status != TStatus::Done && t.status != TStatus::Error) {
                    double frac = t.total > 0 ? (double)t.done / (double)t.total : 0;
                    bool prep = t.status == TStatus::Preparing && t.prep >= 0;
                    if (barL < barR) {
                        progressBar(R(barL, cy - 3, barR, cy + 3), frac, dc);
                        // estrazione della parte: sottile barra secondaria sotto quella principale
                        if (prep) progressBar(R(barL, cy + 5, barR, cy + 7), t.prep, pal_.text3);
                    }
                    std::wstring right;
                    switch (t.status) {
                        case TStatus::Queued: right = L"In coda"; break;
                        case TStatus::Paused: right = L"In pausa"; break;
                        case TStatus::Preparing:
                            if (down) right = L"Ricerca parti…";
                            else if (prep) {
                                right = narrow ? L"Preparo " : L"Preparo la parte " +
                                        std::to_wstring(std::min(t.part, t.partCount)) + L" · ";
                                right += std::to_wstring((int)(t.prep * 100)) + L"%";
                            }
                            else right = L"Preparazione…";
                            break;
                        case TStatus::Joining: right = down ? (t.partCount > 1 ? L"Unisco le parti…" : L"Salvataggio…") : L""; break;
                        case TStatus::Verifying: right = L"Verifica…"; break;
                        default: {
                            right = U(formatSpeed(t.speed));
                            std::string eta = formatEta(t.total - t.done, t.speed);
                            if (!eta.empty() && !narrow) right += L" · " + U(eta);
                        }
                    }
                    text(right, R(barR + 8, r.top, bx - 6, r.bottom), fSmall_.Get(), pal_.text2, DWRITE_TEXT_ALIGNMENT_TRAILING);
                } else if (t.status == TStatus::Done) {
                    text(down ? L"Completato" : L"Inviato", R(barR + 8, r.top, bx - 6, r.bottom), fSmall_.Get(), pal_.success,
                         DWRITE_TEXT_ALIGNMENT_TRAILING);
                }
                // pulsanti tondi
                D2D1_RECT_F b1 = R(bx + 2, cy - 13, bx + 28, cy + 13), b2 = R(bx + 32, cy - 13, bx + 58, cy + 13);
                std::string k = std::to_string(t.id);
                if (t.status == TStatus::Done) {
                    if (down) iconButton(b1, Ic::Folder, Act::TFolder, t.id, k, nullptr, false, 15);
                    iconButton(b2, Ic::Close, Act::TCancel, t.id, k, nullptr, false, 12, 2.0f);
                } else {
                    if (t.status == TStatus::Paused || t.status == TStatus::Error)
                        iconButton(b1, t.status == TStatus::Error ? Ic::Retry : Ic::Play, Act::TResume, t.id, k, nullptr,
                                   true, 13, 2.0f);
                    else
                        iconButton(b1, Ic::Pause, Act::TPause, t.id, k, nullptr, false, 14, 2.8f);
                    iconButton(b2, Ic::Close, Act::TCancel, t.id, k, nullptr, false, 12, 2.0f);
                }
            }
            y += kTRowH;
        }
        if (list.empty()) {
            const wchar_t* m = tTab_ == 0 ? L"Nessun trasferimento in corso. Trascina file o cartelle nella finestra per caricarli."
                             : tTab_ == 1 ? L"La coda è vuota." : L"Nessun trasferimento completato.";
            text(m, R(sc.area.left + 16, sc.area.top, sc.area.right - 16, sc.area.top + 40), fSmall_.Get(), pal_.text3,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        rt_->PopAxisAlignedClip();
        clipRowHits(hits_, firstRowHit, sc.area);
        drawScrollbar(SC_TRANSFERS);
    }

    // riga di stato
    double dn, upS;
    eng_.totalSpeed(dn, upS);
    Settings s = eng_.settings();
    float sy = a.bottom - kStatusH;
    // velocità con le icone (le frecce Unicode non ci sono in tutti i font)
    float sx = L;
    auto speed = [&](Ic ic, double v, D2D1_COLOR_F c) {
        icon(ic, R(sx, sy + 2, sx + 14, a.bottom - 6), c, 13, false, 2.0f);
        std::wstring t = U(formatSpeed(v));
        float w = measure(t, fSmall_.Get()) + 4;
        text(t, R(sx + 17, sy, sx + 17 + w, a.bottom - 4), fSmall_.Get(), pal_.text2);
        sx += 17 + w + 14;
    };
    speed(Ic::Download, dn, pal_.accent);
    speed(Ic::Upload, upS, pal_.success);
    std::wstring st;
    if (!narrow) st = L"Paralleli: " + std::to_wstring(s.maxParallel) + L"    Limite file: " + U(formatSize(eng_.uploadLimit()));
    float stW = measure(st, fSmall_.Get()) + 8;
    text(st, R(sx, sy, sx + stW, a.bottom - 4), fSmall_.Get(), pal_.text3);
    stW += sx - L;
    D2D1_RECT_F dr = R(std::max(L + stW + 16, Rr - 360), sy, Rr, a.bottom - 4);
    bool hv = hovered(Act::DestFolder, 0);
    float dw = measure(U(s.downloadDir), fSmall_.Get());
    float ix = std::max(dr.left, dr.right - dw - 22);
    icon(Ic::Folder, R(ix, sy + 2, ix + 16, a.bottom - 6), hv ? pal_.accent : pal_.text3, 13, true, 1.6f);
    text(U(s.downloadDir), R(ix + 20, sy, dr.right, a.bottom - 4), fSmall_.Get(), hv ? pal_.accent : pal_.text2,
         DWRITE_TEXT_ALIGNMENT_TRAILING);
    addHit(dr, Act::DestFolder);
}

void MainWindow::drawToasts(float W, float bottom) {
    float y = bottom;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        float tw = std::min(measure(it->text, fBody_.Get()), W - kSideW - 120);
        float w = tw + 56, h = 38;
        float cx = kSideW + (W - kSideW) / 2;
        D2D1_RECT_F r = R(cx - w / 2, y - h, cx + w / 2, y);
        // ombra morbida
        fill(R(r.left, r.top + 2, r.right, r.bottom + 3), rgb(0x000000, pal_.dark ? 0.35f : 0.10f), 12);
        fill(r, pal_.dark ? rgb(0x3A3A3A) : rgb(0x2B2B2D), 10);
        icon(it->error ? Ic::Alert : Ic::Check, R(r.left + 10, r.top, r.left + 34, r.bottom),
             it->error ? rgb(0xFF8A80) : rgb(0x7CD992), 16, it->error, 2.0f);
        text(it->text, R(r.left + 40, r.top, r.right - 14, r.bottom), fBody_.Get(), rgb(0xFFFFFF));
        y -= h + 8;
    }
}

void MainWindow::drawModal(float W, float H) {
    fill(R(0, 0, W, H), rgb(0x000000, pal_.dark ? 0.45f : 0.28f));
    addHit(R(0, 0, W, H), Act::ModalBg);
    D2D1_RECT_F c = modalRect(W, H);
    fill(R(c.left, c.top + 4, c.right, c.bottom + 8), rgb(0x000000, 0.18f), 16);
    fill(c, pal_.surface, 14);
    stroke(c, pal_.border, 14);
    addHit(c, Act::Block);

    // icona e titolo
    D2D1_COLOR_F ac = pal_.accent;
    ac.a = 0.14f;
    brush_->SetColor(ac);
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(c.left + 44, c.top + 44), 20, 20), brush_.Get());
    icon(modalGroup_ ? Ic::Group : Ic::Channel, R(c.left + 24, c.top + 24, c.left + 64, c.top + 64), pal_.accent, 22, true, 1.8f);
    text(modalGroup_ ? L"Nuovo gruppo" : L"Nuovo canale", R(c.left + 76, c.top + 22, c.right - 24, c.top + 46), fTitle_.Get(), pal_.text);
    text(modalGroup_ ? L"Tutti i membri possono inviare file" : L"Solo tu (e gli amministratori) potete pubblicare",
         R(c.left + 76, c.top + 46, c.right - 24, c.top + 64), fSmall_.Get(), pal_.text2);

    // tipo: canale / gruppo
    D2D1_RECT_F seg = R(c.left + 24, c.top + 82, c.right - 24, c.top + 110);
    fill(seg, pal_.track, 8);
    const wchar_t* k[2] = {L"Canale", L"Gruppo"};
    float half = (seg.right - seg.left - 4) / 2;
    for (int i = 0; i < 2; ++i) {
        D2D1_RECT_F r = R(seg.left + 2 + (float)i * half, seg.top + 2, seg.left + 2 + (float)(i + 1) * half, seg.bottom - 2);
        bool on = (int)modalGroup_ == i;
        if (on) { fill(r, pal_.surface, 6); stroke(r, pal_.border, 6); }
        text(k[i], r, on ? fSemi_.Get() : fBody_.Get(), on ? pal_.text : pal_.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
        addHit(r, Act::ModalKind, i);
    }

    auto field = [&](float y, const wchar_t* label, HWND ed) {
        text(label, R(c.left + 24, y, c.right - 24, y + 18), fSmallSemi_.Get(), pal_.text2);
        D2D1_RECT_F in = R(c.left + 24, y + 22, c.right - 24, y + 54);
        fill(in, pal_.inputBg, 8);
        bool f = GetFocus() == ed;
        stroke(in, f ? pal_.accent : pal_.inputBorder, 8, f ? 1.5f : 1.f);
    };
    field(c.top + 122, L"Nome", edModalTitle_);
    field(c.top + 182, L"Descrizione (facoltativa)", edModalDesc_);
    if (!modalError_.empty())
        text(modalError_, R(c.left + 24, c.top + 240, c.right - 24, c.top + 258), fSmall_.Get(), pal_.error);

    button(R(c.right - 24 - 96, c.bottom - 50, c.right - 24, c.bottom - 18), L"Crea", Act::ModalOk, 0, true);
    button(R(c.right - 24 - 96 - 10 - 96, c.bottom - 50, c.right - 24 - 106, c.bottom - 18), L"Annulla", Act::ModalCancel, 0, false);
}

// ---------------------------------------------------------------- input

const Hit* MainWindow::hitAt(float x, float y) const {
    // le ultime aggiunte stanno sopra: si cerca a ritroso
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
        if (inside(it->r, x, y)) return &*it;
    return nullptr;
}

void MainWindow::scrollBy(ScrollId id, float delta) {
    ScrollArea& s = scroll_[id];
    s.pos = clampf(s.pos + delta, 0, std::max(0.f, s.content - s.view));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::onClick(const Hit& h, float x, float y) {
    (void)x;
    POINT pt;
    GetCursorPos(&pt);
    switch (h.act) {
        case Act::Chat:
            eng_.selectChat(h.param);
            scroll_[SC_LIST].pos = 0;
            sel_.clear();
            SetWindowTextW(edFiles_, L"");
            break;
        case Act::ChatMenu: onChatContext(h.param, pt); break;
        case Act::NewChat: showNewChatMenu(pt); break;
        case Act::Filter:
            eng_.setFilter((Filter)h.param);
            scroll_[SC_LIST].pos = 0;
            break;
        case Act::RowCheck:
        case Act::Row:
            if (sel_.count(h.key)) sel_.erase(h.key);
            else sel_.insert(h.key);
            break;
        case Act::CheckAll:
            if (!entriesSnap_.empty() && sel_.size() == entriesSnap_.size()) sel_.clear();
            else for (auto& e : entriesSnap_) sel_.insert(e.key);
            break;
        case Act::RowAction: eng_.download(h.key); break;
        case Act::DownloadSel:
            for (auto& e : entriesSnap_)
                if (sel_.count(e.key) && !eng_.alreadyDownloaded(e)) eng_.download(e.key);
            sel_.clear();
            break;
        case Act::SortCol: {
            int col = (int)h.param;
            if (sortCol_ == col) sortDesc_ = !sortDesc_;
            else { sortCol_ = col; sortDesc_ = col != 1; }  // nome: A→Z; dimensione e data: dal più grande/recente
            Settings st = eng_.settings();
            st.sortColumn = sortCol_;
            st.sortDescending = sortDesc_;
            eng_.setSettings(st);
            scroll_[SC_LIST].pos = 0;
            break;
        }
        case Act::DeleteSel: deleteSelected(std::vector<std::string>(sel_.begin(), sel_.end())); break;
        case Act::Upload: showUploadMenu(pt); break;
        case Act::TTab:
            tTab_ = (int)h.param;
            scroll_[SC_TRANSFERS].pos = 0;
            if (transCollapsed_) { transCollapsed_ = false; saveTransfersLayout(); }
            break;
        case Act::TCollapse:
            transCollapsed_ = !transCollapsed_;
            saveTransfersLayout();
            break;
        case Act::Splitter: break;  // gestito in WM_LBUTTONDOWN (trascinamento)
        case Act::TPause: eng_.pause((int)h.param); break;
        case Act::TResume: eng_.resume((int)h.param); break;
        case Act::TCancel: {
            bool done = false;
            for (auto& t : eng_.transfers()) if (t.id == h.param) done = t.status == TStatus::Done;
            if (!done && !confirm(L"Annullare il trasferimento?", L"La parte già trasferita andrà persa.",
                                  L"Annulla trasferimento", true))
                break;
            eng_.cancel((int)h.param);
            break;
        }
        case Act::TFolder:
            for (auto& t : eng_.transfers())
                if (t.id == h.param) {
                    std::wstring args = L"/select,\"" + U(t.path) + L"\"";
                    ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                }
            break;
        case Act::PauseAll: eng_.pauseAll(); break;
        case Act::ResumeAll: eng_.resumeAll(); break;
        case Act::CancelAll: {
            int n = 0;
            for (auto& t : eng_.transfers()) if (t.status != TStatus::Done) ++n;
            if (n && confirm(n == 1 ? std::wstring(L"Annullare il trasferimento?")
                                    : L"Annullare tutti i " + std::to_wstring(n) + L" trasferimenti?",
                             L"Download e upload in corso, in pausa e in coda verranno annullati; la parte gi\u00E0 "
                             L"trasferita andr\u00E0 persa. I file completati restano.",
                             L"Annulla tutti", true))
                eng_.cancelAll();
            break;
        }
        case Act::ClearDone: eng_.clearCompleted(); break;
        case Act::Upgrade:
            // l'abbonamento si acquista nell'app ufficiale o tramite il bot ufficiale di Telegram
            ShellExecuteW(hwnd_, L"open", L"https://t.me/PremiumBot", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case Act::Settings: showSettingsMenu(pt); break;
        case Act::DestFolder: pickDownloadDir(); break;
        case Act::LoginTab:
            loginTab_ = (int)h.param;
            if (loginTab_ == 1 && eng_.auth().state == AuthState::WaitQr) eng_.usePhoneLogin();
            if (loginTab_ == 0) qrRequested_ = false;
            layoutEdits();
            break;
        case Act::LoginSubmit: submitLogin(); break;
        case Act::LoginRetry:
            // dal codice si torna al numero: TDLib richiede di ricominciare con logOut
            eng_.logout();
            loginTab_ = 1;
            break;
        case Act::ModalOk: submitModal(); break;
        case Act::ModalCancel:
        case Act::ModalBg: closeModal(); break;
        case Act::ModalKind: modalGroup_ = h.param == 1; break;
        case Act::ScrollTrack: {
            // clic sulla traccia: pagina su/giù verso il punto cliccato
            ScrollArea& s = scroll_[h.param];
            scrollBy((ScrollId)h.param, y < s.thumb.top ? -s.view * 0.9f : s.view * 0.9f);
            break;
        }
        default: break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::confirm(const std::wstring& title, const std::wstring& body, const std::wstring& okLabel, bool danger) {
#ifdef TF_SIM_BUILD
    if (_wgetenv(L"TF_AUTOCONFIRM")) return true;  // solo per le prove automatiche
#endif
    TASKDIALOG_BUTTON buttons[] = {{1001, okLabel.c_str()}, {IDCANCEL, L"Annulla"}};
    TASKDIALOGCONFIG cfg{sizeof cfg};
    cfg.hwndParent = hwnd_;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    cfg.pszWindowTitle = L"TeleFiles";
    cfg.pszMainIcon = danger ? TD_WARNING_ICON : TD_INFORMATION_ICON;
    cfg.pszMainInstruction = title.c_str();
    cfg.pszContent = body.c_str();
    cfg.cButtons = 2;
    cfg.pButtons = buttons;
    cfg.nDefaultButton = IDCANCEL;  // per le azioni distruttive il predefinito è "Annulla"
    int pressed = 0;
    bool ok;
    if (SUCCEEDED(TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr))) ok = pressed == 1001;
    else  // ripiego (TaskDialog non disponibile)
        ok = MessageBoxW(hwnd_, (title + L"\n\n" + body).c_str(), L"TeleFiles",
                         MB_YESNO | MB_DEFBUTTON2 | (danger ? MB_ICONWARNING : MB_ICONQUESTION)) == IDYES;
    dropStrayClicks();
    return ok;
}

// Dopo una finestra di dialogo o un menu: scarta i clic rimasti in coda, così il clic sul pulsante
// del dialogo non "passa" alla riga che sta sotto, e ridisegna subito.
void MainWindow::dropStrayClicks() {
    MSG m;
    while (PeekMessageW(&m, hwnd_, WM_LBUTTONDOWN, WM_MBUTTONDBLCLK, PM_REMOVE)) {}
    ignoreClicksUntil_ = GetTickCount64() + 200;
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void MainWindow::deleteSelected(std::vector<std::string> keys) {
    if (keys.empty()) return;
    std::wstring chat;
    for (auto& c : eng_.chats()) if (c.id == eng_.selectedChat()) chat = U(c.title);
    int parts = 0;
    std::wstring one;
    for (auto& e : entriesSnap_)
        if (std::find(keys.begin(), keys.end(), e.key) != keys.end()) {
            if (e.isGroup) parts += e.partCount;
            one = U(e.name);
        }
    std::wstring title = keys.size() == 1 ? L"Eliminare «" + one + L"»?"
                                          : L"Eliminare " + std::to_wstring(keys.size()) + L" file?";
    std::wstring body = L"Verranno eliminati da Telegram («" + chat + L"») per tutti e non si possono recuperare.";
    if (parts) body += L" Per i file divisi vengono eliminate tutte le " + std::to_wstring(parts) + L" parti.";
    body += L"\nLe copie già scaricate sul PC restano.";
    if (!confirm(title, body, L"Elimina", true)) return;
    eng_.deleteEntries(keys);
    for (auto& k : keys) sel_.erase(k);
}

void MainWindow::submitLogin() {
    AuthInfo a = eng_.auth();
    if (a.busy) return;
    if (a.state == AuthState::WaitPassword) {
        std::wstring p = editText(edPwd_);
        if (!p.empty()) eng_.submitPassword(wideToUtf8(p));
    } else if (a.state == AuthState::WaitCode) {
        std::wstring c = editText(edLogin_);
        std::wstring digits;
        for (wchar_t ch : c) if (iswdigit(ch)) digits += ch;
        if (!digits.empty()) eng_.submitCode(wideToUtf8(digits));
    } else if (a.state == AuthState::WaitPhone || a.state == AuthState::WaitQr) {
        std::wstring p = editText(edLogin_);
        std::wstring clean;
        for (wchar_t ch : p) if (iswdigit(ch) || (ch == L'+' && clean.empty())) clean += ch;
        if (clean.size() >= 6) eng_.submitPhone(wideToUtf8(clean));
    }
}

void MainWindow::openModal(bool group) {
    modal_ = 1;
    modalGroup_ = group;
    modalError_.clear();
    SetWindowTextW(edModalTitle_, L"");
    SetWindowTextW(edModalDesc_, L"");
    layoutEdits();
    SetFocus(edModalTitle_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::closeModal() {
    modal_ = 0;
    layoutEdits();
    SetFocus(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::submitModal() {
    std::wstring t = editText(edModalTitle_), d = editText(edModalDesc_);
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
    if (t.empty()) {
        modalError_ = L"Scrivi un nome.";
        SetFocus(edModalTitle_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    eng_.createChannel(wideToUtf8(t), wideToUtf8(d), modalGroup_);
    closeModal();
}

void MainWindow::onRowContext(const std::string& key, POINT pt) {
    const FileEntry* e = nullptr;
    for (auto& x : entriesSnap_) if (x.key == key) e = &x;
    if (!e) return;
    // il menu agisce sulla selezione se la riga ne fa parte, altrimenti solo sulla riga
    std::vector<std::string> keys;
    if (sel_.count(key) && sel_.size() > 1) keys.assign(sel_.begin(), sel_.end());
    else keys.push_back(key);
    HMENU m = CreatePopupMenu();
    bool have = eng_.alreadyDownloaded(*e);
    if (keys.size() > 1) {
        AppendMenuW(m, MF_STRING, 1, (L"Scarica " + std::to_wstring(keys.size()) + L" file").c_str());
    } else {
        AppendMenuW(m, MF_STRING, 1, have ? L"Scarica di nuovo" : L"Scarica");
        if (have) AppendMenuW(m, MF_STRING, 2, L"Mostra nella cartella");
        AppendMenuW(m, MF_STRING, 3, L"Copia nome file");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 4, keys.size() > 1 ? (L"Elimina " + std::to_wstring(keys.size()) + L" file da Telegram…").c_str()
                                                 : L"Elimina da Telegram…");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    dropStrayClicks();
    if (cmd == 1) for (auto& k : keys) eng_.download(k);
    else if (cmd == 2) {
        std::string path = joinPath(eng_.settings().downloadDir, e->folder.empty() ? e->name : e->folder + "/" + e->name);
        std::wstring wp = U(path);
        std::replace(wp.begin(), wp.end(), L'/', L'\\');
        std::wstring args = L"/select,\"" + wp + L"\"";
        ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else if (cmd == 3 && OpenClipboard(hwnd_)) {
        std::wstring n = U(e->name);
        EmptyClipboard();
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (n.size() + 1) * sizeof(wchar_t));
        if (g) {
            memcpy(GlobalLock(g), n.c_str(), (n.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(g);
            SetClipboardData(CF_UNICODETEXT, g);
        }
        CloseClipboard();
    } else if (cmd == 4) {
        deleteSelected(keys);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::onChatContext(int64_t chatId, POINT pt) {
    Chat c{};
    for (auto& x : eng_.chats()) if (x.id == chatId) c = x;
    if (!c.id) return;
    HMENU m = CreatePopupMenu();
    bool channel = c.kind == ChatKind::Channel;
    bool menuable = channel || c.kind == ChatKind::Group;
    AppendMenuW(m, MF_STRING, 1, L"Apri");
    if (menuable) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        // il proprietario può solo eliminare per tutti; gli altri possono uscire
        if (c.owner) AppendMenuW(m, MF_STRING, 2, channel ? L"Elimina canale per tutti\u2026" : L"Elimina gruppo per tutti\u2026");
        else AppendMenuW(m, MF_STRING, 3, channel ? L"Esci dal canale\u2026" : L"Esci dal gruppo\u2026");
    }
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    dropStrayClicks();
    std::wstring t = U(c.title);
    if (cmd == 1) {
        eng_.selectChat(chatId);
        scroll_[SC_LIST].pos = 0;
        sel_.clear();
    } else if (cmd == 2) {
        if (confirm(std::wstring(channel ? L"Eliminare il canale" : L"Eliminare il gruppo") + L" «" + t + L"» per tutti?",
                    L"Tutti i messaggi e i file verranno eliminati per tutti gli iscritti. L'operazione non si può annullare.",
                    L"Elimina per tutti", true))
            eng_.deleteChat(chatId);
    } else if (cmd == 3) {
        if (confirm(std::wstring(channel ? L"Uscire dal canale" : L"Uscire dal gruppo") + L" \u00AB" + t + L"\u00BB?",
                    L"Potrai rientrare solo se \u00E8 pubblico o con un nuovo invito.", L"Esci", true))
            eng_.deleteChat(chatId);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::onTransferContext(int id, POINT pt) {
    TransferView t{};
    bool found = false;
    for (auto& x : eng_.transfers()) if (x.id == id) { t = x; found = true; }
    if (!found) return;
    HMENU m = CreatePopupMenu();
    bool down = t.dir == Direction::Download;
    if (t.status == TStatus::Done && down) {
        AppendMenuW(m, MF_STRING, 1, L"Apri file");
        AppendMenuW(m, MF_STRING, 2, L"Mostra nella cartella");
    }
    if (t.status == TStatus::Paused || t.status == TStatus::Error) AppendMenuW(m, MF_STRING, 3, t.status == TStatus::Error ? L"Riprova" : L"Riprendi");
    else if (t.status != TStatus::Done) AppendMenuW(m, MF_STRING, 4, L"Pausa");
    AppendMenuW(m, MF_STRING, 5, t.status == TStatus::Done ? L"Rimuovi dall'elenco" : L"Annulla");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    dropStrayClicks();
    if (cmd == 1) ShellExecuteW(hwnd_, L"open", U(t.path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    else if (cmd == 2) {
        std::wstring args = L"/select,\"" + U(t.path) + L"\"";
        ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else if (cmd == 3) eng_.resume(id);
    else if (cmd == 4) eng_.pause(id);
    else if (cmd == 5) eng_.cancel(id);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::showSettingsMenu(POINT pt) {
    Settings s = eng_.settings();
    HMENU m = CreatePopupMenu(), par = CreatePopupMenu(), th = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Cartella di download…");
    for (int i = 1; i <= 5; ++i)
        AppendMenuW(par, MF_STRING | (s.maxParallel == i ? MF_CHECKED : 0), 10 + i, std::to_wstring(i).c_str());
    AppendMenuW(m, MF_POPUP, (UINT_PTR)par, L"Trasferimenti paralleli");
    AppendMenuW(m, MF_STRING | (s.keepParts ? MF_CHECKED : 0), 2, L"Conserva le parti dopo la ricomposizione");
    AppendMenuW(th, MF_STRING | (themeMode_ == 0 ? MF_CHECKED : 0), 20, L"Come il sistema");
    AppendMenuW(th, MF_STRING | (themeMode_ == 1 ? MF_CHECKED : 0), 21, L"Chiaro");
    AppendMenuW(th, MF_STRING | (themeMode_ == 2 ? MF_CHECKED : 0), 22, L"Scuro");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)th, L"Tema");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, L"Esci dall'account");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    dropStrayClicks();
    if (cmd == 1) pickDownloadDir();
    else if (cmd == 2) { s.keepParts = !s.keepParts; eng_.setSettings(s); }
    else if (cmd >= 11 && cmd <= 15) { s.maxParallel = cmd - 10; eng_.setSettings(s); }
    else if (cmd >= 20 && cmd <= 22) {
        themeMode_ = cmd - 20;
        s.theme = themeMode_;
        eng_.setSettings(s);
        applyTheme();
    } else if (cmd == 3) {
        bool busy = false;
        for (auto& t : eng_.transfers()) if (t.status != TStatus::Done) busy = true;
        if (confirm(L"Uscire dall'account?",
                    busy ? L"I trasferimenti in corso verranno annullati." : L"Per rientrare dovrai accedere di nuovo con il QR o il numero.",
                    L"Esci", true)) {
            loginTab_ = 0;
            eng_.logout();
        }
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::showUploadMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Carica file…");
    AppendMenuW(m, MF_STRING, 2, L"Carica cartella…");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_RIGHTALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    dropStrayClicks();
    if (cmd == 1) pickUploadFiles();
    else if (cmd == 2) pickUploadFolder();
}

void MainWindow::showNewChatMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Nuovo canale…");
    AppendMenuW(m, MF_STRING, 2, L"Nuovo gruppo…");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    dropStrayClicks();
    if (cmd == 1 || cmd == 2) openModal(cmd == 2);
}

void MainWindow::pickUploadFiles() {
    int64_t chat = eng_.selectedChat();
    if (!chat) return;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.GetAddressOf()))))
        return;
    FILEOPENDIALOGOPTIONS o;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dlg->SetTitle(L"Scegli i file da caricare");
    if (FAILED(dlg->Show(hwnd_))) return;
    ComPtr<IShellItemArray> items;
    if (FAILED(dlg->GetResults(items.GetAddressOf()))) return;
    DWORD n = 0;
    items->GetCount(&n);
    for (DWORD i = 0; i < n; ++i) {
        ComPtr<IShellItem> it;
        PWSTR p = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, it.GetAddressOf())) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            eng_.upload(chat, wideToUtf8(p));
            CoTaskMemFree(p);
        }
    }
    tTab_ = 0;
    if (transCollapsed_) { transCollapsed_ = false; saveTransfersLayout(); }
}

void MainWindow::pickUploadFolder() {
    int64_t chat = eng_.selectedChat();
    if (!chat) return;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.GetAddressOf()))))
        return;
    FILEOPENDIALOGOPTIONS o;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT);
    dlg->SetTitle(L"Scegli le cartelle da caricare (con tutte le sottocartelle)");
    if (FAILED(dlg->Show(hwnd_))) return;
    ComPtr<IShellItemArray> items;
    if (FAILED(dlg->GetResults(items.GetAddressOf()))) return;
    DWORD n = 0;
    items->GetCount(&n);
    for (DWORD i = 0; i < n; ++i) {
        ComPtr<IShellItem> it;
        PWSTR p = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, it.GetAddressOf())) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            eng_.uploadFolder(chat, wideToUtf8(p));
            CoTaskMemFree(p);
        }
    }
    tTab_ = 0;
    if (transCollapsed_) { transCollapsed_ = false; saveTransfersLayout(); }
}

void MainWindow::pickDownloadDir() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.GetAddressOf()))))
        return;
    FILEOPENDIALOGOPTIONS o;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Cartella in cui salvare i download");
    if (FAILED(dlg->Show(hwnd_))) return;
    ComPtr<IShellItem> it;
    PWSTR p = nullptr;
    if (SUCCEEDED(dlg->GetResult(it.GetAddressOf())) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
        Settings s = eng_.settings();
        s.downloadDir = wideToUtf8(p);
        eng_.setSettings(s);
        CoTaskMemFree(p);
    }
}

std::wstring MainWindow::editText(HWND e) {
    int n = GetWindowTextLengthW(e);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(e, s.data(), n + 1);
    s.resize(n);
    return s;
}

void MainWindow::layoutEdits() {
    if (!hwnd_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    float W = (float)rc.right / scale_, H = (float)rc.bottom / scale_;
    AuthInfo a = eng_.auth();
    bool main = configError_.empty() && a.state == AuthState::Ready;
    auto place = [&](HWND e, bool show, D2D1_RECT_F r) {
        if (!show) { ShowWindow(e, SW_HIDE); return; }
        // centra verticalmente il testo dentro la casella disegnata
        int h = (int)std::lround(18 * scale_);
        int x = (int)std::lround(r.left * scale_), w = (int)std::lround((r.right - r.left) * scale_);
        int y = (int)std::lround((r.top + r.bottom) / 2 * scale_) - h / 2;
        SetWindowPos(e, nullptr, x, y, std::max(w, 10), h, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(e, SW_SHOW);
    };
    D2D1_RECT_F cs = chatSearchRect(), fs = fileSearchRect(W);
    // i controlli nativi stanno sopra al disegno: con la finestra modale aperta vanno nascosti
    place(edChats_, main && !modal_, R(cs.left + 28, cs.top, cs.right - 8, cs.bottom));
    place(edFiles_, main && !modal_, R(fs.left + 28, fs.top, fs.right - 8, fs.bottom));
    D2D1_RECT_F mc = modalRect(W, H);
    place(edModalTitle_, main && modal_, R(mc.left + 34, mc.top + 144, mc.right - 34, mc.top + 176));
    place(edModalDesc_, main && modal_, R(mc.left + 34, mc.top + 204, mc.right - 34, mc.top + 236));

    float cw = 420, ch = 500;
    float x = (W - cw) / 2, y = kTitleH + std::max(16.f, (H - kTitleH - ch) / 2);
    bool phoneFlow = a.state == AuthState::WaitCode || a.state == AuthState::WaitPassword;
    float cy = y + 140 + (phoneFlow ? 0 : 48);
    D2D1_RECT_F in = R(x + 36 + 10, cy + 26, x + cw - 36 - 10, cy + 60);
    bool loginVis = !main && configError_.empty() &&
                    ((loginTab_ == 1 && (a.state == AuthState::WaitPhone || a.state == AuthState::WaitQr)) ||
                     a.state == AuthState::WaitCode);
    bool pwdVis = !main && configError_.empty() && a.state == AuthState::WaitPassword;
    static AuthState lastShown = AuthState::Starting;
    if (a.state != lastShown) {
        lastShown = a.state;
        if (a.state == AuthState::WaitCode) {
            SetWindowTextW(edLogin_, L"");
            SendMessageW(edLogin_, EM_SETCUEBANNER, FALSE, (LPARAM)L"Codice di 5 cifre");
        } else if (a.state == AuthState::WaitPhone) {
            SendMessageW(edLogin_, EM_SETCUEBANNER, FALSE, (LPARAM)L"+39 333 123 4567");
        }
    }
    place(edLogin_, loginVis, in);
    place(edPwd_, pwdVis, in);
    if (loginVis && GetFocus() != edLogin_) SetFocus(edLogin_);
    if (pwdVis && GetFocus() != edPwd_) SetFocus(edPwd_);
}

LRESULT MainWindow::hitTestNC(int sx, int sy) {
    POINT p{sx, sy};
    ScreenToClient(hwnd_, &p);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int border = (int)(6 * scale_);
    bool max = IsZoomed(hwnd_);
    if (!max) {
        bool l = p.x < border, r = p.x >= rc.right - border, t = p.y < border, b = p.y >= rc.bottom - border;
        if (t && l) return HTTOPLEFT;
        if (t && r) return HTTOPRIGHT;
        if (b && l) return HTBOTTOMLEFT;
        if (b && r) return HTBOTTOMRIGHT;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (t) return HTTOP;
        if (b) return HTBOTTOM;
    }
    float x = (float)p.x / scale_, y = (float)p.y / scale_, W = (float)rc.right / scale_;
    if (y < kTitleH) {
        if (x >= W - 46) return HTCLOSE;
        if (x >= W - 92) return HTMAXBUTTON;  // abilita i layout di snap di Windows 11
        if (x >= W - 138) return HTMINBUTTON;
        return HTCAPTION;
    }
    return HTCLIENT;
}

void MainWindow::captionAction(Act a) {
    if (a == Act::Min) ShowWindow(hwnd_, SW_MINIMIZE);
    else if (a == Act::Max) ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
    else if (a == Act::Close) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

static Act ncToAct(WPARAM ht) {
    return ht == HTCLOSE ? Act::Close : ht == HTMAXBUTTON ? Act::Max : ht == HTMINBUTTON ? Act::Min : Act::None;
}

LRESULT CALLBACK MainWindow::EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<MainWindow*>(ref);
    // la rotella va sempre alla finestra principale, anche se il cursore è in una casella di testo
    if (msg == WM_MOUSEWHEEL) return SendMessageW(self->hwnd_, msg, wp, lp);
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (h == self->edLogin_ || h == self->edPwd_) self->submitLogin();
        if (h == self->edFiles_) {
            KillTimer(self->hwnd_, TIMER_SEARCH);
            self->eng_.setQuery(wideToUtf8(self->editText(h)));
        }
        if (h == self->edModalTitle_ || h == self->edModalDesc_) self->submitModal();
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        if (h == self->edModalTitle_ || h == self->edModalDesc_) { self->closeModal(); return 0; }
        if (h == self->edFiles_ || h == self->edChats_) { SetWindowTextW(h, L""); return 0; }
    }
    if (msg == WM_KEYDOWN && wp == VK_TAB && (h == self->edModalTitle_ || h == self->edModalDesc_)) {
        SetFocus(h == self->edModalTitle_ ? self->edModalDesc_ : self->edModalTitle_);
        return 0;
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB)) return 0;  // niente "bip"
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) InvalidateRect(self->hwnd_, nullptr, FALSE);
    return DefSubclassProc(h, msg, wp, lp);
}

LRESULT CALLBACK MainWindow::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self;
    if (msg == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT MainWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp) {
                // l'area client occupa tutta la finestra: la barra del titolo la disegniamo noi
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                if (IsZoomed(hwnd_)) {
                    int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi_) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
                    int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi_) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
                    p->rgrc[0].left += fx;
                    p->rgrc[0].right -= fx;
                    p->rgrc[0].top += fy;
                    p->rgrc[0].bottom -= fy;
                }
                return 0;
            }
            break;
        case WM_NCHITTEST: {
            LRESULT r = hitTestNC(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            Act a = ncToAct(r);
            if (a != hoverCaption_) { hoverCaption_ = a; InvalidateRect(hwnd_, nullptr, FALSE); }
            return r;
        }
        case WM_NCMOUSEMOVE: {
            Act a = ncToAct(wp);
            if (a != hoverCaption_) { hoverCaption_ = a; InvalidateRect(hwnd_, nullptr, FALSE); }
            if (!trackingNc_) {
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE | TME_NONCLIENT, hwnd_, 0};
                TrackMouseEvent(&t);
                trackingNc_ = true;
            }
            if (a != Act::None) return 0;
            break;
        }
        case WM_NCMOUSELEAVE:
            trackingNc_ = false;
            hoverCaption_ = pressedCaption_ = Act::None;
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        case WM_NCLBUTTONDOWN: {
            Act a = ncToAct(wp);
            if (a != Act::None) { pressedCaption_ = a; InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
            break;
        }
        case WM_NCLBUTTONUP: {
            Act a = ncToAct(wp);
            if (a != Act::None) {
                if (a == pressedCaption_) captionAction(a);
                pressedCaption_ = Act::None;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        }
        case WM_NCLBUTTONDBLCLK:
            if (ncToAct(wp) != Act::None) return 0;
            break;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {(LONG)(900 * scale_), (LONG)(560 * scale_)};
            return 0;
        }
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) return 0;
            if (rt_ && FAILED(rt_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp))))) {
                rt_.Reset();
                brush_.Reset();
            }
            layoutEdits();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wp);
            scale_ = (float)dpi_ / 96.f;
            rt_.Reset();
            brush_.Reset();
            createFormats();
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layoutEdits();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0 && themeMode_ == 0) applyTheme();
            break;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursor(nullptr, cursor_));
                return TRUE;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd_, &ps);
            lastPaint_ = GetTickCount64();
            paint();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_APP_REFRESH:
            refreshPosted_ = false;
            // WM_TIMER ha priorità bassa e con molti aggiornamenti può arrivare in ritardo
            if (GetTickCount64() - lastTick_ >= 1000) { lastTick_ = GetTickCount64(); eng_.tick(); }
            InvalidateRect(hwnd_, nullptr, FALSE);
            // anche WM_PAINT ha priorità bassa: ridisegna subito, al massimo ~30 volte al secondo
            if (GetTickCount64() - lastPaint_ >= 33) UpdateWindow(hwnd_);
            return 0;
        case WM_TIMER:
            if (wp == TIMER_TICK && GetTickCount64() - lastTick_ >= 900) { lastTick_ = GetTickCount64(); eng_.tick(); }
            if (wp == TIMER_SEARCH) {
                KillTimer(hwnd_, TIMER_SEARCH);
                eng_.setQuery(wideToUtf8(editText(edFiles_)));
                scroll_[SC_LIST].pos = 0;
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_COMMAND:
            if (HIWORD(wp) == EN_CHANGE) {
                if ((HWND)lp == edFiles_) SetTimer(hwnd_, TIMER_SEARCH, 350, nullptr);
                if ((HWND)lp == edChats_) { chatFilter_ = editText(edChats_); scroll_[SC_CHATS].pos = 0; }
                if ((HWND)lp == edModalTitle_) modalError_.clear();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_CTLCOLOREDIT:
            SetTextColor((HDC)wp, pal_.editText);
            SetBkColor((HDC)wp, pal_.editBg);
            return (LRESULT)editBrush_;
        case WM_MOUSEMOVE: {
            float x = GET_X_LPARAM(lp) / scale_, y = GET_Y_LPARAM(lp) / scale_;
            if (dragScroll_ >= 0) {
                // trascinamento del pollice della barra di scorrimento
                ScrollArea& s = scroll_[dragScroll_];
                float trackH = s.track.bottom - s.track.top, thumbH = s.thumb.bottom - s.thumb.top;
                float maxPos = s.content - s.view;
                if (trackH > thumbH && maxPos > 0) {
                    float top = clampf(y - dragGrab_, s.track.top, s.track.bottom - thumbH);
                    s.pos = (top - s.track.top) / (trackH - thumbH) * maxPos;
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (dragSplit_) {
                RECT rc;
                GetClientRect(hwnd_, &rc);
                transH_ = (float)rc.bottom / scale_ - (y - splitGrab_);
                Layout l = computeLayout((float)rc.right / scale_, (float)rc.bottom / scale_);
                transH_ = l.transH;  // applica i limiti
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            const Hit* hh = hitAt(x, y);
            Act a = hh ? hh->act : Act::None;
            int64_t p = hh ? hh->param : 0;
            std::string k = hh ? hh->key : "";
            if (a != hoverAct_ || p != hoverParam_ || k != hoverKey_ || hoverCaption_ != Act::None) {
                hoverAct_ = a; hoverParam_ = p; hoverKey_ = k;
                hoverCaption_ = Act::None;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            if (!tracking_) {
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&t);
                tracking_ = true;
            }
            cursor_ = a == Act::Splitter ? IDC_SIZENS
                    : (a == Act::None || a == Act::Row || a == Act::Block || a == Act::ModalBg ||
                       a == Act::ScrollTrack || a == Act::ScrollThumb) ? IDC_ARROW : IDC_HAND;
            SetCursor(LoadCursor(nullptr, cursor_));
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            hoverAct_ = Act::None;
            hoverKey_.clear();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            if (GetTickCount64() < ignoreClicksUntil_) return 0;
            float x = GET_X_LPARAM(lp) / scale_, y = GET_Y_LPARAM(lp) / scale_;
            HWND f = GetFocus();
            const Hit* hh = hitAt(x, y);
            if (!modal_ && f != hwnd_) SetFocus(hwnd_);  // tastiera alla lista (PagGiù, Canc, ...)
            if (!hh) return 0;
            if (hh->act == Act::ScrollThumb) {
                dragScroll_ = (int)hh->param;
                dragGrab_ = y - scroll_[dragScroll_].thumb.top;
                SetCapture(hwnd_);
                return 0;
            }
            if (hh->act == Act::Splitter) {
                RECT rc;
                GetClientRect(hwnd_, &rc);
                dragSplit_ = true;
                splitGrab_ = y - ((float)rc.bottom / scale_ - computeLayout((float)rc.right / scale_, (float)rc.bottom / scale_).transH);
                SetCapture(hwnd_);
                return 0;
            }
            Hit copy = *hh;  // onClick può aprire menu e rigenerare hits_
            onClick(copy, x, y);
            return 0;
        }
        case WM_LBUTTONUP:
            if (dragScroll_ >= 0 || dragSplit_) {
                if (dragSplit_) saveTransfersLayout();
                dragScroll_ = -1;
                dragSplit_ = false;
                ReleaseCapture();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (dragSplit_) saveTransfersLayout();
            dragScroll_ = -1;
            dragSplit_ = false;
            return 0;
        case WM_LBUTTONDBLCLK: {
            float x = GET_X_LPARAM(lp) / scale_, y = GET_Y_LPARAM(lp) / scale_;
            const Hit* hh = hitAt(x, y);
            if (!hh) return 0;
            if (hh->act == Act::Row) {
                // doppio clic: scarica (annulla la selezione fatta dal primo clic)
                sel_.erase(hh->key);
                eng_.download(hh->key);
            } else if (hh->act == Act::Splitter) {
                transCollapsed_ = true;
                saveTransfersLayout();
            } else if (hh->act == Act::ScrollTrack || hh->act == Act::TPause || hh->act == Act::TResume ||
                       hh->act == Act::Filter || hh->act == Act::TTab || hh->act == Act::RowCheck) {
                Hit copy = *hh;  // il secondo clic di un doppio clic conta come clic
                onClick(copy, x, y);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_RBUTTONUP: {
            float x = GET_X_LPARAM(lp) / scale_, y = GET_Y_LPARAM(lp) / scale_;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd_, &pt);
            const Hit* hh = hitAt(x, y);
            if (!hh || modal_) return 0;
            Hit h = *hh;
            if (h.act == Act::Row || h.act == Act::RowCheck || h.act == Act::RowAction) onRowContext(h.key, pt);
            else if (h.act == Act::Chat || h.act == Act::ChatMenu) onChatContext(h.param, pt);
            else if (h.key == "t" || h.act == Act::TPause || h.act == Act::TResume || h.act == Act::TCancel ||
                     h.act == Act::TFolder)
                onTransferContext((int)h.param, pt);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (modal_) return 0;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            float x = (float)pt.x / scale_, y = (float)pt.y / scale_;
            // delta frazionari dei touchpad di precisione: 120 = una tacca = tre righe
            float d = -GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 3 * kRowH;
            int id = SC_LIST;
            if (inside(scroll_[SC_CHATS].area, x, y)) id = SC_CHATS;
            else if (inside(scroll_[SC_TRANSFERS].area, x, y)) id = SC_TRANSFERS;
            scrollBy((ScrollId)id, d);
            return 0;
        }
        case WM_KEYDOWN: {
            if (modal_) return 0;
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            ScrollArea& s = scroll_[SC_LIST];
            switch (wp) {
                case 'F': if (ctrl) SetFocus(edFiles_); return 0;
                case 'A':
                    if (ctrl) { for (auto& e : entriesSnap_) sel_.insert(e.key); InvalidateRect(hwnd_, nullptr, FALSE); }
                    return 0;
                case VK_NEXT: scrollBy(SC_LIST, s.view * 0.9f); return 0;
                case VK_PRIOR: scrollBy(SC_LIST, -s.view * 0.9f); return 0;
                case VK_DOWN: scrollBy(SC_LIST, kRowH); return 0;
                case VK_UP: scrollBy(SC_LIST, -kRowH); return 0;
                case VK_HOME: scrollBy(SC_LIST, -1e9f); return 0;
                case VK_END: scrollBy(SC_LIST, 1e9f); return 0;
                case VK_DELETE: deleteSelected(std::vector<std::string>(sel_.begin(), sel_.end())); return 0;
                case VK_ESCAPE: sel_.clear(); InvalidateRect(hwnd_, nullptr, FALSE); return 0;
            }
            break;
        }
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            int64_t chat = eng_.selectedChat();
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n && chat && !modal_; ++i) {
                wchar_t path[32768];
                DragQueryFileW(drop, i, path, 32768);
                DWORD attr = GetFileAttributesW(path);
                if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) eng_.uploadFolder(chat, wideToUtf8(path));
                else eng_.upload(chat, wideToUtf8(path));
            }
            DragFinish(drop);
            tTab_ = 0;
            if (transCollapsed_) { transCollapsed_ = false; saveTransfersLayout(); }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_CLOSE: {
            bool busy = false;
            for (auto& t : eng_.transfers())
                if (t.status != TStatus::Done && t.status != TStatus::Paused && t.status != TStatus::Error &&
                    t.status != TStatus::Queued)
                    busy = true;
            if (busy && !confirm(L"Chiudere TeleFiles?",
                                 L"Ci sono trasferimenti in corso: verranno messi in pausa e potrai riprenderli alla prossima apertura.",
                                 L"Chiudi", false))
                return 0;
            DestroyWindow(hwnd_);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd_, TIMER_TICK);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace tf
