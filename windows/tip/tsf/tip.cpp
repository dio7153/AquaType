#include "tip.h"
#include <cstdarg>
#include <cstdio>
#include <inputscope.h>
#include <olectl.h>
#include <string>

namespace aqtsf {
namespace {
constexpr UINT WM_ENGINE = WM_APP + 1;
// GUID_PROP_INPUTSCOPE (msctf.h declares it; no import library defines it)
constexpr GUID kPropInputScope = {0x1713dd5a, 0x68e7, 0x4a5b, {0x9a, 0xf6, 0x59, 0x2a, 0x59, 0x5c, 0x77, 0x8d}};
const wchar_t kMsgClass[] = L"AquaTypeMessageWindow";

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {}; int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}

// The composer runs inside other people's apps: an exception (a malformed reply, an allocation failure) must end
// at the TSF boundary, never unwind into the app.
template <class F> void guard(F&& f) { try { f(); } catch (...) {} }

// an edit session that runs a function with the cookie it is given
class EditSession : public ITfEditSession {
public:
    explicit EditSession(std::function<void(TfEditCookie)> f) : f_(std::move(f)) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_ITfEditSession) { *ppv = static_cast<ITfEditSession*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&refs_); if (!r) delete this; return r; }
    STDMETHODIMP DoEditSession(TfEditCookie ec) override { guard([&] { f_(ec); }); return S_OK; }
private:
    std::function<void(TfEditCookie)> f_; LONG refs_ = 1;
};

// the composition's look: a thin solid underline in the app's own colours (design-spec.md §4)
class DisplayAttrInfo : public ITfDisplayAttributeInfo {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_ITfDisplayAttributeInfo) { *ppv = static_cast<ITfDisplayAttributeInfo*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    DisplayAttrInfo() { lock_module(); }
    ~DisplayAttrInfo() { unlock_module(); }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&refs_); if (!r) delete this; return r; }
    STDMETHODIMP GetGUID(GUID* g) override { *g = GUID_DisplayAttributeInput; return S_OK; }
    STDMETHODIMP GetDescription(BSTR* d) override { *d = SysAllocString(L"AquaType composition"); return *d ? S_OK : E_OUTOFMEMORY; }
    STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE* a) override { *a = attr(); return S_OK; }
    STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE*) override { return E_NOTIMPL; }
    STDMETHODIMP Reset() override { return S_OK; }
    static TF_DISPLAYATTRIBUTE attr() { TF_DISPLAYATTRIBUTE a{}; a.crText.type = TF_CT_NONE; a.crBk.type = TF_CT_NONE; a.lsStyle = TF_LS_SOLID; a.crLine.type = TF_CT_NONE; a.bAttr = TF_ATTR_INPUT; return a; }
private:
    LONG refs_ = 1;
};
class EnumDisplayAttrInfo : public IEnumTfDisplayAttributeInfo {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IEnumTfDisplayAttributeInfo) { *ppv = static_cast<IEnumTfDisplayAttributeInfo*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    EnumDisplayAttrInfo() { lock_module(); }
    ~EnumDisplayAttrInfo() { unlock_module(); }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&refs_); if (!r) delete this; return r; }
    STDMETHODIMP Clone(IEnumTfDisplayAttributeInfo** e) override { auto* c = new EnumDisplayAttrInfo(); c->pos_ = pos_; *e = c; return S_OK; }
    STDMETHODIMP Next(ULONG n, ITfDisplayAttributeInfo** out, ULONG* fetched) override {
        ULONG got = 0; if (n > 0 && pos_ == 0) { out[0] = new DisplayAttrInfo(); pos_ = 1; got = 1; }
        if (fetched) *fetched = got; return got == n ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override { pos_ = 0; return S_OK; }
    STDMETHODIMP Skip(ULONG n) override { if (n > 0 && pos_ == 0) { pos_ = 1; return n == 1 ? S_OK : S_FALSE; } return n ? S_FALSE : S_OK; }
private:
    LONG refs_ = 1; ULONG pos_ = 0;
};
}  // namespace

// What the composer asks of the text service, bound to the edit session in progress.  The text before the caret and
// the password-field test are read when a composition starts and kept: the composer may ask for them later, from a
// reply that arrives outside any edit session, and they do not change while the composition stays where it is.
struct TextService::HostImpl : aqtip::Host {
    TextService* ts; TfEditCookie ec = TF_INVALID_EDIT_COOKIE; ITfContext* ctx = nullptr;
    std::string context; bool secure = false;
    void set_preedit(const std::wstring& t, int caret) override { if (ctx && ec != TF_INVALID_EDIT_COOKIE) ts->set_preedit(ec, ctx, t, caret); }
    void commit(const std::wstring& t) override { if (ctx && ec != TF_INVALID_EDIT_COOKIE) ts->commit(ec, ctx, t); }
    void beep() override { MessageBeep(MB_OK); }
    std::string context_before_caret() override { return context; }
    bool secure_input() override { return secure; }
};

TextService::TextService() { lock_module(); }
TextService::~TextService() { unlock_module(); }

STDMETHODIMP TextService::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    if (riid == IID_IUnknown || riid == IID_ITfTextInputProcessor || riid == IID_ITfTextInputProcessorEx) *ppv = static_cast<ITfTextInputProcessorEx*>(this);
    else if (riid == IID_ITfThreadMgrEventSink) *ppv = static_cast<ITfThreadMgrEventSink*>(this);
    else if (riid == IID_ITfKeyEventSink) *ppv = static_cast<ITfKeyEventSink*>(this);
    else if (riid == IID_ITfCompositionSink) *ppv = static_cast<ITfCompositionSink*>(this);
    else if (riid == IID_ITfThreadFocusSink) *ppv = static_cast<ITfThreadFocusSink*>(this);
    else if (riid == IID_ITfDisplayAttributeProvider) *ppv = static_cast<ITfDisplayAttributeProvider*>(this);
    else if (riid == IID_ITfCompartmentEventSink) *ppv = static_cast<ITfCompartmentEventSink*>(this);
    else if (riid == IID_ITfFnConfigure || riid == IID_ITfFunction) *ppv = static_cast<ITfFnConfigure*>(this);
    else { *ppv = nullptr; return E_NOINTERFACE; }
    AddRef(); return S_OK;
}
STDMETHODIMP_(ULONG) TextService::AddRef() { return InterlockedIncrement(&refs_); }
STDMETHODIMP_(ULONG) TextService::Release() { LONG r = InterlockedDecrement(&refs_); if (!r) delete this; return r; }

// ---------------------------------------------------------------- activation
STDMETHODIMP TextService::ActivateEx(ITfThreadMgr* tm, TfClientId id, DWORD flags) {
    tm_ = tm; tm_->AddRef(); id_ = id; flags_ = flags;
    ITfSource* src = nullptr;
    if (SUCCEEDED(tm_->QueryInterface(IID_ITfSource, (void**)&src))) {
        src->AdviseSink(IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this), &tm_sink_);
        src->AdviseSink(IID_ITfThreadFocusSink, static_cast<ITfThreadFocusSink*>(this), &focus_sink_);
        src->Release();
    }
    ITfKeystrokeMgr* km = nullptr;
    if (SUCCEEDED(tm_->QueryInterface(IID_ITfKeystrokeMgr, (void**)&km))) {
        km->AdviseKeyEventSink(id_, static_cast<ITfKeyEventSink*>(this), TRUE);
        // Shift is watched in the key event sink itself, as Weasel does: a preserved Shift key (SampleIME's way) took
        // Shift away from the sink on Windows 11 and was itself never reported.
        km->Release(); }
    ITfCategoryMgr* cat = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr, (void**)&cat))) { cat->RegisterGUID(GUID_DisplayAttributeInput, &attr_atom_); cat->Release(); }
    // the keyboard starts open: this is a Chinese input method and nothing else turns it on
    ITfCompartmentMgr* cm = nullptr;
    if (SUCCEEDED(tm_->QueryInterface(IID_ITfCompartmentMgr, (void**)&cm))) { ITfCompartment* c = nullptr;
        if (SUCCEEDED(cm->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &c))) { VARIANT v; VariantInit(&v); v.vt = VT_I4; v.lVal = 1; c->SetValue(id_, &v); c->Release(); }
        cm->Release(); }

    WNDCLASSEXW wc{sizeof wc}; wc.lpfnWndProc = msg_proc; wc.hInstance = g_module; wc.lpszClassName = kMsgClass; RegisterClassExW(&wc);
    // a hidden top-level window rather than a message-only one: the theme change is broadcast only to top-level windows
    msg_hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kMsgClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_module, this);
    engine_ = std::make_unique<aqtip::PipeEngine>(aqtip::PipeEngine::default_pipe_name(), aqtip::default_launch(g_module));
    engine_->set_notify(msg_hwnd_, WM_ENGINE);
    host_ = std::make_unique<HostImpl>(); host_->ts = this; host_->secure = (flags & TF_TMAE_SECUREMODE) != 0;
    composer_ = std::make_unique<aqtip::Composer>(*host_, *engine_);
    settings_msg_ = RegisterWindowMessageW(aqtip::kSettingsChangedMessage);
    if (settings_msg_) ChangeWindowMessageFilterEx(msg_hwnd_, settings_msg_, MSGFLT_ALLOW, nullptr);   // the settings app may run at a higher integrity than this app
    reload_settings();
    panel_.on_select = [this](int i) { on_select(i); };
    panel_.on_page = [this](bool d) { on_page(d); };
    if (ITfCompartment* c = mode_compartment()) { ITfSource* s = nullptr;
        if (SUCCEEDED(c->QueryInterface(IID_ITfSource, (void**)&s))) { s->AdviseSink(IID_ITfCompartmentEventSink, static_cast<ITfCompartmentEventSink*>(this), &mode_sink_); s->Release(); }
        c->Release(); }
    mode_button_ = new InputModeButton();
    mode_button_->is_ascii = [this] { return composer_ && composer_->ascii_mode(); };
    mode_button_->on_click = [this] { toggle_mode(); };
    mode_button_->on_menu = [this](POINT p) { show_menu(p); };
    ITfLangBarItemMgr* lbm = nullptr;
    if (SUCCEEDED(tm_->QueryInterface(IID_ITfLangBarItemMgr, (void**)&lbm))) { lbm->AddItem(mode_button_); lbm->Release(); }
    sync_input_mode();
    return S_OK;
}

void TextService::reload_settings() {
    const aqtip::Settings s = aqtip::Settings::load();
    if (composer_) composer_->set_settings(s);
    panel_.set_text_size(s.candidate_text_size());
}

// as a Shift tap: going to English commits what is being composed first
void TextService::toggle_mode() {
    if (!composer_) return;
    if (composer_->composing() && comp_ctx_) { ITfContext* c = comp_ctx_; c->AddRef();
        run(c, [this, c](TfEditCookie ec) { host_->ec = ec; host_->ctx = c; composer_->shift_tapped(); after_edit(ec, c); sync_input_mode(); host_->ec = TF_INVALID_EDIT_COOKIE; host_->ctx = nullptr; }, false);
        c->Release(); }
    else { guard([&] { composer_->set_ascii(!composer_->ascii_mode()); }); sync_input_mode(); }
}

void TextService::show_menu(POINT at) {
    if (!msg_hwnd_ || !composer_) return;
    HMENU m = CreatePopupMenu(); if (!m) return;
    enum { kToggle = 1, kSettings = 2 };
    AppendMenuW(m, MF_STRING, kToggle, composer_->ascii_mode() ? L"切換到中文" : L"切換到英文");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kSettings, L"設定…");
    SetForegroundWindow(msg_hwnd_);                  // so the menu closes when the person clicks elsewhere
    const int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, at.x, at.y, msg_hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd == kToggle) toggle_mode();
    else if (cmd == kSettings) open_settings_app();
}

void open_settings_app() {
    wchar_t path[MAX_PATH]; if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return;
    std::wstring dir = path; dir = dir.substr(0, dir.find_last_of(L'\\'));
    std::wstring exe = dir + L"\\AquaTypeSettings.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) exe = dir.substr(0, dir.find_last_of(L'\\')) + L"\\AquaTypeSettings.exe";   // the 32-bit copy sits in x86
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si{sizeof si}; PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi)
        || CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}

STDMETHODIMP TextService::GetDisplayName(BSTR* name) { if (!name) return E_INVALIDARG; *name = SysAllocString(L"AquaType 設定"); return *name ? S_OK : E_OUTOFMEMORY; }
STDMETHODIMP TextService::Show(HWND, LANGID, REFGUID) { open_settings_app(); return S_OK; }

// The input mode compartment is what Windows shows as 中/英 on the taskbar, and clicking that indicator changes it.
ITfCompartment* TextService::mode_compartment() {
    ITfCompartmentMgr* cm = nullptr; ITfCompartment* c = nullptr;
    if (tm_ && SUCCEEDED(tm_->QueryInterface(IID_ITfCompartmentMgr, (void**)&cm))) { cm->GetCompartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &c); cm->Release(); }
    return c;
}
void TextService::sync_input_mode() {
    if (!composer_) return;
    ITfCompartment* c = mode_compartment(); if (!c) return;
    VARIANT v; VariantInit(&v); v.vt = VT_I4; v.lVal = composer_->ascii_mode() ? TF_CONVERSIONMODE_ALPHANUMERIC : TF_CONVERSIONMODE_NATIVE;
    setting_mode_ = true; c->SetValue(id_, &v); setting_mode_ = false; c->Release();
    if (mode_button_) mode_button_->update();
}
STDMETHODIMP TextService::OnChange(REFGUID g) {
    if (g != GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION || setting_mode_ || !composer_) return S_OK;
    ITfCompartment* c = mode_compartment(); if (!c) return S_OK;
    VARIANT v; VariantInit(&v);
    if (c->GetValue(&v) == S_OK && v.vt == VT_I4) { const bool ascii = (v.lVal & TF_CONVERSIONMODE_NATIVE) == 0; if (ascii != composer_->ascii_mode()) guard([&] { composer_->set_ascii(ascii); }); if (mode_button_) mode_button_->update(); }
    c->Release(); return S_OK;
}

STDMETHODIMP TextService::Deactivate() {
    if (comp_ && comp_ctx_) { ITfContext* c = comp_ctx_; c->AddRef(); run(c, [this, c](TfEditCookie ec) { host_->ec = ec; host_->ctx = c; composer_->focus_lost(); if (comp_) end_composition(ec); host_->ec = TF_INVALID_EDIT_COOKIE; host_->ctx = nullptr; }); c->Release(); }
    panel_.hide();
    if (mode_button_) { ITfLangBarItemMgr* lbm = nullptr; if (tm_ && SUCCEEDED(tm_->QueryInterface(IID_ITfLangBarItemMgr, (void**)&lbm))) { lbm->RemoveItem(mode_button_); lbm->Release(); }
        mode_button_->Release(); mode_button_ = nullptr; }
    if (mode_sink_ != TF_INVALID_COOKIE) { if (ITfCompartment* c = mode_compartment()) { ITfSource* s = nullptr; if (SUCCEEDED(c->QueryInterface(IID_ITfSource, (void**)&s))) { s->UnadviseSink(mode_sink_); s->Release(); } c->Release(); } mode_sink_ = TF_INVALID_COOKIE; }
    ITfSource* src = nullptr;
    if (tm_ && SUCCEEDED(tm_->QueryInterface(IID_ITfSource, (void**)&src))) {
        if (tm_sink_ != TF_INVALID_COOKIE) src->UnadviseSink(tm_sink_); if (focus_sink_ != TF_INVALID_COOKIE) src->UnadviseSink(focus_sink_); src->Release();
    }
    tm_sink_ = focus_sink_ = TF_INVALID_COOKIE;
    ITfKeystrokeMgr* km = nullptr;
    if (tm_ && SUCCEEDED(tm_->QueryInterface(IID_ITfKeystrokeMgr, (void**)&km))) { km->UnadviseKeyEventSink(id_); km->Release(); }
    composer_.reset(); host_.reset(); engine_.reset();
    if (msg_hwnd_) { DestroyWindow(msg_hwnd_); msg_hwnd_ = nullptr; }
    if (comp_) { comp_->Release(); comp_ = nullptr; } if (comp_ctx_) { comp_ctx_->Release(); comp_ctx_ = nullptr; }
    if (tm_) { tm_->Release(); tm_ = nullptr; } id_ = TF_CLIENTID_NULL;
    return S_OK;
}

// ---------------------------------------------------------------- keys
aqtip::Key TextService::make_key(WPARAM w) const {
    aqtip::Key k; k.vk = (uint32_t)w; k.shift = GetKeyState(VK_SHIFT) < 0; k.ctrl = GetKeyState(VK_CONTROL) < 0; k.alt = GetKeyState(VK_MENU) < 0; k.caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
    return k;
}
bool TextService::keyboard_open() {
    bool open = true; ITfCompartmentMgr* cm = nullptr;
    if (tm_ && SUCCEEDED(tm_->QueryInterface(IID_ITfCompartmentMgr, (void**)&cm))) { ITfCompartment* c = nullptr;
        if (SUCCEEDED(cm->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &c))) { VARIANT v; VariantInit(&v); if (c->GetValue(&v) == S_OK && v.vt == VT_I4) open = v.lVal != 0; c->Release(); }
        cm->Release(); }
    return open;
}
HRESULT TextService::run(ITfContext* ctx, const std::function<void(TfEditCookie)>& body, bool sync) {
    if (!ctx) return E_FAIL;
    EditSession* s = new EditSession(body); HRESULT hr = E_FAIL;
    ctx->RequestEditSession(id_, s, TF_ES_READWRITE | (sync ? TF_ES_SYNC : TF_ES_ASYNCDONTCARE), &hr);
    s->Release(); return hr;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext* ctx, WPARAM w, LPARAM, BOOL* eaten) {
    *eaten = FALSE;
    if (w == VK_SHIFT || w == VK_LSHIFT || w == VK_RSHIFT) { shift_down(); return S_OK; }
    if (GetKeyState(VK_SHIFT) < 0) other_key_with_shift_ = true;          // Shift+something: not a Shift tap
    if (!composer_ || !keyboard_open()) return S_OK;
    guard([&] { composer_->pump(); *eaten = composer_->wants(make_key(w)) ? TRUE : FALSE; });
    return S_OK;
}
STDMETHODIMP TextService::OnKeyDown(ITfContext* ctx, WPARAM w, LPARAM l, BOOL* eaten) {
    *eaten = FALSE;
    if (w == VK_SHIFT || w == VK_LSHIFT || w == VK_RSHIFT) { shift_down(); return S_OK; }   // apps that skip OnTestKeyDown
    if (GetKeyState(VK_SHIFT) < 0) other_key_with_shift_ = true;
    if (!composer_ || !keyboard_open()) return S_OK;
    const aqtip::Key k = make_key(w);
    bool wants = false; guard([&] { wants = composer_->wants(k); });
    if (!wants) return S_OK;
    aqtip::Result r = aqtip::Result::Pass;
    run(ctx, [&](TfEditCookie ec) {
        host_->ec = ec; host_->ctx = ctx;
        if (!composer_->composing()) { host_->context = context_before_caret(ec, ctx); host_->secure = (flags_ & TF_TMAE_SECUREMODE) || secure_input(ec, ctx); }
        r = composer_->key(k);
        after_edit(ec, ctx); sync_input_mode();
        host_->ec = TF_INVALID_EDIT_COOKIE; host_->ctx = nullptr;
    });
    *eaten = r == aqtip::Result::Eaten ? TRUE : FALSE;
    return S_OK;
}
// Shift down then up with nothing in between, within half a second: a tap, which switches 中/英 (going to English commits
// the composition first, as on the Mac).  Shift used with another key - a capital, a symbol, Shift+click - is not.
// Apps differ in which of the test / real key calls they make, so both report here and a tap is taken once.
void TextService::shift_down() {
    if (shift_held_) return;                                  // auto-repeat
    shift_held_ = true; shift_down_at_ = GetTickCount64(); other_key_with_shift_ = false;
}
bool TextService::shift_up(ITfContext* ctx) {
    if (!shift_held_) return false;
    shift_held_ = false;
    const ULONGLONG held = GetTickCount64() - shift_down_at_;
    const bool tap = composer_ && composer_->settings().shift_toggle && !other_key_with_shift_ && held < 500 && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_MENU) >= 0 && GetKeyState(VK_LBUTTON) >= 0;
    if (!tap || !composer_ || !keyboard_open()) return false;
    ITfContext* c = ctx ? ctx : comp_ctx_;
    if (c) { c->AddRef();
        run(c, [this, c](TfEditCookie ec) { host_->ec = ec; host_->ctx = c; composer_->shift_tapped(); after_edit(ec, c); sync_input_mode(); host_->ec = TF_INVALID_EDIT_COOKIE; host_->ctx = nullptr; }, false);
        c->Release(); }
    else { guard([&] { composer_->shift_tapped(); }); sync_input_mode(); }
    return true;
}
STDMETHODIMP TextService::OnTestKeyUp(ITfContext* ctx, WPARAM w, LPARAM, BOOL* eaten) {
    *eaten = FALSE;
    if (w == VK_SHIFT || w == VK_LSHIFT || w == VK_RSHIFT) shift_up(ctx);
    return S_OK;
}
STDMETHODIMP TextService::OnPreservedKey(ITfContext*, REFGUID, BOOL* eaten) { *eaten = FALSE; return S_OK; }STDMETHODIMP TextService::OnKeyUp(ITfContext* ctx, WPARAM w, LPARAM, BOOL* eaten) {
    *eaten = FALSE;
    if (w == VK_SHIFT || w == VK_LSHIFT || w == VK_RSHIFT) shift_up(ctx);   // apps that skip OnTestKeyUp
    return S_OK;
}

// ---------------------------------------------------------------- the composition
void TextService::start_composition(TfEditCookie ec, ITfContext* ctx) {
    ITfInsertAtSelection* ins = nullptr; ITfRange* range = nullptr; ITfContextComposition* cc = nullptr;
    if (FAILED(ctx->QueryInterface(IID_ITfInsertAtSelection, (void**)&ins))) return;
    if (SUCCEEDED(ins->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr, 0, &range)) && SUCCEEDED(ctx->QueryInterface(IID_ITfContextComposition, (void**)&cc))) {
        if (SUCCEEDED(cc->StartComposition(ec, range, static_cast<ITfCompositionSink*>(this), &comp_)) && comp_) { comp_ctx_ = ctx; comp_ctx_->AddRef(); }
        cc->Release();
    }
    if (range) range->Release(); ins->Release();
}
void TextService::end_composition(TfEditCookie ec) {
    if (!comp_) return;
    comp_->EndComposition(ec); comp_->Release(); comp_ = nullptr;
    if (comp_ctx_) { comp_ctx_->Release(); comp_ctx_ = nullptr; }
}
static void caret_to_end(TfEditCookie ec, ITfContext* ctx, ITfRange* range, int at = -1) {   // at: UTF-16 units from the start; -1 the end
    ITfRange* r = nullptr; if (FAILED(range->Clone(&r))) return;
    if (at < 0) r->Collapse(ec, TF_ANCHOR_END);
    else { LONG moved = 0; r->Collapse(ec, TF_ANCHOR_START); r->ShiftEnd(ec, at, &moved, nullptr); r->Collapse(ec, TF_ANCHOR_END); } TF_SELECTION sel{}; sel.range = r; sel.style.ase = TF_AE_NONE; sel.style.fInterimChar = FALSE;
    ctx->SetSelection(ec, 1, &sel); r->Release();
}
void TextService::apply_attribute(TfEditCookie ec, ITfContext* ctx, ITfRange* range) {
    if (attr_atom_ == TF_INVALID_GUIDATOM) return;
    ITfProperty* prop = nullptr; if (FAILED(ctx->GetProperty(GUID_PROP_ATTRIBUTE, &prop))) return;
    VARIANT v; VariantInit(&v); v.vt = VT_I4; v.lVal = (LONG)attr_atom_; prop->SetValue(ec, range, &v); prop->Release();
}
void TextService::set_preedit(TfEditCookie ec, ITfContext* ctx, const std::wstring& text, int caret) {
    if (text.empty()) {
        if (comp_) { ITfRange* r = nullptr; if (SUCCEEDED(comp_->GetRange(&r))) { r->SetText(ec, 0, L"", 0); r->Release(); } end_composition(ec); }
        return;
    }
    if (!comp_) start_composition(ec, ctx);
    if (!comp_) return;
    ITfRange* r = nullptr; if (FAILED(comp_->GetRange(&r))) return;
    r->SetText(ec, 0, text.c_str(), (LONG)text.size()); apply_attribute(ec, ctx, r); caret_to_end(ec, ctx, r, caret); r->Release();
}
void TextService::commit(TfEditCookie ec, ITfContext* ctx, const std::wstring& text) {
    if (comp_) {
        ITfRange* r = nullptr;
        if (SUCCEEDED(comp_->GetRange(&r))) {
            r->SetText(ec, 0, text.c_str(), (LONG)text.size());
            ITfProperty* prop = nullptr; if (SUCCEEDED(ctx->GetProperty(GUID_PROP_ATTRIBUTE, &prop))) { prop->Clear(ec, r); prop->Release(); }
            caret_to_end(ec, ctx, r); r->Release();
        }
        end_composition(ec); return;
    }
    ITfInsertAtSelection* ins = nullptr; if (FAILED(ctx->QueryInterface(IID_ITfInsertAtSelection, (void**)&ins))) return;
    ITfRange* r = nullptr; if (SUCCEEDED(ins->InsertTextAtSelection(ec, 0, text.c_str(), (LONG)text.size(), &r)) && r) { caret_to_end(ec, ctx, r); r->Release(); }
    ins->Release();
}
STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie ec, ITfComposition* comp) {
    // The app ended the composition (a click elsewhere, a save): what is there is the bopomofo echo.  Write the
    // composer's answer over it, then let the composition go.
    if (comp_ && comp == comp_ && composer_ && composer_->composing()) {
        ITfContext* c = comp_ctx_; host_->ec = ec; host_->ctx = c;
        guard([&] { composer_->focus_lost(); });
        host_->ec = TF_INVALID_EDIT_COOKIE; host_->ctx = nullptr;
    }
    if (comp_ && comp == comp_) { comp_->Release(); comp_ = nullptr; if (comp_ctx_) { comp_ctx_->Release(); comp_ctx_ = nullptr; } }
    panel_.hide();
    return S_OK;
}

// ---------------------------------------------------------------- reading the document
std::string TextService::context_before_caret(TfEditCookie ec, ITfContext* ctx) {
    TF_SELECTION sel{}; ULONG n = 0;
    if (FAILED(ctx->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &n)) || n == 0 || !sel.range) return "";
    std::string out; ITfRange* r = nullptr;
    if (SUCCEEDED(sel.range->Clone(&r))) {
        r->Collapse(ec, TF_ANCHOR_START); LONG moved = 0;
        if (SUCCEEDED(r->ShiftStart(ec, -256, &moved, nullptr)) && moved < 0) {
            wchar_t buf[257]; ULONG got = 0;
            if (SUCCEEDED(r->GetText(ec, 0, buf, 256, &got)) && got == (ULONG)(-moved)) out = narrow(std::wstring(buf, got));   // a short or adjusted read is no context
        }
        r->Release();
    }
    sel.range->Release();
    return out;
}
bool TextService::secure_input(TfEditCookie ec, ITfContext* ctx) {
    bool secure = false; ITfReadOnlyProperty* prop = nullptr;
    if (FAILED(ctx->GetAppProperty(kPropInputScope, &prop)) && FAILED(ctx->GetProperty(kPropInputScope, (ITfProperty**)&prop))) return false;
    TF_SELECTION sel{}; ULONG n = 0;
    if (SUCCEEDED(ctx->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel, &n)) && n && sel.range) {
        VARIANT v; VariantInit(&v);
        if (SUCCEEDED(prop->GetValue(ec, sel.range, &v)) && v.vt == VT_UNKNOWN && v.punkVal) {
            ITfInputScope* is = nullptr;
            if (SUCCEEDED(v.punkVal->QueryInterface(IID_ITfInputScope, (void**)&is))) { InputScope* scopes = nullptr; UINT count = 0;
                if (SUCCEEDED(is->GetInputScopes(&scopes, &count))) { for (UINT i = 0; i < count; i++) if (scopes[i] == IS_PASSWORD || scopes[i] == IS_PRIVATE) secure = true; CoTaskMemFree(scopes); }
                is->Release(); }
        }
        VariantClear(&v); sel.range->Release();
    }
    prop->Release(); return secure;
}

// ---------------------------------------------------------------- the panel
void TextService::after_edit(TfEditCookie ec, ITfContext* ctx) {
    if (!composer_ || !composer_->panel_visible()) { panel_.hide(); return; }
    ITfContextView* view = nullptr; ITfRange* r = nullptr; bool placed = false;
    if (comp_ && SUCCEEDED(comp_->GetRange(&r)) && SUCCEEDED(ctx->GetActiveView(&view))) {
        RECT rc{}; BOOL clipped = FALSE; int off = 0, len = 0;
        if (composer_->target_span(&off, &len)) {                   // 整句直出: the list belongs to one character; line up with it
            ITfRange* t = nullptr; LONG moved = 0;
            if (SUCCEEDED(r->Clone(&t))) { t->Collapse(ec, TF_ANCHOR_START); t->ShiftEnd(ec, off + len, &moved, nullptr); t->ShiftStart(ec, off, &moved, nullptr); r->Release(); r = t; }
        }
        if (view->GetTextExt(ec, r, &rc, &clipped) == S_OK && (rc.right > rc.left || rc.bottom > rc.top)) { anchor_ = rc; placed = true; }
    }
    if (view) view->Release(); if (r) r->Release();
    if (!placed && !have_anchor_) {                                  // no layout from the app: the system caret
        GUITHREADINFO gi{sizeof gi};
        if (GetGUIThreadInfo(GetCurrentThreadId(), &gi) && gi.hwndCaret) { POINT p{gi.rcCaret.left, gi.rcCaret.bottom}; ClientToScreen(gi.hwndCaret, &p); anchor_ = RECT{p.x, p.y - (gi.rcCaret.bottom - gi.rcCaret.top), p.x + 1, p.y}; placed = true; }
    }
    have_anchor_ = have_anchor_ || placed;
    panel_.show(composer_->panel(), anchor_);
}
void TextService::on_engine() {
    if (!composer_) return;
    bool changed = false; guard([&] { changed = composer_->pump(); });
    if (changed && composer_->sentence_mode() && composer_->composing() && comp_ctx_) {   // the sentence itself changed: rewrite the composition
        ITfContext* c = comp_ctx_; c->AddRef();
        run(c, [this, c](TfEditCookie ec) { host_->ec = ec; host_->ctx = c; composer_->refresh_preedit(); after_edit(ec, c); host_->ec = TF_INVALID_EDIT_COOKIE; host_->ctx = nullptr; }, false);
        c->Release(); return;
    }
    guard([&] { if (composer_->panel_visible()) panel_.update(composer_->panel()); else panel_.hide(); });
}
void TextService::on_select(int index) {
    if (!comp_ctx_ || !composer_) return;
    ITfContext* c = comp_ctx_; c->AddRef();
    run(c, [this, c, index](TfEditCookie ec) { host_->ec = ec; host_->ctx = c; composer_->select(index); after_edit(ec, c); host_->ec = TF_INVALID_EDIT_COOKIE; host_->ctx = nullptr; }, false);
    c->Release();
}
void TextService::on_page(bool down) { if (composer_) guard([&] { composer_->page(down); panel_.update(composer_->panel()); }); }

LRESULT CALLBACK TextService::msg_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
    if (m == WM_ENGINE) { if (auto* self = (TextService*)GetWindowLongPtrW(h, GWLP_USERDATA)) self->on_engine(); return 0; }
    if (auto* self = (TextService*)GetWindowLongPtrW(h, GWLP_USERDATA); self && self->settings_msg_ && m == self->settings_msg_) { self->reload_settings(); return 0; }
    if (m == WM_SETTINGCHANGE && l && lstrcmpW((LPCWSTR)l, L"ImmersiveColorSet") == 0) {   // light/dark changed: the taskbar icon follows
        if (auto* self = (TextService*)GetWindowLongPtrW(h, GWLP_USERDATA)) if (self->mode_button_) self->mode_button_->update(); }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- focus
STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr*, ITfDocumentMgr*) {
    // Another field: what was being composed in the last one is committed there.
    if (comp_ && comp_ctx_ && composer_ && composer_->composing()) {
        ITfContext* c = comp_ctx_; c->AddRef();
        run(c, [this, c](TfEditCookie ec) { host_->ec = ec; host_->ctx = c; composer_->focus_lost(); if (comp_) end_composition(ec); host_->ec = TF_INVALID_EDIT_COOKIE; host_->ctx = nullptr; }, false);
        c->Release();
    }
    if (composer_) { composer_->reset_session_mode(); sync_input_mode(); }
    panel_.hide(); have_anchor_ = false;
    return S_OK;
}
STDMETHODIMP TextService::OnKillThreadFocus() { panel_.hide(); return S_OK; }

// ---------------------------------------------------------------- display attributes
STDMETHODIMP TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** e) { if (!e) return E_INVALIDARG; *e = new EnumDisplayAttrInfo(); return S_OK; }
STDMETHODIMP TextService::GetDisplayAttributeInfo(REFGUID g, ITfDisplayAttributeInfo** info) {
    if (!info) return E_INVALIDARG; *info = nullptr;
    if (g != GUID_DisplayAttributeInput) return E_INVALIDARG;
    *info = new DisplayAttrInfo(); return S_OK;
}

}  // namespace aqtsf
