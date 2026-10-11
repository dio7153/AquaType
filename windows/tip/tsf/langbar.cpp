#include "langbar.h"
#include <ctfutb.h>
#include <olectl.h>

namespace aqtsf {
namespace {
// GUID_LBI_INPUTMODE: the item Windows shows as the input mode on the taskbar (as SampleIME defines it)
constexpr GUID kLbiInputMode = {0x2c77a81e, 0x41cc, 0x4178, {0xa3, 0xa7, 0x5f, 0x8a, 0x98, 0x75, 0x68, 0xe6}};
constexpr DWORD kSinkCookie = 1;
// the taskbar's theme, which is the system theme (not the apps' theme)
bool taskbar_dark() {
    DWORD v = 1, n = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n) == ERROR_SUCCESS && v == 0;
}
}  // namespace

InputModeButton::InputModeButton() { lock_module(); }
InputModeButton::~InputModeButton() { if (sink_) sink_->Release(); unlock_module(); }

STDMETHODIMP InputModeButton::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    if (riid == IID_IUnknown || riid == IID_ITfLangBarItem || riid == IID_ITfLangBarItemButton) *ppv = static_cast<ITfLangBarItemButton*>(this);
    else if (riid == IID_ITfSource) *ppv = static_cast<ITfSource*>(this);
    else { *ppv = nullptr; return E_NOINTERFACE; }
    AddRef(); return S_OK;
}
STDMETHODIMP_(ULONG) InputModeButton::AddRef() { return InterlockedIncrement(&refs_); }
STDMETHODIMP_(ULONG) InputModeButton::Release() { LONG r = InterlockedDecrement(&refs_); if (!r) delete this; return r; }

STDMETHODIMP InputModeButton::GetInfo(TF_LANGBARITEMINFO* info) {
    info->clsidService = CLSID_AquaType; info->guidItem = kLbiInputMode;
    info->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY; info->ulSort = 0;
    wcscpy_s(info->szDescription, L"AquaType 中／英");
    return S_OK;
}
STDMETHODIMP InputModeButton::GetTooltipString(BSTR* tip) {
    const bool ascii = is_ascii && is_ascii();
    *tip = SysAllocString(ascii ? L"英文模式（單按 Shift 切回中文）" : L"中文模式（單按 Shift 切到英文）");
    return *tip ? S_OK : E_OUTOFMEMORY;
}
STDMETHODIMP InputModeButton::OnClick(TfLBIClick click, POINT, const RECT*) { if (click == TF_LBI_CLK_LEFT && on_click) on_click(); return S_OK; }
STDMETHODIMP InputModeButton::GetIcon(HICON* icon) {
    const bool ascii = is_ascii && is_ascii(), dark = taskbar_dark();
    const int id = ascii ? (dark ? IDI_EN_DARK : IDI_EN_LIGHT) : (dark ? IDI_ZH_DARK : IDI_ZH_LIGHT);
    const int size = GetSystemMetrics(SM_CXSMICON);
    *icon = (HICON)LoadImageW(g_module, MAKEINTRESOURCEW(id), IMAGE_ICON, size, size, LR_DEFAULTCOLOR);   // the caller destroys it
    return *icon ? S_OK : E_FAIL;
}
STDMETHODIMP InputModeButton::GetText(BSTR* text) { *text = SysAllocString((is_ascii && is_ascii()) ? L"英" : L"中"); return *text ? S_OK : E_OUTOFMEMORY; }

STDMETHODIMP InputModeButton::AdviseSink(REFIID riid, IUnknown* punk, DWORD* cookie) {
    if (riid != IID_ITfLangBarItemSink) return CONNECT_E_CANNOTCONNECT;
    if (sink_) return CONNECT_E_ADVISELIMIT;
    if (FAILED(punk->QueryInterface(IID_ITfLangBarItemSink, (void**)&sink_))) { sink_ = nullptr; return E_NOINTERFACE; }
    *cookie = kSinkCookie; return S_OK;
}
STDMETHODIMP InputModeButton::UnadviseSink(DWORD cookie) { if (cookie != kSinkCookie || !sink_) return CONNECT_E_NOCONNECTION; sink_->Release(); sink_ = nullptr; return S_OK; }
void InputModeButton::update() { if (sink_) sink_->OnUpdate(TF_LBI_ICON | TF_LBI_TEXT | TF_LBI_TOOLTIP); }

}  // namespace aqtsf
