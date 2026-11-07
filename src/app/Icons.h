// TeleFiles - icone vettoriali disegnate con Direct2D (stile linea arrotondata, come macOS)
//
// Ogni icona è descritta da un percorso in sintassi SVG su una griglia 24x24 (disegni originali
// di TeleFiles). Le geometrie vengono create una volta e riusate a qualsiasi dimensione/DPI.
#pragma once
#include <d2d1.h>
#include <wrl/client.h>

#include <map>
#include <string>

namespace tf {

enum class Ic {
    Download, Upload, Pause, Play, Close, Search, Gear, Check, Folder, FolderPlus, Saved, Star,
    Alert, Retry, Plus, Trash, Channel, Group, Person, ChevronDown, ChevronUp, Logout, Doc,
    WinMin, WinMax, WinRestore, WinClose, Leave, More,
};

class Icons {
public:
    void init(ID2D1Factory* f) { factory_ = f; }
    // Disegna l'icona centrata in r (lato = min(larghezza, altezza)); 'weight' = spessore a 24 px.
    void draw(ID2D1RenderTarget* rt, Ic ic, D2D1_RECT_F r, D2D1_COLOR_F color, float weight = 1.7f,
              bool duotone = false);

private:
    struct Geo {
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> stroke, fill, solid;
    };
    const Geo& get(Ic ic);
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> parse(const char* svg);

    ID2D1Factory* factory_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round_;
    std::map<Ic, Geo> cache_;
};

} // namespace tf
