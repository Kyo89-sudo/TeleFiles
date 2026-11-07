// TeleFiles - finestra principale (Win32 + Direct2D, disegnata a mano)
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../core/Engine.h"
#include "Icons.h"
#include "Theme.h"

namespace tf {

using Microsoft::WRL::ComPtr;

enum class Act {
    None, Min, Max, Close,
    Chat, ChatMenu, NewChat, Filter, Row, RowCheck, RowAction, CheckAll,
    Upload, DownloadSel, DeleteSel,
    TTab, TPause, TResume, TCancel, TFolder, PauseAll, ResumeAll, CancelAll, ClearDone, TCollapse, Splitter,
    Settings, Upgrade, DestFolder,
    LoginTab, LoginSubmit, LoginRetry,
    ScrollThumb, ScrollTrack, SortCol,
    ModalBg, ModalOk, ModalCancel, ModalKind,
    Block,
};

struct Hit {
    D2D1_RECT_F r;
    Act act;
    int64_t param;
    std::string key;
};

// Barra di scorrimento sottile, sovrapposta (stile macOS); una per area scorrevole.
enum ScrollId { SC_CHATS = 0, SC_LIST = 1, SC_TRANSFERS = 2, SC_COUNT = 3 };
struct ScrollArea {
    float pos = 0, content = 0, view = 0;
    D2D1_RECT_F area{}, track{}, thumb{};
};

struct Toast {
    std::wstring text;
    bool error = false;
    ULONGLONG until = 0;
};

class MainWindow {
public:
    MainWindow(Engine& engine, std::wstring configError, bool portable = false);
    bool create(HINSTANCE inst, int show);
    HWND hwnd() const { return hwnd_; }
    void requestRefresh();   // thread-safe

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    // risorse grafiche
    void createDevice();
    void createFormats();
    std::wstring pickFamily(std::initializer_list<const wchar_t*> names);
    void applyTheme();
    bool systemDark();
    ComPtr<ID2D1Bitmap> loadBitmap(const std::string& path);
    ID2D1Bitmap* appIcon(int px);

    // layout (in DIP), ricalcolato a ogni disegno
    struct Layout {
        float W = 0, H = 0;
        float transTop = 0, transH = 0;
        bool collapsed = false;
        bool narrow = false;     // finestra stretta: colonne ridotte
    };
    Layout computeLayout(float W, float H);

    // disegno
    void paint();
    void drawTitleBar(float W);
    void drawLogin(float W, float H);
    void drawSidebar(float W, float H);
    void drawContent(const Layout& lay);
    void drawFileList(D2D1_RECT_F area, bool narrow);
    void sortEntries();
    void drawTransfers(D2D1_RECT_F area, bool collapsed);
    void drawQr(const std::string& link, D2D1_RECT_F r);
    void drawScrollbar(ScrollId id);
    void drawModal(float W, float H);
    void drawToasts(float W, float bottom);
    void docIcon(D2D1_RECT_F r, const FileEntry& e);

    // primitive
    void fill(D2D1_RECT_F r, D2D1_COLOR_F c, float radius = 0);
    void stroke(D2D1_RECT_F r, D2D1_COLOR_F c, float radius = 0, float w = 1);
    void line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float w = 1);
    void text(const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* f, D2D1_COLOR_F c,
              DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT va = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    void icon(Ic ic, D2D1_RECT_F r, D2D1_COLOR_F c, float px = 16, bool duo = false, float weight = 1.7f);
    float measure(const std::wstring& s, IDWriteTextFormat* f);
    void progressBar(D2D1_RECT_F r, double frac, D2D1_COLOR_F fg);
    bool button(D2D1_RECT_F r, const std::wstring& label, Act a, int64_t p, bool primary, Ic ic = Ic::Doc,
                bool withIcon = false, bool danger = false);
    bool iconButton(D2D1_RECT_F r, Ic ic, Act a, int64_t p, const std::string& key = {},
                    const D2D1_COLOR_F* tint = nullptr, bool filled = false, float px = 15, float weight = 1.7f);
    void avatar(D2D1_RECT_F r, const std::wstring& title, int64_t seed, ID2D1Bitmap* bmp = nullptr);
    bool hovered(Act a, int64_t p, const std::string& key = {}) const;
    void addHit(D2D1_RECT_F r, Act a, int64_t p = 0, const std::string& key = {});

    // input
    const Hit* hitAt(float x, float y) const;
    void onClick(const Hit& h, float x, float y);
    void onRowContext(const std::string& key, POINT screen);
    void onChatContext(int64_t chatId, POINT screen);
    void onTransferContext(int id, POINT screen);
    void showSettingsMenu(POINT screen);
    void showUploadMenu(POINT screen);
    void showNewChatMenu(POINT screen);
    void pickUploadFiles();
    void pickUploadFolder();
    void pickDownloadDir();
    void deleteSelected(std::vector<std::string> keys);
    void openModal(bool group);
    void closeModal();
    void submitModal();
    void dropStrayClicks();
    bool confirm(const std::wstring& title, const std::wstring& text, const std::wstring& okLabel, bool danger);
    void scrollBy(ScrollId id, float delta);
    void layoutEdits();
    void submitLogin();
    std::wstring editText(HWND e);
    LRESULT hitTestNC(int x, int y);
    void captionAction(Act a);
    void saveTransfersLayout();

    Engine& eng_;
    std::wstring configError_;
    bool portable_ = false;
    HWND hwnd_ = nullptr;
    HWND edChats_ = nullptr, edFiles_ = nullptr, edLogin_ = nullptr, edPwd_ = nullptr;
    HWND edModalTitle_ = nullptr, edModalDesc_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH editBrush_ = nullptr;
    LPCWSTR cursor_ = IDC_ARROW;
    float scale_ = 1.f;
    UINT dpi_ = 96;

    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dw_;
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<ID2D1HwndRenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> fBody_, fSemi_, fSmall_, fSmallSemi_, fTitle_, fHeader_, fBadge_, fWrap_, fWrapSm_, fDoc_;
    std::map<std::string, ComPtr<ID2D1Bitmap>> bitmaps_;
    std::map<int, ComPtr<ID2D1Bitmap>> appIcons_;
    Icons icons_;

    Palette pal_;
    int themeMode_ = 0;  // 0 sistema, 1 chiaro, 2 scuro

    // stato interfaccia
    std::vector<Hit> hits_;
    Act hoverAct_ = Act::None;
    int64_t hoverParam_ = 0;
    std::string hoverKey_;
    Act pressedCaption_ = Act::None;
    Act hoverCaption_ = Act::None;
    bool trackingNc_ = false, tracking_ = false;
    ScrollArea scroll_[SC_COUNT];
    int dragScroll_ = -1;           // barra trascinata
    float dragGrab_ = 0;            // posizione del mouse nel pollice
    bool dragSplit_ = false;        // ridimensionamento del pannello trasferimenti
    float splitGrab_ = 0;
    float transH_ = 210;
    bool transCollapsed_ = false;
    std::set<std::string> sel_;
    std::wstring chatFilter_;
    int tTab_ = 0;          // 0 in corso, 1 in coda, 2 completati
    int sortCol_ = 0;       // 0 data, 1 nome, 2 dimensione
    bool sortDesc_ = true;
    int loginTab_ = 0;      // 0 QR, 1 numero
    bool qrRequested_ = false;
    AuthState lastAuth_ = AuthState::Starting;
    std::atomic<bool> refreshPosted_{false};
    ULONGLONG lastTick_ = 0, lastPaint_ = 0, ignoreClicksUntil_ = 0;
    std::vector<FileEntry> entriesSnap_;
    std::vector<Toast> toasts_;
    int modal_ = 0;         // 0 nessuna, 1 nuovo canale/gruppo
    bool modalGroup_ = false;
    std::wstring modalError_;
};

} // namespace tf
