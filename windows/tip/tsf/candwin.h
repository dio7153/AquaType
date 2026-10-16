// The candidate window, as AquaType draws it on the Mac (packaging/payload/rime/squirrel.custom.yaml): one white column
// in light and dark mode alike, grey numbers kept in the background (digits are bopomofo keys, so they never select), the
// highlighted candidate on a blue capsule, the 「記憶」 note; no group lines, no page footer.  The highlight follows the
// pointer, a click picks, the wheel turns pages.  Candidates wrap at the Mac's share of the screen.  A topmost popup
// that never takes focus, placed so the candidates line up with the composition, above it when there is no room below.
// Drawn with Direct2D and DirectWrite.
#pragma once
#include "globals.h"            // first: NOMINMAX before any Windows header
#include <cmath>
#include <functional>
#include <vector>
#include <d2d1.h>
#include <dwrite.h>
#include "composer.h"

namespace aqtsf {

class CandidateWindow {
public:
    std::function<void(int)> on_select;          // a row was clicked: its index in the candidate list
    std::function<void(bool)> on_page;           // the wheel: true = down
    std::function<void(int)> on_hover;           // the pointer is over a row: its index
    std::function<void()> on_painted;            // a paint finished (the text service's AQ_TRACE timing)
    ~CandidateWindow();
    void show(const aqtip::PanelView& view, const RECT& anchor);   // anchor: the composition's screen rectangle
    void update(const aqtip::PanelView& view);                     // new content, same anchor
    void hide();
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    // The candidates' text size in DIPs (15 by default, the Mac's; the settings offer 13 / 15 / 18).  Lines are 4/3 of it.
    void set_text_size(float size);
    float text_size() const { return text_size_; }
    // How wide candidates may be before they wrap, and how many full-width characters that is (the composer pages by
    // it), for a composition at `anchor`.
    static float text_max_width(const RECT& anchor, float text_size);
    static int columns(const RECT& anchor, float text_size);
    // Whether some installed font has every character of `text` (what DirectWrite's fallback could draw).
    static bool drawable(const std::wstring& text);
    // Draws the window into a PNG instead of on screen (for checking the look without a window on the desktop).
    bool render_png(const aqtip::PanelView& view, const wchar_t* path, UINT dpi, bool dark, int hover = -1);
private:
    struct Row { int index; float top, height; IDWriteTextLayout* text = nullptr; };
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    bool ensure();
    bool ensure_resources();
    void measure();                              // rows, size (DIPs)
    void layout();                               // measure, then place the window
    void paint();
    void draw(ID2D1RenderTarget* rt);
    void release_rows();
    int row_at(float x, float y) const;
    float line_h() const { return std::round(text_size_ * 4 / 3); }
    float row_min_h() const { return line_h() + 5; }

    HWND hwnd_ = nullptr; aqtip::PanelView view_; RECT anchor_{};
    ID2D1Factory* d2d_ = nullptr; IDWriteFactory* dw_ = nullptr; ID2D1HwndRenderTarget* rt_ = nullptr;
    IDWriteTextFormat *text_fmt_ = nullptr, *num_fmt_ = nullptr, *note_fmt_ = nullptr;
    std::vector<Row> rows_;
    float width_ = 0, height_ = 0, text_w_ = 0, note_w_ = 0, text_max_w_ = 360;
    float text_size_ = 15;
    UINT dpi_ = 96; bool high_contrast_ = false, dwm_border_ = false;
    int side_ = 0, wheel_ = 0;                   // side: 1 below the composition, -1 above, 0 not chosen yet
};

}  // namespace aqtsf
