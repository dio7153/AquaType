// The candidate window: one column, the page the composer chose, absolute numbers as labels (digits are bopomofo keys,
// so they never select), the 「記憶」 note, the highlighted row.  A topmost popup that never takes focus, placed under the
// composition and flipped above it when there is no room below.  Clicks and the wheel go back to the text service.
#pragma once
#include <functional>
#include "globals.h"
#include "composer.h"

namespace aqtsf {

class CandidateWindow {
public:
    std::function<void(int)> on_select;          // a row was clicked: its index in the candidate list
    std::function<void(bool)> on_page;           // the wheel: true = down
    ~CandidateWindow();
    void show(const aqtip::PanelView& view, const RECT& anchor);   // anchor: the composition's screen rectangle
    void update(const aqtip::PanelView& view);                     // new content, same anchor
    void hide();
    bool visible() const { return hwnd_ && IsWindowVisible(hwnd_); }
private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    bool ensure();
    void layout();
    void paint(HDC dc);
    int row_at(POINT p) const;
    HWND hwnd_ = nullptr; aqtip::PanelView view_; RECT anchor_{};
    struct Row { int index; RECT rc; };
    std::vector<Row> rows_; SIZE size_{}; UINT dpi_ = 96; bool dark_ = false;
    HFONT text_font_ = nullptr, label_font_ = nullptr; UINT font_dpi_ = 0;
};

}  // namespace aqtsf
