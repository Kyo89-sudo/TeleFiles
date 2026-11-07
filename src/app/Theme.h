// TeleFiles - colori stile Windows 11 (chiaro / scuro)
#pragma once
#include <d2d1.h>

namespace tf {

inline D2D1_COLOR_F rgb(unsigned v, float a = 1.f) {
    return D2D1::ColorF((float)((v >> 16) & 255) / 255.f, (float)((v >> 8) & 255) / 255.f, (float)(v & 255) / 255.f, a);
}

struct Palette {
    D2D1_COLOR_F bg, surface, surfaceAlt, border, text, text2, text3, accent, accentText, accentSoft,
        hover, hoverSurface, selected, success, error, track, inputBg, inputBorder, gold, goldSoft, overlay;
    unsigned editText, editBg;  // COLORREF per i controlli EDIT nativi (0x00BBGGRR)
    bool dark;
};

inline Palette lightPalette() {
    Palette p;
    p.bg = rgb(0xEEF1F5);
    p.surface = rgb(0xFFFFFF);
    p.surfaceAlt = rgb(0xF7F8FA);
    p.border = rgb(0xDCDFE4);
    p.text = rgb(0x1B1B1B);
    p.text2 = rgb(0x5F6368);
    p.text3 = rgb(0x8A8F96);
    p.accent = rgb(0x005FB8);
    p.accentText = rgb(0xFFFFFF);
    p.accentSoft = rgb(0xE3ECF8);
    p.hover = rgb(0xE4E7EC);
    p.hoverSurface = rgb(0xF3F5F8);
    p.selected = rgb(0xDDE2E9);
    p.success = rgb(0x0F7B3F);
    p.error = rgb(0xC42B1C);
    p.track = rgb(0xE3E6EA);
    p.inputBg = rgb(0xFBFBFC);
    p.inputBorder = rgb(0xD9DCE1);
    p.gold = rgb(0x9A6B00);
    p.goldSoft = rgb(0xFBF0D5);
    p.overlay = rgb(0x000000, 0.04f);
    p.editText = 0x001B1B1B;
    p.editBg = 0x00FCFBFB;
    p.dark = false;
    return p;
}

inline Palette darkPalette() {
    Palette p;
    p.bg = rgb(0x202020);
    p.surface = rgb(0x2B2B2B);
    p.surfaceAlt = rgb(0x272727);
    p.border = rgb(0x3A3A3A);
    p.text = rgb(0xFFFFFF);
    p.text2 = rgb(0xC5C5C5);
    p.text3 = rgb(0x9A9A9A);
    p.accent = rgb(0x60CDFF);
    p.accentText = rgb(0x000000);
    p.accentSoft = rgb(0x1E3A4C);
    p.hover = rgb(0x2D2D2D);
    p.hoverSurface = rgb(0x333333);
    p.selected = rgb(0x383838);
    p.success = rgb(0x6CCB5F);
    p.error = rgb(0xFF99A4);
    p.track = rgb(0x404040);
    p.inputBg = rgb(0x2D2D2D);
    p.inputBorder = rgb(0x474747);
    p.gold = rgb(0xE8C35A);
    p.goldSoft = rgb(0x3D3320);
    p.overlay = rgb(0xFFFFFF, 0.05f);
    p.editText = 0x00FFFFFF;
    p.editBg = 0x002D2D2D;
    p.dark = true;
    return p;
}

} // namespace tf
