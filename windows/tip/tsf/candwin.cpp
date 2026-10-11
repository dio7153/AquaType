#include "candwin.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <string>

namespace aqtsf {
namespace {
const wchar_t kClass[] = L"AquaTypeCandidateWindow";
constexpr int kPadding = 6, kRowGap = 2, kLabelGap = 8, kLineChars = 20;
struct Palette { COLORREF bg, border, text, label, hi_bg, hi_text, note, dim; };
const Palette kLight = {RGB(255, 255, 255), RGB(200, 200, 204), RGB(30, 30, 32), RGB(140, 140, 146), RGB(0, 103, 192), RGB(255, 255, 255), RGB(0, 120, 90), RGB(150, 150, 156)};
const Palette kDark = {RGB(43, 43, 46), RGB(80, 80, 86), RGB(235, 235, 238), RGB(150, 150, 156), RGB(76, 160, 255), RGB(16, 16, 18), RGB(90, 200, 160), RGB(130, 130, 136)};
bool system_dark() {
    DWORD v = 1, n = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n) == ERROR_SUCCESS && v == 0;
}
}  // namespace

CandidateWindow::~CandidateWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (text_font_) DeleteObject(text_font_); if (label_font_) DeleteObject(label_font_);
}

bool CandidateWindow::ensure() {
    if (hwnd_) return true;
    WNDCLASSEXW wc{sizeof wc}; wc.style = CS_DROPSHADOW | CS_HREDRAW | CS_VREDRAW; wc.lpfnWndProc = proc; wc.hInstance = g_module;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = kClass;
    RegisterClassExW(&wc);                                   // once per process; a second call fails harmlessly
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_module, this);
    if (!hwnd_) return false;
    const DWORD round = 3 /* DWMWCP_ROUNDSMALL */; DwmSetWindowAttribute(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
    return true;
}

void CandidateWindow::layout() {
    HMONITOR mon = MonitorFromRect(&anchor_, MONITOR_DEFAULTTONEAREST); UINT dx = 96, dy = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static auto get_dpi = (GetDpiForMonitorFn)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor");
    if (get_dpi) get_dpi(mon, 0, &dx, &dy);
    dpi_ = dx; dark_ = system_dark();
    if (font_dpi_ != dpi_) {
        if (text_font_) DeleteObject(text_font_); if (label_font_) DeleteObject(label_font_);
        text_font_ = CreateFontW(-MulDiv(18, dpi_, 72) * 72 / 96, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft JhengHei UI");
        label_font_ = CreateFontW(-MulDiv(12, dpi_, 72) * 72 / 96, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        font_dpi_ = dpi_;
    }
    const int pad = MulDiv(kPadding, dpi_, 96), gap = MulDiv(kRowGap, dpi_, 96), lgap = MulDiv(kLabelGap, dpi_, 96);
    HDC dc = GetDC(hwnd_);
    SelectObject(dc, label_font_); SIZE lab{}; GetTextExtentPoint32W(dc, L"63.", 3, &lab);
    SIZE note{}; GetTextExtentPoint32W(dc, L"記憶", 2, &note);
    SelectObject(dc, text_font_); SIZE ch{}; GetTextExtentPoint32W(dc, L"國", 1, &ch);
    const int text_w = ch.cx * kLineChars;
    rows_.clear(); int y = pad, widest = ch.cx * 4;
    for (int idx : view_.rows) {
        const std::wstring& t = (*view_.cands)[idx].wtext; RECT r{0, 0, text_w, 0};
        DrawTextW(dc, t.c_str(), (int)t.size(), &r, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        widest = std::max(widest, (int)(r.right - r.left));
        const int h = std::max((int)(r.bottom - r.top), (int)ch.cy) + 2 * gap;
        rows_.push_back({idx, RECT{0, y, 0, y + h}}); y += h;
    }
    ReleaseDC(hwnd_, dc);
    const bool many = view_.cands && view_.cands->size() > view_.rows.size();
    const int w = pad + lab.cx + lgap + widest + lgap + note.cx + pad;
    for (auto& r : rows_) { r.rc.left = 0; r.rc.right = w; }
    size_ = SIZE{w, y + pad + (many ? lab.cy : 0)};
    MONITORINFO mi{sizeof mi}; GetMonitorInfoW(mon, &mi); const RECT& wa = mi.rcWork;
    int x = anchor_.left, top = anchor_.bottom + MulDiv(4, dpi_, 96);
    if (top + size_.cy > wa.bottom) top = anchor_.top - MulDiv(4, dpi_, 96) - size_.cy;          // no room below: above the text
    x = std::max((int)wa.left, std::min(x, (int)wa.right - (int)size_.cx)); top = std::max((int)wa.top, top);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, top, size_.cx, size_.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void CandidateWindow::show(const aqtip::PanelView& v, const RECT& anchor) { anchor_ = anchor; update(v); }
void CandidateWindow::update(const aqtip::PanelView& v) {
    view_ = v;
    if (!view_.cands || view_.rows.empty()) { hide(); return; }
    if (!ensure()) return;
    layout(); InvalidateRect(hwnd_, nullptr, FALSE);
}
void CandidateWindow::hide() { if (hwnd_) ShowWindow(hwnd_, SW_HIDE); }

void CandidateWindow::paint(HDC dc) {
    const Palette& P = dark_ ? kDark : kLight; RECT all{0, 0, size_.cx, size_.cy};
    HBRUSH bg = CreateSolidBrush(P.bg); FillRect(dc, &all, bg); DeleteObject(bg);
    HBRUSH bd = CreateSolidBrush(P.border); FrameRect(dc, &all, bd); DeleteObject(bd);
    SetBkMode(dc, TRANSPARENT);
    const int pad = MulDiv(kPadding, dpi_, 96), gap = MulDiv(kRowGap, dpi_, 96), lgap = MulDiv(kLabelGap, dpi_, 96);
    SelectObject(dc, label_font_); SIZE lab{}; GetTextExtentPoint32W(dc, L"63.", 3, &lab); SIZE note{}; GetTextExtentPoint32W(dc, L"記憶", 2, &note);
    for (const Row& r : rows_) {
        const auto& c = (*view_.cands)[r.index]; const bool hi = r.index == view_.highlighted && !view_.interim;
        if (hi) { RECT h = r.rc; InflateRect(&h, -MulDiv(2, dpi_, 96), 0); HBRUSH b = CreateSolidBrush(P.hi_bg); FillRect(dc, &h, b); DeleteObject(b); }
        const std::wstring num = std::to_wstring(r.index + 1) + L".";
        SelectObject(dc, label_font_); SetTextColor(dc, hi ? P.hi_text : P.label);
        RECT lr{pad, r.rc.top + gap, pad + lab.cx, r.rc.bottom - gap}; DrawTextW(dc, num.c_str(), (int)num.size(), &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, text_font_); SetTextColor(dc, hi ? P.hi_text : (view_.interim ? P.dim : P.text));
        RECT tr{pad + lab.cx + lgap, r.rc.top + gap, r.rc.right - pad - note.cx - lgap, r.rc.bottom - gap};
        DrawTextW(dc, c.wtext.c_str(), (int)c.wtext.size(), &tr, DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
        if (c.memory) { SelectObject(dc, label_font_); SetTextColor(dc, hi ? P.hi_text : P.note);
            RECT nr{r.rc.right - pad - note.cx, r.rc.top + gap, r.rc.right - pad, r.rc.bottom - gap}; DrawTextW(dc, L"記憶", 2, &nr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE); }
    }
    if (view_.cands->size() > view_.rows.size()) {                       // which part of the list this page is
        SelectObject(dc, label_font_); SetTextColor(dc, P.label);
        const std::wstring s = std::to_wstring(view_.rows.front() + 1) + L"–" + std::to_wstring(view_.rows.back() + 1) + L" / " + std::to_wstring(view_.cands->size());
        RECT pr{pad, size_.cy - pad - lab.cy, size_.cx - pad, size_.cy - pad}; DrawTextW(dc, s.c_str(), (int)s.size(), &pr, DT_RIGHT | DT_SINGLELINE);
    }
}

int CandidateWindow::row_at(POINT p) const { for (const Row& r : rows_) if (PtInRect(&r.rc, p)) return r.index; return -1; }

LRESULT CALLBACK CandidateWindow::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
    auto* self = (CandidateWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
            if (self) { HDC mem = CreateCompatibleDC(dc); HBITMAP bmp = CreateCompatibleBitmap(dc, self->size_.cx, self->size_.cy); HGDIOBJ old = SelectObject(mem, bmp);
                self->paint(mem); BitBlt(dc, 0, 0, self->size_.cx, self->size_.cy, mem, 0, 0, SRCCOPY); SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); }
            EndPaint(h, &ps); return 0; }
        case WM_LBUTTONUP: if (self && self->on_select && !self->view_.interim) { int i = self->row_at(POINT{GET_X_LPARAM(l), GET_Y_LPARAM(l)}); if (i >= 0) self->on_select(i); } return 0;
        case WM_MOUSEWHEEL: if (self && self->on_page) self->on_page(GET_WHEEL_DELTA_WPARAM(w) < 0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace aqtsf
