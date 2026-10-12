// The candidate window, as docs/AquaType/ui-design/design-spec.md §1 draws it (Windows 11 list style, as Microsoft
// Bopomofo): one column, the page the composer chose, absolute numbers kept in the background (digits are bopomofo keys,
// so they never select), a soft highlight with an accent bar, the 「記憶」 note, a thin line between the groups
// (sentences / words / characters), and a footer with the group hint (句 · 詞 · 字) and the page.  A topmost popup that
// never takes focus, placed so the candidates line up with the composition, flipped above it when there is no room.
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
    std::function<void(bool)> on_page;           // a page button: true = down
    ~CandidateWindow();
    void show(const aqtip::PanelView& view, const RECT& anchor);   // anchor: the composition's screen rectangle
    void update(const aqtip::PanelView& view);                     // new content, same anchor
    void hide();
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
    // The candidates' text size in DIPs (18 by default; the settings offer 16 / 18 / 22).  Lines are 4/3 of it.
    void set_text_size(float size);
    // Draws the window into a PNG instead of on screen (for checking the look without a window on the desktop).
    bool render_png(const aqtip::PanelView& view, const wchar_t* path, UINT dpi, bool dark, int hover = -1);
private:
    struct Row { int index; float top, height; IDWriteTextLayout* text = nullptr; };
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    bool ensure();
    bool ensure_resources();
    void measure();                              // rows, dividers, size (DIPs)
    void layout();                               // measure, then place the window
    void paint();
    void draw(ID2D1RenderTarget* rt);
    void release_rows();
    int row_at(float x, float y) const;
    int button_at(float x, float y) const;       // 0 none, 1 up, 2 down
    bool page_enabled(bool down) const;
    float line_h() const { return std::round(text_size_ * 4 / 3); }
    float row_min_h() const { return line_h() + 8; }

    HWND hwnd_ = nullptr; aqtip::PanelView view_; RECT anchor_{};
    ID2D1Factory* d2d_ = nullptr; IDWriteFactory* dw_ = nullptr; ID2D1HwndRenderTarget* rt_ = nullptr;
    IDWriteTextFormat *text_fmt_ = nullptr, *num_fmt_ = nullptr, *note_fmt_ = nullptr, *group_fmt_ = nullptr, *group_bold_fmt_ = nullptr, *icon_fmt_ = nullptr;
    std::vector<Row> rows_; std::vector<float> dividers_;
    float width_ = 0, height_ = 0, text_w_ = 0, note_w_ = 0, footer_top_ = -1;
    float text_size_ = 18;
    UINT dpi_ = 96; bool dark_ = false, high_contrast_ = false, dwm_border_ = false;
    int hover_ = -1, hover_button_ = 0; bool tracking_ = false;
};

}  // namespace aqtsf
