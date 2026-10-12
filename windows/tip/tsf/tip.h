// The text service: TSF on one side, the composer (windows/tip/composer.h) on the other.  Keys arrive through the
// key event sink; each key the composer takes runs inside a synchronous edit session, where the host below turns its
// effects into composition and insertion calls.  Engine replies arrive on a hidden window and only repaint the panel.
#pragma once
#include <memory>
#include "globals.h"
#include <ctffunc.h>            // ITfFnConfigure
#include "candwin.h"
#include "composer.h"
#include "engine_link.h"
#include "langbar.h"

namespace aqtsf {

class TextService : public ITfTextInputProcessorEx, public ITfThreadMgrEventSink, public ITfKeyEventSink, public ITfCompositionSink,
                    public ITfThreadFocusSink, public ITfDisplayAttributeProvider, public ITfCompartmentEventSink, public ITfFnConfigure {
public:
    TextService();
    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    // ITfTextInputProcessor(Ex)
    STDMETHODIMP Activate(ITfThreadMgr* tm, TfClientId id) override { return ActivateEx(tm, id, 0); }
    STDMETHODIMP ActivateEx(ITfThreadMgr* tm, TfClientId id, DWORD flags) override;
    STDMETHODIMP Deactivate() override;
    // ITfThreadMgrEventSink
    STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    STDMETHODIMP OnSetFocus(ITfDocumentMgr* focus, ITfDocumentMgr* prev) override;
    STDMETHODIMP OnPushContext(ITfContext*) override { return S_OK; }
    STDMETHODIMP OnPopContext(ITfContext*) override { return S_OK; }
    // ITfKeyEventSink
    STDMETHODIMP OnSetFocus(BOOL) override { return S_OK; }
    STDMETHODIMP OnTestKeyDown(ITfContext* ctx, WPARAM w, LPARAM l, BOOL* eaten) override;
    STDMETHODIMP OnKeyDown(ITfContext* ctx, WPARAM w, LPARAM l, BOOL* eaten) override;
    STDMETHODIMP OnTestKeyUp(ITfContext* ctx, WPARAM w, LPARAM l, BOOL* eaten) override;
    STDMETHODIMP OnKeyUp(ITfContext* ctx, WPARAM w, LPARAM l, BOOL* eaten) override;
    STDMETHODIMP OnPreservedKey(ITfContext* ctx, REFGUID g, BOOL* eaten) override;
    // ITfCompositionSink
    STDMETHODIMP OnCompositionTerminated(TfEditCookie ec, ITfComposition* comp) override;
    // ITfThreadFocusSink
    STDMETHODIMP OnSetThreadFocus() override { return S_OK; }
    STDMETHODIMP OnKillThreadFocus() override;
    // ITfDisplayAttributeProvider
    STDMETHODIMP EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** e) override;
    STDMETHODIMP GetDisplayAttributeInfo(REFGUID g, ITfDisplayAttributeInfo** info) override;

    // ITfCompartmentEventSink: the input mode changed from outside (the taskbar's 中/英 indicator)
    STDMETHODIMP OnChange(REFGUID g) override;
    // ITfFnConfigure: Windows Settings' 「選項」 for this keyboard
    STDMETHODIMP GetDisplayName(BSTR* name) override;
    STDMETHODIMP Show(HWND parent, LANGID, REFGUID) override;

    // used by the edit sessions
    void set_preedit(TfEditCookie ec, ITfContext* ctx, const std::wstring& text, int caret = -1);
    void commit(TfEditCookie ec, ITfContext* ctx, const std::wstring& text);
    std::string context_before_caret(TfEditCookie ec, ITfContext* ctx);
    bool secure_input(TfEditCookie ec, ITfContext* ctx);
    void after_edit(TfEditCookie ec, ITfContext* ctx);           // place and redraw the panel
private:
    ~TextService();
    aqtip::Key make_key(WPARAM w) const;
    bool keyboard_open();
    HRESULT run(ITfContext* ctx, const std::function<void(TfEditCookie)>& body, bool sync = true);
    void start_composition(TfEditCookie ec, ITfContext* ctx);
    void end_composition(TfEditCookie ec);
    void apply_attribute(TfEditCookie ec, ITfContext* ctx, ITfRange* range);
    void on_engine();
    void on_select(int index); void on_page(bool down);
    static LRESULT CALLBACK msg_proc(HWND, UINT, WPARAM, LPARAM);
    void sync_input_mode();                                          // the composer's mode -> the system's indicator
    void reload_settings();                                          // HKCU\Software\AquaType -> the composer and the panel
    void toggle_mode();                                              // the indicator's click, as a Shift tap
    void show_menu(POINT at);                                        // the indicator's right click
    ITfCompartment* compartment(REFGUID g);
    ITfCompartment* mode_compartment() { return compartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION); }
    void open_keyboard();                                            // Windows turned the input method off (Ctrl+Space): on again, in 中
    DWORD mode_sink_ = TF_INVALID_COOKIE, openclose_sink_ = TF_INVALID_COOKIE; bool setting_mode_ = false;
    InputModeButton* mode_button_ = nullptr;

    LONG refs_ = 1;
    ITfThreadMgr* tm_ = nullptr; TfClientId id_ = TF_CLIENTID_NULL; DWORD flags_ = 0;
    DWORD tm_sink_ = TF_INVALID_COOKIE, focus_sink_ = TF_INVALID_COOKIE;
    ITfComposition* comp_ = nullptr; ITfContext* comp_ctx_ = nullptr;
    TfGuidAtom attr_atom_ = TF_INVALID_GUIDATOM;
    HWND msg_hwnd_ = nullptr;
    std::unique_ptr<aqtip::PipeEngine> engine_;
    struct HostImpl; std::unique_ptr<HostImpl> host_;
    std::unique_ptr<aqtip::Composer> composer_;
    CandidateWindow panel_; RECT anchor_{}; bool have_anchor_ = false;
    ULONGLONG shift_down_at_ = 0; bool shift_held_ = false; bool other_key_with_shift_ = false;
    void shift_down(); bool shift_up(ITfContext* ctx);
    UINT settings_msg_ = 0;
};

// AquaTypeSettings.exe, next to the 64-bit text service
void open_settings_app();

}  // namespace aqtsf
