#include "Icons.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

namespace tf {

using Microsoft::WRL::ComPtr;

namespace {

// Definizione di un'icona: tratto, area di riempimento leggera (duotone), parti piene.
struct Def {
    const char* stroke;
    const char* fill;   // facoltativa: riempita con il colore al 18% in modalità duotone
    const char* solid;  // facoltativa: sempre piena (es. il triangolo di "play")
};

const Def& def(Ic ic) {
    static const Def none{"", nullptr, nullptr};
    static const std::map<Ic, Def> defs = {
        {Ic::Download, {"M12 4v11M7.5 10.5 12 15l4.5-4.5M5 19.5h14", nullptr, nullptr}},
        {Ic::Upload, {"M12 15.5V4.5M7.5 9 12 4.5 16.5 9M5 19.5h14", nullptr, nullptr}},
        {Ic::Pause, {"M8.5 6v12M15.5 6v12", nullptr, nullptr}},
        {Ic::Play, {"", nullptr, "M8 5.6c0-.8.9-1.3 1.6-.9l9.2 5.7c.7.4.7 1.4 0 1.8l-9.2 5.7c-.7.4-1.6-.1-1.6-.9z"}},
        {Ic::Close, {"M6.5 6.5l11 11M17.5 6.5l-11 11", nullptr, nullptr}},
        {Ic::Search, {"M10.5 4.5a6 6 0 1 0 0 12a6 6 0 1 0 0-12zM15 15l4.5 4.5", nullptr, nullptr}},
        {Ic::Gear, {"", nullptr, nullptr}},  // generata in codice
        {Ic::Check, {"M5 12.5l4.5 4.5L19 7.5", nullptr, nullptr}},
        {Ic::Folder, {"M3.5 8A2 2 0 0 1 5.5 6h3.6c.5 0 1 .2 1.4.6l1.3 1.4h6.7a2 2 0 0 1 2 2v7.5a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z",
                      "M3.5 8A2 2 0 0 1 5.5 6h3.6c.5 0 1 .2 1.4.6l1.3 1.4h6.7a2 2 0 0 1 2 2v7.5a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z",
                      nullptr}},
        {Ic::FolderPlus, {"M3.5 8A2 2 0 0 1 5.5 6h3.6c.5 0 1 .2 1.4.6l1.3 1.4h6.7a2 2 0 0 1 2 2v7.5a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2zM12 11v5M9.5 13.5h5",
                          "M3.5 8A2 2 0 0 1 5.5 6h3.6c.5 0 1 .2 1.4.6l1.3 1.4h6.7a2 2 0 0 1 2 2v7.5a2 2 0 0 1-2 2h-13a2 2 0 0 1-2-2z",
                          nullptr}},
        {Ic::Saved, {"M7.5 4.5h9a1.5 1.5 0 0 1 1.5 1.5v13.6c0 .4-.5.7-.8.4L12 16.2l-5.2 3.8c-.3.3-.8 0-.8-.4V6a1.5 1.5 0 0 1 1.5-1.5z",
                     "M7.5 4.5h9a1.5 1.5 0 0 1 1.5 1.5v13.6c0 .4-.5.7-.8.4L12 16.2l-5.2 3.8c-.3.3-.8 0-.8-.4V6a1.5 1.5 0 0 1 1.5-1.5z",
                     nullptr}},
        {Ic::Star, {"M12 4.3l2.3 4.8 5.2.7-3.8 3.7.9 5.2L12 16.2l-4.6 2.5.9-5.2-3.8-3.7 5.2-.7z",
                    "M12 4.3l2.3 4.8 5.2.7-3.8 3.7.9 5.2L12 16.2l-4.6 2.5.9-5.2-3.8-3.7 5.2-.7z", nullptr}},
        {Ic::Alert, {"M12 4a8 8 0 1 0 0 16a8 8 0 1 0 0-16zM12 8v4.6", "M12 4a8 8 0 1 0 0 16a8 8 0 1 0 0-16z",
                     "M12 14.9a1.1 1.1 0 1 0 0 2.2a1.1 1.1 0 1 0 0-2.2z"}},
        {Ic::Retry, {"M18.6 13.5A7 7 0 1 1 17 7.2M18.5 4v4h-4", nullptr, nullptr}},
        {Ic::Plus, {"M12 5.5v13M5.5 12h13", nullptr, nullptr}},
        {Ic::Trash, {"M4.5 7h15M9.5 7V5.5a1 1 0 0 1 1-1h3a1 1 0 0 1 1 1V7M6.5 7l.8 11.6a1.5 1.5 0 0 0 1.5 1.4h6.4a1.5 1.5 0 0 0 1.5-1.4L17.5 7M10.2 10.5v6M13.8 10.5v6",
                     "M6.5 7l.8 11.6a1.5 1.5 0 0 0 1.5 1.4h6.4a1.5 1.5 0 0 0 1.5-1.4L17.5 7z", nullptr}},
        {Ic::Channel, {"M4.5 10.2v3.6a1.2 1.2 0 0 0 1.2 1.2h1.8l7.3 4V5l-7.3 4H5.7a1.2 1.2 0 0 0-1.2 1.2zM8 15l1 4M18 9.3a3.8 3.8 0 0 1 0 5.4",
                       "M4.5 10.2v3.6a1.2 1.2 0 0 0 1.2 1.2h1.8l7.3 4V5l-7.3 4H5.7a1.2 1.2 0 0 0-1.2 1.2z", nullptr}},
        {Ic::Group, {"M9 5.2a3 3 0 1 0 0 6a3 3 0 1 0 0-6zM3.5 19a5.5 5.5 0 0 1 11 0M15.8 5.6a2.7 2.7 0 0 1 0 5.2M17 13.7a4.8 4.8 0 0 1 3.5 4.8",
                     "M9 5.2a3 3 0 1 0 0 6a3 3 0 1 0 0-6z", nullptr}},
        {Ic::Person, {"M12 4.5a3.5 3.5 0 1 0 0 7a3.5 3.5 0 1 0 0-7zM5 19.5a7 7 0 0 1 14 0", "M12 4.5a3.5 3.5 0 1 0 0 7a3.5 3.5 0 1 0 0-7z", nullptr}},
        {Ic::ChevronDown, {"M7 10l5 5 5-5", nullptr, nullptr}},
        {Ic::ChevronUp, {"M7 14l5-5 5 5", nullptr, nullptr}},
        {Ic::Logout, {"M13.5 4.5H7A1.5 1.5 0 0 0 5.5 6v12A1.5 1.5 0 0 0 7 19.5h6.5M10 12h10M16.5 8.5 20 12l-3.5 3.5", nullptr, nullptr}},
        {Ic::Leave, {"M13.5 4.5H7A1.5 1.5 0 0 0 5.5 6v12A1.5 1.5 0 0 0 7 19.5h6.5M10 12h10M16.5 8.5 20 12l-3.5 3.5", nullptr, nullptr}},
        {Ic::Doc, {"M7 3.5h6.2c.4 0 .8.2 1.1.4l3.8 3.8c.3.3.4.7.4 1.1V19a1.5 1.5 0 0 1-1.5 1.5H7A1.5 1.5 0 0 1 5.5 19V5A1.5 1.5 0 0 1 7 3.5zM13.5 3.8V8h4.2",
                   "M7 3.5h6.2c.4 0 .8.2 1.1.4l3.8 3.8c.3.3.4.7.4 1.1V19a1.5 1.5 0 0 1-1.5 1.5H7A1.5 1.5 0 0 1 5.5 19V5A1.5 1.5 0 0 1 7 3.5z", nullptr}},
        {Ic::More, {"", nullptr, "M6 10.6a1.4 1.4 0 1 0 0 2.8a1.4 1.4 0 1 0 0-2.8zM12 10.6a1.4 1.4 0 1 0 0 2.8a1.4 1.4 0 1 0 0-2.8zM18 10.6a1.4 1.4 0 1 0 0 2.8a1.4 1.4 0 1 0 0-2.8z"}},
        // pulsanti della finestra (stile Windows 11, tratto sottile)
        {Ic::WinMin, {"M7 12h10", nullptr, nullptr}},
        {Ic::WinMax, {"M8.5 7h7A1.5 1.5 0 0 1 17 8.5v7a1.5 1.5 0 0 1-1.5 1.5h-7A1.5 1.5 0 0 1 7 15.5v-7A1.5 1.5 0 0 1 8.5 7z", nullptr, nullptr}},
        {Ic::WinRestore, {"M8.5 9h6A1.5 1.5 0 0 1 16 10.5v6a1.5 1.5 0 0 1-1.5 1.5h-6A1.5 1.5 0 0 1 7 16.5v-6A1.5 1.5 0 0 1 8.5 9zM9.5 6.8h6.2a2.3 2.3 0 0 1 2.3 2.3v6.2", nullptr, nullptr}},
        {Ic::WinClose, {"M7.5 7.5l9 9M16.5 7.5l-9 9", nullptr, nullptr}},
    };
    auto it = defs.find(ic);
    return it == defs.end() ? none : it->second;
}

// Ingranaggio: 8 denti arrotondati e foro centrale, generato una volta.
std::string gearPath(bool outlineOnly) {
    const double cx = 12, cy = 12, rIn = 6.4, rOut = 8.6, pi = 3.14159265358979;
    const int teeth = 8;
    auto pt = [&](double r, double a) {
        char b[64];
        std::snprintf(b, sizeof b, "%.3f %.3f", cx + r * std::cos(a), cy + r * std::sin(a));
        return std::string(b);
    };
    auto arc = [](double r) {
        char b[48];
        std::snprintf(b, sizeof b, "A%.2f %.2f 0 0 1 ", r, r);
        return std::string(b);
    };
    std::string p;
    for (int i = 0; i < teeth; ++i) {
        double a0 = (i - 0.22) * 2 * pi / teeth, a1 = (i + 0.22) * 2 * pi / teeth;
        double b0 = (i + 0.30) * 2 * pi / teeth, b1 = (i + 0.70) * 2 * pi / teeth;
        p += (i == 0 ? "M" : "L") + pt(rOut, a0);
        p += arc(rOut) + pt(rOut, a1);
        p += "L" + pt(rIn, b0);
        p += arc(rIn) + pt(rIn, b1);
    }
    p += "Z";
    if (!outlineOnly) p += "M12 9.4a2.6 2.6 0 1 0 0 5.2a2.6 2.6 0 1 0 0-5.2z";
    return p;
}

// --- mini parser della sintassi "d" di SVG: M L H V C S Q A Z (assoluti e relativi)
class PathParser {
public:
    explicit PathParser(const char* s) : p_(s) {}

    void run(ID2D1GeometrySink* sink) {
        char cmd = 0;
        D2D1_POINT_2F cur{0, 0}, start{0, 0}, lastCtrl{0, 0};
        bool open = false;
        char prev = 0;
        auto begin = [&](D2D1_POINT_2F at) {
            if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->BeginFigure(at, D2D1_FIGURE_BEGIN_FILLED);
            open = true;
            start = at;
        };
        while (true) {
            skip();
            if (!*p_) break;
            if (std::isalpha((unsigned char)*p_)) cmd = *p_++;
            else if (!cmd) break;
            bool rel = std::islower((unsigned char)cmd) != 0;
            char c = (char)std::toupper((unsigned char)cmd);
            float ox = rel ? cur.x : 0, oy = rel ? cur.y : 0;
            switch (c) {
                case 'M': {
                    D2D1_POINT_2F pt{ox + num(), oy + num()};
                    begin(pt);
                    cur = pt;
                    cmd = rel ? 'l' : 'L';  // coppie successive = linee
                    break;
                }
                case 'L': { cur = {ox + num(), oy + num()}; sink->AddLine(cur); break; }
                case 'H': { cur.x = ox + num(); sink->AddLine(cur); break; }
                case 'V': { cur.y = oy + num(); sink->AddLine(cur); break; }
                case 'C': {
                    D2D1_POINT_2F a{ox + num(), oy + num()}, b{ox + num(), oy + num()}, e{ox + num(), oy + num()};
                    sink->AddBezier(D2D1::BezierSegment(a, b, e));
                    lastCtrl = b;
                    cur = e;
                    break;
                }
                case 'S': {
                    D2D1_POINT_2F a = (prev == 'C' || prev == 'S') ? D2D1::Point2F(2 * cur.x - lastCtrl.x, 2 * cur.y - lastCtrl.y) : cur;
                    D2D1_POINT_2F b{ox + num(), oy + num()}, e{ox + num(), oy + num()};
                    sink->AddBezier(D2D1::BezierSegment(a, b, e));
                    lastCtrl = b;
                    cur = e;
                    break;
                }
                case 'Q': {
                    D2D1_POINT_2F a{ox + num(), oy + num()}, e{ox + num(), oy + num()};
                    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(a, e));
                    cur = e;
                    break;
                }
                case 'A': {
                    float rx = num(), ry = num(), rot = num();
                    int large = flag(), sweep = flag();
                    D2D1_POINT_2F e{ox + num(), oy + num()};
                    arcToBeziers(sink, cur, e, rx, ry, rot, large != 0, sweep != 0);
                    cur = e;
                    break;
                }
                case 'Z':
                    if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    open = false;
                    cur = start;
                    cmd = 0;
                    break;
                default:
                    return;
            }
            prev = c;
        }
        if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    }

private:
    // Arco SVG -> curve di Bézier cubiche (W3C SVG, appendice F.6). Evita AddArc, così il disegno è
    // identico ovunque (anche dove l'implementazione degli archi di Direct2D è incompleta).
    static void arcToBeziers(ID2D1GeometrySink* sink, D2D1_POINT_2F p0, D2D1_POINT_2F p1, float rx, float ry,
                             float rotDeg, bool large, bool sweep) {
        const double pi = 3.14159265358979323846;
        if (rx == 0 || ry == 0 || (p0.x == p1.x && p0.y == p1.y)) { sink->AddLine(p1); return; }
        double phi = rotDeg * pi / 180, cp = std::cos(phi), sp = std::sin(phi);
        double dx = (p0.x - p1.x) / 2.0, dy = (p0.y - p1.y) / 2.0;
        double x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
        double Rx = std::fabs(rx), Ry = std::fabs(ry);
        double lam = (x1 * x1) / (Rx * Rx) + (y1 * y1) / (Ry * Ry);
        if (lam > 1) { Rx *= std::sqrt(lam); Ry *= std::sqrt(lam); }
        double num = Rx * Rx * Ry * Ry - Rx * Rx * y1 * y1 - Ry * Ry * x1 * x1;
        double den = Rx * Rx * y1 * y1 + Ry * Ry * x1 * x1;
        double co = den == 0 ? 0 : std::sqrt(std::max(0.0, num / den));
        if (large == sweep) co = -co;
        double cxp = co * Rx * y1 / Ry, cyp = -co * Ry * x1 / Rx;
        double cx = cp * cxp - sp * cyp + (p0.x + p1.x) / 2.0, cy = sp * cxp + cp * cyp + (p0.y + p1.y) / 2.0;
        auto ang = [](double ux, double uy, double vx, double vy) { return std::atan2(ux * vy - uy * vx, ux * vx + uy * vy); };
        double t1 = ang(1, 0, (x1 - cxp) / Rx, (y1 - cyp) / Ry);
        double dt = ang((x1 - cxp) / Rx, (y1 - cyp) / Ry, (-x1 - cxp) / Rx, (-y1 - cyp) / Ry);
        if (!sweep && dt > 0) dt -= 2 * pi;
        if (sweep && dt < 0) dt += 2 * pi;
        int segs = (int)std::ceil(std::fabs(dt) / (pi / 2) - 1e-6);
        if (segs < 1) segs = 1;
        double d = dt / segs, k = 4.0 / 3.0 * std::tan(d / 4);
        double t = t1;
        for (int i = 0; i < segs; ++i) {
            double ta = t, tb = t + d;
            // controlli: P(ta) + k*P'(ta), P(tb) - k*P'(tb)
            auto P = [&](double tt) { return std::make_pair(Rx * std::cos(tt), Ry * std::sin(tt)); };
            auto D = [&](double tt) { return std::make_pair(-Rx * std::sin(tt), Ry * std::cos(tt)); };
            auto tr = [&](double x, double y) { return D2D1::Point2F((float)(cp * x - sp * y + cx), (float)(sp * x + cp * y + cy)); };
            auto pa = P(ta), da = D(ta), pb = P(tb), db = D(tb);
            D2D1_POINT_2F a1 = tr(pa.first + k * da.first, pa.second + k * da.second);
            D2D1_POINT_2F b1 = tr(pb.first - k * db.first, pb.second - k * db.second);
            D2D1_POINT_2F e1 = i == segs - 1 ? p1 : tr(pb.first, pb.second);
            sink->AddBezier(D2D1::BezierSegment(a1, b1, e1));
            t = tb;
        }
    }

    void skip() { while (*p_ == ' ' || *p_ == ',' || *p_ == '\n' || *p_ == '\t') ++p_; }
    float num() {
        skip();
        char* end = nullptr;
        float v = std::strtof(p_, &end);
        if (end == p_) return 0;
        p_ = end;
        return v;
    }
    int flag() {  // nei flag degli archi "1 0" può essere scritto "10"
        skip();
        int v = *p_ == '1';
        if (*p_ == '0' || *p_ == '1') ++p_;
        return v;
    }
    const char* p_;
};

} // namespace

ComPtr<ID2D1PathGeometry> Icons::parse(const char* svg) {
    ComPtr<ID2D1PathGeometry> g;
    if (!svg || !*svg || !factory_ || FAILED(factory_->CreatePathGeometry(g.GetAddressOf()))) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(g->Open(sink.GetAddressOf()))) return nullptr;
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    PathParser(svg).run(sink.Get());
    sink->Close();
    return g;
}

const Icons::Geo& Icons::get(Ic ic) {
    auto it = cache_.find(ic);
    if (it != cache_.end()) return it->second;
    Geo g;
    if (ic == Ic::Gear) {
        static const std::string full = gearPath(false), outer = gearPath(true);
        g.stroke = parse(full.c_str());
        g.fill = parse(outer.c_str());
    } else {
        const Def& d = def(ic);
        g.stroke = parse(d.stroke);
        g.fill = parse(d.fill);
        g.solid = parse(d.solid);
    }
    return cache_[ic] = g;
}

void Icons::draw(ID2D1RenderTarget* rt, Ic ic, D2D1_RECT_F r, D2D1_COLOR_F color, float weight, bool duotone) {
    if (!rt || !factory_) return;
    if (!round_) {
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                      D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        factory_->CreateStrokeStyle(sp, nullptr, 0, round_.GetAddressOf());
    }
    const Geo& g = get(ic);
    float side = std::min(r.right - r.left, r.bottom - r.top);
    float k = side / 24.f;
    D2D1_MATRIX_3X2_F old;
    rt->GetTransform(&old);
    rt->SetTransform(D2D1::Matrix3x2F::Scale(k, k) *
                     D2D1::Matrix3x2F::Translation((r.left + r.right - side) / 2, (r.top + r.bottom - side) / 2) * old);
    ComPtr<ID2D1SolidColorBrush> br;
    rt->CreateSolidColorBrush(color, br.GetAddressOf());
    if (br) {
        if (duotone && g.fill) {
            D2D1_COLOR_F soft = color;
            soft.a *= 0.18f;
            br->SetColor(soft);
            rt->FillGeometry(g.fill.Get(), br.Get());
            br->SetColor(color);
        }
        if (g.solid) rt->FillGeometry(g.solid.Get(), br.Get());
        if (g.stroke) rt->DrawGeometry(g.stroke.Get(), br.Get(), weight * std::max(1.f, 18.f / side), round_.Get());
    }
    rt->SetTransform(old);
}

} // namespace tf
