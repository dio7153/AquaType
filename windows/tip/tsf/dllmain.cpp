// DLL entry points: the class factory and registration (COM server + TSF profile + categories).
#include "globals.h"
#include "tip.h"
#include <string>

namespace aqtsf {
HINSTANCE g_module = nullptr;
std::atomic<long> g_locks{0};

class ClassFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = static_cast<IClassFactory*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { lock_module(); return 2; }
    STDMETHODIMP_(ULONG) Release() override { unlock_module(); return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        TextService* t = new (std::nothrow) TextService(); if (!t) return E_OUTOFMEMORY;
        HRESULT hr = t->QueryInterface(riid, ppv); t->Release(); return hr;
    }
    STDMETHODIMP LockServer(BOOL lock) override { if (lock) lock_module(); else unlock_module(); return S_OK; }
};
static ClassFactory g_factory;

static std::wstring guid_string(REFGUID g) { wchar_t s[64]; StringFromGUID2(g, s, 64); return s; }
static std::wstring module_path() { wchar_t p[MAX_PATH]; GetModuleFileNameW(g_module, p, MAX_PATH); return p; }

static HRESULT register_server() {
    const std::wstring key = L"Software\\Classes\\CLSID\\" + guid_string(CLSID_AquaType);
    HKEY k; if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return E_ACCESSDENIED;
    RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)kDescription, sizeof kDescription); RegCloseKey(k);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, (key + L"\\InprocServer32").c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return E_ACCESSDENIED;
    const std::wstring path = module_path(); static const wchar_t model[] = L"Apartment";
    RegSetValueExW(k, nullptr, 0, REG_SZ, (const BYTE*)path.c_str(), (DWORD)((path.size() + 1) * sizeof(wchar_t)));
    RegSetValueExW(k, L"ThreadingModel", 0, REG_SZ, (const BYTE*)model, sizeof model); RegCloseKey(k);
    return S_OK;
}
static void unregister_server() { RegDeleteTreeW(HKEY_LOCAL_MACHINE, (L"Software\\Classes\\CLSID\\" + guid_string(CLSID_AquaType)).c_str()); }

// Only what the service implements: no COM-less activation, no UI elements (the candidate window is drawn by the service itself).
static const GUID kCategories[] = {GUID_TFCAT_TIP_KEYBOARD, GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER, GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT, GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT, GUID_TFCAT_TIPCAP_SECUREMODE,
                                   GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT};

static HRESULT register_profile() {
    ITfInputProcessorProfileMgr* mgr = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfileMgr, (void**)&mgr);
    if (FAILED(hr)) return hr;
    const std::wstring icon = module_path();
    hr = mgr->RegisterProfile(CLSID_AquaType, kLangId, GUID_Profile, kDescription, (ULONG)wcslen(kDescription), icon.c_str(), (ULONG)icon.size(), 0, nullptr, 0, TRUE, 0);
    mgr->Release(); if (FAILED(hr)) return hr;
    ITfCategoryMgr* cat = nullptr;
    hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr, (void**)&cat);
    if (FAILED(hr)) return hr;
    for (const GUID& g : kCategories) cat->RegisterCategory(CLSID_AquaType, g, CLSID_AquaType);
    cat->Release(); return S_OK;
}
static void unregister_profile() {
    ITfInputProcessorProfileMgr* mgr = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfileMgr, (void**)&mgr))) { mgr->UnregisterProfile(CLSID_AquaType, kLangId, GUID_Profile, 0); mgr->Release(); }
    ITfCategoryMgr* cat = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr, (void**)&cat))) { for (const GUID& g : kCategories) cat->UnregisterCategory(CLSID_AquaType, g, CLSID_AquaType); cat->Release(); }
}
}  // namespace aqtsf

using namespace aqtsf;

BOOL APIENTRY DllMain(HINSTANCE h, DWORD reason, LPVOID) { if (reason == DLL_PROCESS_ATTACH) { g_module = h; DisableThreadLibraryCalls(h); } return TRUE; }
STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != CLSID_AquaType) { *ppv = nullptr; return CLASS_E_CLASSNOTAVAILABLE; }
    return g_factory.QueryInterface(riid, ppv);
}
STDAPI DllCanUnloadNow() { return g_locks.load() == 0 ? S_OK : S_FALSE; }
STDAPI DllRegisterServer() {
    HRESULT hr = register_server(); if (FAILED(hr)) return hr;
    hr = register_profile(); if (FAILED(hr)) { unregister_server(); return hr; }
    return S_OK;
}
STDAPI DllUnregisterServer() { unregister_profile(); unregister_server(); return S_OK; }
