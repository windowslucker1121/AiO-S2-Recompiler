// SPDX-License-Identifier: GPL-3.0-or-later
// The game library's look: colours of the dark theme and GDI+ helpers (rounded shapes, text), shared by the window (setup_main.cpp)
// and the settings panel (setup_settings.cpp).
#pragma once
#include <windows.h>
#include <objidl.h>

#include <algorithm>
#include <string>

namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

namespace setup::theme {

namespace gp = Gdiplus;

constexpr COLORREF kBg = RGB(0x12, 0x14, 0x19), kSide = RGB(0x18, 0x1B, 0x22), kCard = RGB(0x20, 0x24, 0x2D), kCardHi = RGB(0x29, 0x2E, 0x39),
                   kLine = RGB(0x2C, 0x31, 0x3C), kText = RGB(0xEC, 0xEE, 0xF2), kMuted = RGB(0x95, 0x9C, 0xA8), kAccent = RGB(0x5B, 0x8C, 0xFF),
                   kAccentHi = RGB(0x7A, 0xA2, 0xFF), kOk = RGB(0x3C, 0xCF, 0x7A), kWarn = RGB(0xF2, 0xB8, 0x4B), kBad = RGB(0xFF, 0x6B, 0x6B),
                   kField = RGB(0x1A, 0x1D, 0x24), kButton = RGB(0x2C, 0x31, 0x3C), kButtonHi = RGB(0x3A, 0x40, 0x4D);

inline gp::Color C(COLORREF c, BYTE a = 255) { return gp::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

inline void round_path(gp::GraphicsPath& p, gp::RectF r, float rad) {
    const float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}
inline void fill_round(gp::Graphics& g, gp::RectF r, float rad, const gp::Brush& b) {
    gp::GraphicsPath p;
    round_path(p, r, rad);
    g.FillPath(&b, &p);
}
inline void text(gp::Graphics& g, const std::wstring& t, const gp::Font& f, gp::RectF r, COLORREF c, gp::StringAlignment h = gp::StringAlignmentNear,
                 gp::StringAlignment v = gp::StringAlignmentNear, bool wrap = false) {
    gp::StringFormat sf;
    sf.SetAlignment(h);
    sf.SetLineAlignment(v);
    sf.SetTrimming(gp::StringTrimmingEllipsisCharacter);
    if (!wrap) sf.SetFormatFlags(gp::StringFormatFlagsNoWrap);
    gp::SolidBrush b(C(c));
    g.DrawString(t.c_str(), static_cast<INT>(t.size()), &f, r, &sf, &b);
}

// A double-buffered GDI+ paint of a whole window: fn(graphics, client rect).
template <class F>
void paint_buffered(HDC hdc, const RECT& rc, F fn) {
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bm = CreateCompatibleBitmap(hdc, std::max<LONG>(rc.right, 1), std::max<LONG>(rc.bottom, 1));
    HGDIOBJ old = SelectObject(dc, bm);
    {
        gp::Graphics g(dc);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
        fn(g);
    }
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bm);
    DeleteDC(dc);
}

}  // namespace setup::theme
