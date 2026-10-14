#include "candwin.h"
#include <dwmapi.h>
#include <wincodec.h>
#include <windowsx.h>
#include <algorithm>
#include <string>
#include <unordered_map>

namespace aqtsf {
namespace {
const wchar_t kClass[] = L"AquaTypeCandidateWindow";
// AquaType's panel (packaging/payload/rime/squirrel.custom.yaml on the Mac, measured on its README's recording), in DIPs:
// 11 around, a candidate every 25 (15 text), the capsule 32 tall reaching 3.5 past its row, numbers and notes at 12
// right against the text.  Direct2D scales them to the monitor.
constexpr float kEdgeH = 11, kEdgeTop = 11, kEdgeBottom = 14, kRowPadV = 2.5f, kCapsuleOut = 3.5f, kCapsuleR = 16, kRowPadL = 4, kRowPadR = 16,
                kNumW = 14, kNumGap = 5, kNoteGap = 12, kMinW = 120;
D2D1_COLOR_F hex(UINT32 v, float a = 1.f) { return D2D1::ColorF(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, a); }
// One scheme for light and dark, as on the Mac: white, near-black text, grey numbers, Apple's blue capsule with white text.
const D2D1_COLOR_F kBack = hex(0xFFFFFF), kText = hex(0x1D1D1F), kLabel = hex(0x8E8E93), kHilite = hex(0x007AFF), kHiliteText = hex(0xFFFFFF), kInterim = hex(0xB0B0B5);
D2D1_COLOR_F sys(int idx) { COLORREF c = GetSysColor(idx); return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f); }
bool high_contrast() { HIGHCONTRASTW hc{sizeof hc}; return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON); }
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
bool has_family(IDWriteFactory* dw, const wchar_t* name) {
    IDWriteFontCollection* fc = nullptr; UINT32 i = 0; BOOL exists = FALSE;
    if (SUCCEEDED(dw->GetSystemFontCollection(&fc))) { fc->FindFamilyName(name, &i, &exists); fc->Release(); }
    return exists != FALSE;
}
UINT monitor_dpi(HMONITOR mon) {
    UINT dx = 96, dy = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static auto get_dpi = (GetDpiForMonitorFn)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor");
    if (get_dpi) get_dpi(mon, 0, &dx, &dy);
    return dx;
}
}  // namespace

// The Mac panel's wrap width: a share of the screen that grows with the text, 1/3 + size/144 (about 44% at 15).
float CandidateWindow::text_max_width(const RECT& anchor, float text_size) {
    HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST); MONITORINFO mi{sizeof mi}; GetMonitorInfoW(mon, &mi);
    const float screen = (mi.rcMonitor.right - mi.rcMonitor.left) * 96.f / monitor_dpi(mon);
    return std::max(8 * text_size, screen * std::min(1.f, 1.f / 3 + text_size / 144) - 2 * kEdgeH);
}
int CandidateWindow::columns(const RECT& anchor, float text_size) { return std::max(8, (int)(text_max_width(anchor, text_size) / text_size)); }

// Each character is looked up once among the installed fonts (the answer is kept); a candidate shows only if all of its
// characters have one.  Rare characters of the extension planes are the usual ones without.
bool CandidateWindow::drawable(const std::wstring& text) {
    static std::unordered_map<UINT32, bool> known; static IDWriteFontCollection* fonts = nullptr; static bool tried = false;
    if (!tried) { tried = true; IDWriteFactory* dw = nullptr;
        if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw))) { dw->GetSystemFontCollection(&fonts); dw->Release(); } }
    if (!fonts) return true;
    for (size_t i = 0; i < text.size(); i++) {
        UINT32 cp = text[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < text.size()) { cp = 0x10000 + ((cp - 0xD800) << 10) + (text[i + 1] - 0xDC00); i++; }
        if (cp < 0x2E80) continue;                                    // below the CJK blocks every font set has it
        auto it = known.find(cp);
        if (it == known.end()) {
            bool has = false;
            for (UINT32 f = 0, n = fonts->GetFontFamilyCount(); f < n && !has; f++) {
                IDWriteFontFamily* fam = nullptr; if (FAILED(fonts->GetFontFamily(f, &fam))) continue;
                IDWriteFont* font = nullptr; BOOL b = FALSE;
                if (SUCCEEDED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font))) { font->HasCharacter(cp, &b); font->Release(); }
                fam->Release(); has = b != FALSE;
            }
            it = known.emplace(cp, has).first;
        }
        if (!it->second) return false;
    }
    return true;
}

CandidateWindow::~CandidateWindow() {
    release_rows();
    release(text_fmt_); release(num_fmt_); release(note_fmt_);
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
    auto fmt = [&](const wchar_t* fam, float size, IDWriteTextFormat** out) { dw_->CreateTextFormat(fam, nullptr, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-tw", out); };
    fmt(cjk, text_size_, &text_fmt_); fmt(num, 12, &num_fmt_); fmt(cjk, 12, &note_fmt_);
    if (!text_fmt_ || !num_fmt_ || !note_fmt_) return false;
    text_fmt_->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line_h(), line_h() * 0.8f);
    text_fmt_->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER);
    for (IDWriteTextFormat* f : {num_fmt_, note_fmt_}) { f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    num_fmt_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    return true;
}

bool CandidateWindow::ensure() {
    if (hwnd_) return true;
    if (!ensure_resources()) return false;
    WNDCLASSEXW wc{sizeof wc}; wc.style = CS_DROPSHADOW; wc.lpfnWndProc = proc; wc.hInstance = g_module;   // the soft shadow macOS gives the panel
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = kClass;
    RegisterClassExW(&wc);                                   // once per process; a second call fails harmlessly
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_module, this);
    if (!hwnd_) return false;
    const DWORD round = 2 /* DWMWCP_ROUND: Windows 11's 8 px, the nearest it offers to the Mac's 14 */;
    dwm_border_ = SUCCEEDED(DwmSetWindowAttribute(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round));   // fails before Windows 11: square, own border
    return true;
}

void CandidateWindow::measure() {
    const auto& C = *view_.cands;
    // the text column: as wide as the widest candidate on this page, at most the Mac's share of the screen (then it wraps)
    release_rows(); text_w_ = 0; note_w_ = 0;
    for (int idx : view_.rows) {
        Row r{idx, 0, 0, nullptr};
        dw_->CreateTextLayout(C[idx].wtext.c_str(), (UINT32)C[idx].wtext.size(), text_fmt_, text_max_w_, 1000, &r.text);
        if (r.text) { DWRITE_TEXT_METRICS m{}; r.text->GetMetrics(&m); text_w_ = std::max(text_w_, std::min(text_max_w_, m.widthIncludingTrailingWhitespace)); r.height = std::max(row_min_h(), m.height + 2 * kRowPadV); }
        else r.height = row_min_h();
        if (C[idx].memory) note_w_ = 24;                     // 「記憶」 at 12
        rows_.push_back(r);
    }
    for (auto& r : rows_) if (r.text) r.text->SetMaxWidth(std::max(text_w_, 1.f));
    float y = kEdgeTop + kCapsuleOut;
    for (size_t i = 0; i < rows_.size(); i++) { rows_[i].top = y; y += rows_[i].height; }
    height_ = y + kCapsuleOut + kEdgeBottom;
    width_ = std::max(kMinW, kEdgeH + kRowPadL + kNumW + kNumGap + text_w_ + (note_w_ > 0 ? kNoteGap + note_w_ : 0) + kRowPadR + kEdgeH);
}

void CandidateWindow::layout() {
    HMONITOR mon = MonitorFromRect(&anchor_, MONITOR_DEFAULTTONEAREST);
    dpi_ = monitor_dpi(mon); high_contrast_ = high_contrast(); text_max_w_ = text_max_width(anchor_, text_size_);
    measure();
    // position: the candidates' left edge under the composition's start, below it (or above when there is no room); the
    // side is chosen when the composition starts and kept, so the panel does not jump while it grows
    const float s = dpi_ / 96.f; const int W = (int)std::ceil(width_ * s), H = (int)std::ceil(height_ * s);
    MONITORINFO mi{sizeof mi}; GetMonitorInfoW(mon, &mi); const RECT& wa = mi.rcWork;
    int x = anchor_.left - (int)std::lround((1 + kEdgeH + kRowPadL + kNumW + kNumGap) * s);
    const int below = anchor_.bottom + (int)std::lround(4 * s), above = anchor_.top - (int)std::lround(4 * s) - H;
    if (side_ == 0) side_ = below + H <= wa.bottom ? 1 : -1;
    int top = side_ > 0 ? below : above;
    if (side_ > 0 && top + H > wa.bottom) top = above; else if (side_ < 0 && top < wa.top) top = below;
    x = std::max((int)wa.left, std::min(x, (int)wa.right - W)); top = std::max((int)wa.top, std::min(top, (int)wa.bottom - H));
    if (dwm_border_) { COLORREF bc = high_contrast_ ? GetSysColor(COLOR_WINDOWTEXT) : RGB(0xD8, 0xD8, 0xDC); DwmSetWindowAttribute(hwnd_, 34 /* DWMWA_BORDER_COLOR */, &bc, sizeof bc); }
    SetWindowPos(hwnd_, HWND_TOPMOST, x, top, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (!rt_) d2d_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), (float)dpi_, (float)dpi_),
                                           D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(W, H)), &rt_);
    else { rt_->SetDpi((float)dpi_, (float)dpi_); rt_->Resize(D2D1::SizeU(W, H)); }
}

void CandidateWindow::show(const aqtip::PanelView& v, const RECT& anchor) {
    if (!visible() || anchor.left != anchor_.left || anchor.top != anchor_.top) side_ = 0;   // a new composition chooses its side again
    anchor_ = anchor; update(v);
}
void CandidateWindow::update(const aqtip::PanelView& v) {
    view_ = v;
    if (!view_.cands || view_.rows.empty()) { hide(); return; }
    if (!ensure()) return;
    layout(); InvalidateRect(hwnd_, nullptr, FALSE);
}
void CandidateWindow::hide() { if (hwnd_) ShowWindow(hwnd_, SW_HIDE); side_ = 0; }

void CandidateWindow::paint() {
    if (!rt_ || !view_.cands) return;
    draw(rt_);
}
void CandidateWindow::draw(ID2D1RenderTarget* rt_) {
    const auto& C = *view_.cands;
    D2D1_COLOR_F bg = kBack, text = kText, label = kLabel, hi = kHilite, hi_text = kHiliteText;
    if (high_contrast_) { bg = sys(COLOR_WINDOW); text = label = sys(COLOR_WINDOWTEXT); hi = sys(COLOR_HIGHLIGHT); hi_text = sys(COLOR_HIGHLIGHTTEXT); }
    rt_->BeginDraw(); rt_->Clear(bg);
    ID2D1SolidColorBrush* b = nullptr; rt_->CreateSolidColorBrush(text, &b); if (!b) { rt_->EndDraw(); return; }
    auto color = [&](D2D1_COLOR_F c) { b->SetColor(c); return b; };
    if (!dwm_border_) rt_->DrawRectangle(D2D1::RectF(0.5f, 0.5f, width_ - 0.5f, height_ - 0.5f), color(hex(0xD8D8DC)));   // before Windows 11
    const float left = kEdgeH, right = width_ - kEdgeH;
    for (const Row& r : rows_) {
        const auto& c = C[r.index]; const bool cur = r.index == view_.highlighted && !view_.interim;
        if (cur) { const float top = r.top - kCapsuleOut, bottom = r.top + r.height + kCapsuleOut, rad = std::min(kCapsuleR, (bottom - top) / 2);
                   rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, top, right, bottom), rad, rad), color(hi)); }
        // an answer for fewer keys than typed is shown greyed and cannot be picked
        const D2D1_COLOR_F tc = view_.interim ? kInterim : cur ? hi_text : text, nc = view_.interim ? kInterim : cur ? hi_text : label;
        const float line_top = r.top + kRowPadV, num_l = left + kRowPadL, text_l = num_l + kNumW + kNumGap;
        const std::wstring num = std::to_wstring(r.index + 1);
        rt_->DrawText(num.c_str(), (UINT32)num.size(), num_fmt_, D2D1::RectF(num_l, line_top, num_l + kNumW, line_top + line_h()), color(nc));
        if (r.text) rt_->DrawTextLayout(D2D1::Point2F(text_l, line_top), r.text, color(tc));
        if (c.memory) rt_->DrawText(L"記憶", 2, note_fmt_, D2D1::RectF(right - kRowPadR - note_w_, line_top, right - kRowPadR, line_top + line_h()), color(nc));
    }
    b->Release();
    if (rt_->EndDraw() == D2DERR_RECREATE_TARGET && rt_ == this->rt_) release(this->rt_);
}

bool CandidateWindow::render_png(const aqtip::PanelView& v, const wchar_t* path, UINT dpi, bool dark, int hover) {
    (void)dark; (void)hover;                                  // one scheme for both; the highlight follows the pointer itself
    view_ = v; if (!view_.cands || view_.rows.empty() || !ensure_resources()) return false;
    dpi_ = dpi; high_contrast_ = false; dwm_border_ = false;
    RECT screen{0, 0, 1920, 1080}; text_max_w_ = text_max_width(screen, text_size_); measure();
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
    if (x < kEdgeH || x > width_ - kEdgeH) return -1;
    for (const Row& r : rows_) if (y >= r.top && y < r.top + r.height) return r.index;
    return -1;
}

LRESULT CALLBACK CandidateWindow::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
    auto* self = (CandidateWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    const float s = self ? self->dpi_ / 96.f : 1.f;
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); if (self) self->paint(); EndPaint(h, &ps); if (self && self->on_painted) self->on_painted(); return 0; }
        case WM_MOUSEMOVE: if (self && !self->view_.interim) {               // the highlight follows the pointer, as on the Mac
            const int row = self->row_at(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s);
            if (row >= 0 && row != self->view_.highlighted && self->on_hover) self->on_hover(row); }
            return 0;
        case WM_MOUSEWHEEL: if (self && !self->view_.interim && self->on_page) {   // a notch turns a page
            self->wheel_ += GET_WHEEL_DELTA_WPARAM(w);
            while (self->wheel_ <= -WHEEL_DELTA) { self->wheel_ += WHEEL_DELTA; self->on_page(true); }
            while (self->wheel_ >= WHEEL_DELTA) { self->wheel_ -= WHEEL_DELTA; self->on_page(false); } }
            return 0;
        case WM_LBUTTONUP: if (self && !self->view_.interim) {
            const int i = self->row_at(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s); if (i >= 0 && self->on_select) self->on_select(i); }
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace aqtsf
