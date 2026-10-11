// The input mode button (GUID_LBI_INPUTMODE): what Windows shows on the taskbar beside the clock for a third-party input
// method - 中 or 英, drawn for the taskbar's light or dark theme - and a click on it switches the mode.
#pragma once
#include <functional>
#include "globals.h"

namespace aqtsf {

class InputModeButton : public ITfLangBarItemButton, public ITfSource {
public:
    std::function<bool()> is_ascii;              // the current mode
    std::function<void()> on_click;              // switch it
    InputModeButton();
    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    // ITfLangBarItem
    STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* info) override;
    STDMETHODIMP GetStatus(DWORD* status) override { *status = 0; return S_OK; }
    STDMETHODIMP Show(BOOL) override { return E_NOTIMPL; }
    STDMETHODIMP GetTooltipString(BSTR* tip) override;
    // ITfLangBarItemButton
    STDMETHODIMP OnClick(TfLBIClick click, POINT, const RECT*) override;
    STDMETHODIMP InitMenu(ITfMenu*) override { return S_OK; }
    STDMETHODIMP OnMenuSelect(UINT) override { return S_OK; }
    STDMETHODIMP GetIcon(HICON* icon) override;
    STDMETHODIMP GetText(BSTR* text) override;
    // ITfSource
    STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD* cookie) override;
    STDMETHODIMP UnadviseSink(DWORD cookie) override;
    void update();                               // the mode changed: redraw
private:
    ~InputModeButton();
    LONG refs_ = 1; ITfLangBarItemSink* sink_ = nullptr;
};

// icon resources (AquaTypeTSF.rc)
enum : int { IDI_APP = 1, IDI_ZH_LIGHT = 2, IDI_EN_LIGHT = 3, IDI_ZH_DARK = 4, IDI_EN_DARK = 5 };

}  // namespace aqtsf
