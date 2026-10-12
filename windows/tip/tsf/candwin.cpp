#include "candwin.h"
#include <dwmapi.h>
#include <wincodec.h>
#include <windowsx.h>
#include <algorithm>
#include <string>

namespace aqtsf {
namespace {
const wchar_t kClass[] = L"AquaTypeCandidateWindow";
// design-spec.md §1.3, in DIPs (96 DPI): Direct2D scales them to the monitor
constexpr float kPad = 4, kRowPadV = 4, kRowPadL = 12, kRowPadR = 10, kNumW = 16, kNumGap = 8, kNoteGap = 12,
                kTextMaxW = 360, kMinW = 200, kRowRadius = 4, kBarW = 3, kBarH = 16, kBarTop = 8, kDividerGap = 4, kDividerInset = 12,
                kFooterH = 28, kFooterPadL = 12, kFooterPadR = 4, kButton = 20, kPageGap = 6, kDotGap = 5;
struct Palette { D2D1_COLOR_F bg, border, text, text2, text3, hi, hover, accent, divider; };
D2D1_COLOR_F hex(UINT32 v, float a = 1.f) { return D2D1::ColorF(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, a); }
const Palette kLight = {hex(0xF9F9F9), hex(0xE0E0E0), hex(0x1A1A1A), hex(0x5F5F5F), hex(0x8A8A8A), hex(0xEAEAEA), hex(0xF2F2F2), hex(0x005FB8), hex(0xE5E5E5)};
const Palette kDark = {hex(0x2C2C2C), hex(0x454545), hex(0xFFFFFF), hex(0xC5C5C5), hex(0x9A9A9A), hex(0x3D3D3D), hex(0x343434), hex(0x60CDFF), hex(0x3D3D3D)};
D2D1_COLOR_F sys(int idx) { COLORREF c = GetSysColor(idx); return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f); }
bool apps_dark() {   // the candidate window follows the apps' theme (§1.5)
    DWORD v = 1, n = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n) == ERROR_SUCCESS && v == 0;
}
bool high_contrast() { HIGHCONTRASTW hc{sizeof hc}; return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON); }
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
bool has_family(IDWriteFactory* dw, const wchar_t* name) {
    IDWriteFontCollection* fc = nullptr; UINT32 i = 0; BOOL exists = FALSE;
    if (SUCCEEDED(dw->GetSystemFontCollection(&fc))) { fc->FindFamilyName(name, &i, &exists); fc->Release(); }
    return exists != FALSE;
}
const wchar_t* group_name(int g) { return g == 0 ? L"句" : g == 1 ? L"詞" : g == 2 ? L"字" : L"注"; }
}  // namespace

CandidateWindow::~CandidateWindow() {
    release_rows();
    release(text_fmt_); release(num_fmt_); release(note_fmt_); release(group_fmt_); release(group_bold_fmt_); release(icon_fmt_);
    release(rt_); release(dw_); release(d2d_);
    if (hwnd_) DestroyWindow(hwnd_);
}
void CandidateWindow::set_text_size(float size) {
    if (size == text_size_) return;
    text_size_ = size; release_rows(); release(text_fmt_);
    if (dw_) {                                                // the other formats stay; this one is made again at the new size
        dw_->CreateTextFormat(L"Microsoft JhengHei UI", nullptr, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, text_size_, L"zh-tw", &text_fmt_);
        if (text_fmt_) { text_fmt_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line_h(), line_h() * 0.8f); text_fmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER); }
    }
}
void CandidateWindow::release_rows() { for (auto& r : rows_) release(r.text); rows_.clear(); }

bool CandidateWindow::ensure_resources() {
    if (d2d_) return true;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw_))) return false;
    const wchar_t* cjk = L"Microsoft JhengHei UI"; const wchar_t* num = has_family(dw_, L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* icons = has_family(dw_, L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    auto fmt = [&](const wchar_t* fam, float size, DWRITE_FONT_WEIGHT w, IDWriteTextFormat** out) { dw_->CreateTextFormat(fam, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-tw", out); };
    fmt(cjk, text_size_, DWRITE_FONT_WEIGHT_REGULAR, &text_fmt_); fmt(num, 12, DWRITE_FONT_WEIGHT_REGULAR, &num_fmt_); fmt(cjk, 12, DWRITE_FONT_WEIGHT_REGULAR, &note_fmt_);
    fmt(cjk, 12, DWRITE_FONT_WEIGHT_REGULAR, &group_fmt_); fmt(cjk, 12, DWRITE_FONT_WEIGHT_BOLD, &group_bold_fmt_); fmt(icons, 10, DWRITE_FONT_WEIGHT_REGULAR, &icon_fmt_);
    if (!text_fmt_ || !num_fmt_ || !note_fmt_ || !group_fmt_ || !group_bold_fmt_ || !icon_fmt_) return false;
    text_fmt_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line_h(), line_h() * 0.8f);   // 24px lines at 18px (§1.3)
    text_fmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER);
    for (IDWriteTextFormat* f : {num_fmt_, note_fmt_, group_fmt_, group_bold_fmt_, icon_fmt_}) { f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    num_fmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING); icon_fmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    return true;
}

bool CandidateWindow::ensure() {
    if (hwnd_) return true;
    if (!ensure_resources()) return false;
    WNDCLASSEXW wc{sizeof wc}; wc.style = CS_DROPSHADOW; wc.lpfnWndProc = proc; wc.hInstance = g_module;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = kClass;
    RegisterClassExW(&wc);                                   // once per process; a second call fails harmlessly
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_module, this);
    if (!hwnd_) return false;
    const DWORD round = 2 /* DWMWCP_ROUND: 8px */;
    dwm_border_ = SUCCEEDED(DwmSetWindowAttribute(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round));   // fails before Windows 11: square, own border
    return true;
}

void CandidateWindow::measure() {
    const auto& C = *view_.cands;
    // the text column: as wide as the widest candidate on this page, at most 360 (then it wraps)
    release_rows(); text_w_ = 0; note_w_ = 0;
    for (int idx : view_.rows) {
        Row r{idx, 0, 0, nullptr};
        dw_->CreateTextLayout(C[idx].wtext.c_str(), (UINT32)C[idx].wtext.size(), text_fmt_, kTextMaxW, 1000, &r.text);
        if (r.text) { DWRITE_TEXT_METRICS m{}; r.text->GetMetrics(&m); text_w_ = std::max(text_w_, std::min(kTextMaxW, m.widthIncludingTrailingWhitespace)); r.height = std::max(row_min_h(), m.height + 2 * kRowPadV); }
        else r.height = row_min_h();
        if (C[idx].memory) note_w_ = 24;                     // 「記憶」 at 12px
        rows_.push_back(r);
    }
    for (auto& r : rows_) if (r.text) r.text->SetMaxWidth(std::max(text_w_, 1.f));
    // rows, with a divider wherever the group changes
    float y = kPad; dividers_.clear();
    for (size_t i = 0; i < rows_.size(); i++) {
        if (i > 0 && C[rows_[i].index].group != C[rows_[i - 1].index].group) { y += kDividerGap; dividers_.push_back(y); y += 1 + kDividerGap; }
        rows_[i].top = y; y += rows_[i].height;
    }
    int groups = 0; for (int g = 0; g < 4; g++) groups += std::any_of(C.begin(), C.end(), [g](const aqtip::Candidate& c) { return c.group == g; });
    const bool footer = C.size() > view_.rows.size() || groups >= 2;
    footer_top_ = footer ? y : -1; if (footer) y += kFooterH;
    height_ = y + kPad;
    width_ = std::max(kMinW, kPad + kRowPadL + kNumW + kNumGap + text_w_ + (note_w_ > 0 ? kNoteGap + note_w_ : 0) + kRowPadR + kPad);
    const float groups_w = groups * 14.f + std::max(0, groups - 1) * (2 * kDotGap + 6);   // 句 · 詞 · 字 · 注, as draw() lays them out
    if (footer) width_ = std::max(width_, kPad + kFooterPadL + groups_w + 16 + 90 + kPageGap + 2 * kButton + kFooterPadR + kPad);
}

void CandidateWindow::layout() {
    HMONITOR mon = MonitorFromRect(&anchor_, MONITOR_DEFAULTTONEAREST); UINT dx = 96, dy = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static auto get_dpi = (GetDpiForMonitorFn)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor");
    if (get_dpi) get_dpi(mon, 0, &dx, &dy);
    dpi_ = dx; dark_ = apps_dark(); high_contrast_ = high_contrast();
    measure();
    // position: the candidates' left edge under the composition's start, below it (or above when there is no room)
    const float s = dpi_ / 96.f; const int W = (int)std::ceil(width_ * s), H = (int)std::ceil(height_ * s);
    MONITORINFO mi{sizeof mi}; GetMonitorInfoW(mon, &mi); const RECT& wa = mi.rcWork;
    int x = anchor_.left - (int)std::lround((1 + kPad + kRowPadL + kNumW + kNumGap) * s), top = anchor_.bottom + (int)std::lround(4 * s);
    if (top + H > wa.bottom) top = anchor_.top - (int)std::lround(4 * s) - H;
    x = std::max((int)wa.left, std::min(x, (int)wa.right - W)); top = std::max((int)wa.top, top);
    if (dwm_border_) { COLORREF bc = high_contrast_ ? GetSysColor(COLOR_WINDOWTEXT) : (dark_ ? RGB(0x45, 0x45, 0x45) : RGB(0xE0, 0xE0, 0xE0)); DwmSetWindowAttribute(hwnd_, 34 /* DWMWA_BORDER_COLOR */, &bc, sizeof bc); }
    SetWindowPos(hwnd_, HWND_TOPMOST, x, top, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (!rt_) d2d_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), (float)dpi_, (float)dpi_),
                                           D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(W, H)), &rt_);
    else { rt_->SetDpi((float)dpi_, (float)dpi_); rt_->Resize(D2D1::SizeU(W, H)); }
}

void CandidateWindow::show(const aqtip::PanelView& v, const RECT& anchor) { anchor_ = anchor; update(v); }
void CandidateWindow::update(const aqtip::PanelView& v) {
    view_ = v;
    if (!view_.cands || view_.rows.empty()) { hide(); return; }
    if (!ensure()) return;
    layout(); InvalidateRect(hwnd_, nullptr, FALSE);
}
void CandidateWindow::hide() { if (hwnd_) ShowWindow(hwnd_, SW_HIDE); hover_ = -1; hover_button_ = 0; }

bool CandidateWindow::page_enabled(bool down) const {
    if (!view_.cands || view_.rows.empty() || view_.interim) return false;
    return down ? view_.rows.back() + 1 < (int)view_.cands->size() : view_.rows.front() > 0;
}

void CandidateWindow::paint() {
    if (!rt_ || !view_.cands) return;
    draw(rt_);
}
void CandidateWindow::draw(ID2D1RenderTarget* rt_) {
    const auto& C = *view_.cands; const Palette& P = dark_ ? kDark : kLight;
    D2D1_COLOR_F bg = P.bg, text = P.text, text2 = P.text2, text3 = P.text3, hi = P.hi, hover = P.hover, accent = P.accent, divider = P.divider;
    if (high_contrast_) { bg = sys(COLOR_WINDOW); text = text2 = text3 = divider = sys(COLOR_WINDOWTEXT); hi = sys(COLOR_HIGHLIGHT); hover = sys(COLOR_HIGHLIGHT); }
    rt_->BeginDraw(); rt_->Clear(bg);
    ID2D1SolidColorBrush* b = nullptr; rt_->CreateSolidColorBrush(text, &b); if (!b) { rt_->EndDraw(); return; }
    auto color = [&](D2D1_COLOR_F c, float a = 1.f) { c.a *= a; b->SetColor(c); return b; };
    if (!dwm_border_) rt_->DrawRectangle(D2D1::RectF(0.5f, 0.5f, width_ - 0.5f, height_ - 0.5f), color(P.border));   // before Windows 11
    const float left = kPad, right = width_ - kPad;
    for (const Row& r : rows_) {
        const auto& c = C[r.index]; const bool cur = r.index == view_.highlighted, hov = r.index == hover_ && !view_.interim;
        const D2D1_ROUNDED_RECT box = D2D1::RoundedRect(D2D1::RectF(left, r.top, right, r.top + r.height), kRowRadius, kRowRadius);
        if (cur) rt_->FillRoundedRectangle(box, color(hi));
        else if (hov) rt_->FillRoundedRectangle(box, color(hover));
        const bool hc_fill = high_contrast_ && cur;                                   // high contrast: the whole row, no bar
        if (cur && !high_contrast_) rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, r.top + kBarTop, left + kBarW, r.top + kBarTop + kBarH), 1.5f, 1.5f), color(view_.interim ? text3 : accent));
        const D2D1_COLOR_F tc = hc_fill ? sys(COLOR_HIGHLIGHTTEXT) : view_.interim ? text3 : text;
        const D2D1_COLOR_F nc = hc_fill ? sys(COLOR_HIGHLIGHTTEXT) : view_.interim ? text3 : cur ? text2 : text3;
        const float line_top = r.top + kRowPadV, num_l = left + kRowPadL, text_l = num_l + kNumW + kNumGap;
        const std::wstring num = std::to_wstring(r.index + 1);
        rt_->DrawText(num.c_str(), (UINT32)num.size(), num_fmt_, D2D1::RectF(num_l, line_top, num_l + kNumW, line_top + line_h()), color(nc));
        if (r.text) rt_->DrawTextLayout(D2D1::Point2F(text_l, line_top), r.text, color(tc));
        if (c.memory) rt_->DrawText(L"記憶", 2, note_fmt_, D2D1::RectF(right - kRowPadR - note_w_, line_top, right - kRowPadR, line_top + line_h()), color(hc_fill ? sys(COLOR_HIGHLIGHTTEXT) : text2));
    }
    for (float y : dividers_) rt_->FillRectangle(D2D1::RectF(left + kDividerInset, y, right - kDividerInset, y + 1), color(divider));
    if (footer_top_ >= 0) {
        const float ft = footer_top_, fb = ft + kFooterH;
        // the groups this list has, the highlighted one in bold: Tab moves between them
        float x = left + kFooterPadL; const int cur_group = (view_.highlighted >= 0 && view_.highlighted < (int)C.size()) ? C[view_.highlighted].group : -1; bool first = true;
        for (int g = 0; g < 4; g++) {
            if (!std::any_of(C.begin(), C.end(), [g](const aqtip::Candidate& c) { return c.group == g; })) continue;
            if (!first) { rt_->DrawText(L"·", 1, group_fmt_, D2D1::RectF(x + kDotGap, ft, x + kDotGap + 6, fb), color(text3)); x += kDotGap * 2 + 6; }
            const bool on = g == cur_group && !view_.interim;
            rt_->DrawText(group_name(g), 1, on ? group_bold_fmt_ : group_fmt_, D2D1::RectF(x, ft, x + 14, fb), color(on ? text : text3)); x += 14; first = false;
        }
        // the page: first–last / total, then the two page buttons
        const float bx_down = right - kFooterPadR - kButton, bx_up = bx_down - kButton;
        const std::wstring pg = std::to_wstring(view_.rows.front() + 1) + L"–" + std::to_wstring(view_.rows.back() + 1) + L" / " + std::to_wstring(C.size());
        IDWriteTextLayout* pl = nullptr; dw_->CreateTextLayout(pg.c_str(), (UINT32)pg.size(), num_fmt_, 200, kFooterH, &pl);
        if (pl) { pl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING); DWRITE_FONT_FEATURE tab{DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1}; IDWriteTypography* ty = nullptr;
            if (SUCCEEDED(dw_->CreateTypography(&ty))) { ty->AddFontFeature(tab); pl->SetTypography(ty, DWRITE_TEXT_RANGE{0, (UINT32)pg.size()}); ty->Release(); }
            rt_->DrawTextLayout(D2D1::Point2F(bx_up - kPageGap - 200, ft), pl, color(text2)); pl->Release(); }
        for (int k = 0; k < 2; k++) {
            const bool down = k == 1, en = page_enabled(down); const float bx = down ? bx_down : bx_up, by = ft + (kFooterH - kButton) / 2;
            if (en && hover_button_ == k + 1) rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bx, by, bx + kButton, by + kButton), 4, 4), color(hover));
            const wchar_t glyph = down ? L'\xE70D' : L'\xE70E';
            rt_->DrawText(&glyph, 1, icon_fmt_, D2D1::RectF(bx, by, bx + kButton, by + kButton), en ? color(text2) : color(text3, 0.5f));
        }
    }
    b->Release();
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET && rt_ == this->rt_) release(this->rt_);
}

bool CandidateWindow::render_png(const aqtip::PanelView& v, const wchar_t* path, UINT dpi, bool dark, int hover) {
    view_ = v; if (!view_.cands || view_.rows.empty() || !ensure_resources()) return false;
    dpi_ = dpi; dark_ = dark; high_contrast_ = false; dwm_border_ = false; hover_ = hover; measure();
    const float s = dpi / 96.f; const UINT W = (UINT)std::ceil(width_ * s), H = (UINT)std::ceil(height_ * s);
    IWICImagingFactory* wic = nullptr; IWICBitmap* bmp = nullptr; ID2D1RenderTarget* rt = nullptr; bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) && SUCCEEDED(wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))
        && SUCCEEDED(d2d_->CreateWicBitmapRenderTarget(bmp, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), (float)dpi, (float)dpi), &rt))) {
        draw(rt);
        IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
        if (SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path, GENERIC_WRITE)) && SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc))
            && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr))) {
            ok = SUCCEEDED(fr->WriteSource(bmp, nullptr)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
        }
        release(fr); release(enc); release(st);
    }
    release(rt); release(bmp); release(wic);
    return ok;
}

int CandidateWindow::row_at(float x, float y) const {
    if (x < kPad || x > width_ - kPad) return -1;
    for (const Row& r : rows_) if (y >= r.top && y < r.top + r.height) return r.index;
    return -1;
}
int CandidateWindow::button_at(float x, float y) const {
    if (footer_top_ < 0 || y < footer_top_ || y > footer_top_ + kFooterH) return 0;
    const float bx_down = width_ - kPad - kFooterPadR - kButton, bx_up = bx_down - kButton;
    if (x >= bx_down && x < bx_down + kButton) return 2; if (x >= bx_up && x < bx_up + kButton) return 1; return 0;
}

LRESULT CALLBACK CandidateWindow::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
    auto* self = (CandidateWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    const float s = self ? self->dpi_ / 96.f : 1.f;
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); if (self) self->paint(); EndPaint(h, &ps); return 0; }
        case WM_MOUSEMOVE: if (self) {
            if (!self->tracking_) { TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0}; TrackMouseEvent(&t); self->tracking_ = true; }
            const float x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s; const int row = self->row_at(x, y), btn = self->button_at(x, y);
            if (row != self->hover_ || btn != self->hover_button_) { self->hover_ = row; self->hover_button_ = btn; InvalidateRect(h, nullptr, FALSE); } }
            return 0;
        case WM_MOUSELEAVE: if (self) { self->tracking_ = false; self->hover_ = -1; self->hover_button_ = 0; InvalidateRect(h, nullptr, FALSE); } return 0;
        case WM_LBUTTONUP: if (self && !self->view_.interim) {
            const float x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s;
            if (const int btn = self->button_at(x, y)) { if (self->page_enabled(btn == 2) && self->on_page) self->on_page(btn == 2); return 0; }
            const int i = self->row_at(x, y); if (i >= 0 && self->on_select) self->on_select(i); }
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace aqtsf
